/* SPEC §12.1 WiFi AP transport: soft-AP, HTTP server, one websocket endpoint. */
#include "net_transport.h"

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "board/board_profile.h"
#include "config_store_nvs.h"
#include "orch_demo.h"
#include "esp_timer.h"
#include "build_mode.h"
#include "firmware_version.h"
#include "laser_bench.h"
#include "truing/limits.h"
#include "truing_proto/json.h"
#include "truing_proto/wire.h"
#include "web_ui.h"

/* Injected by tools/build_identity.py; the fallbacks keep an ad-hoc compile working. */
#ifndef TRUING_BUILD_REV
#define TRUING_BUILD_REV "unknown"
#endif
#ifndef TRUING_UI_HASH
#define TRUING_UI_HASH "unknown"
#endif

static const char *TAG = "net";

/* Deliberately small. A deep outbound queue would only let a dead client accumulate
 * stale telemetry that is worthless by the time it drains; dropping early is what
 * SPEC §12.2 asks for, and the current-state query is how a client resynchronises. */
#define OUTBOUND_SLOTS      12
#define INBOUND_SLOTS        4
#define MAX_WS_CLIENTS       4
/* Every socket the server may hold open, websocket or not: the page, /id, the websocket and
 * whatever is still in keep-alive. httpd_get_client_list() enumerates ALL of them and fails
 * outright - not partially - if the caller's array cannot hold every one, so this is the size
 * that array must be. Sizing it by MAX_WS_CLIENTS instead made the enumeration fail as soon as
 * a fifth socket existed, which the server is configured to allow, and a failed enumeration
 * reads as "no clients": every telemetry frame and every command reply was then dropped by
 * sink_send() while the page sat there connected. */
/* +3, not +1: a client is not one socket. A browser holding the websocket also fetches the
 * page and /id, and those linger in keep-alive, so a single tab can hold three. With only one
 * spare slot the server evicted a live websocket every time anything made an HTTP request,
 * which is a page that goes dead for no visible reason. */
#define MAX_OPEN_SOCKETS    (MAX_WS_CLIENTS + 3)
#define HTTPD_STACK_BYTES   8192   /* the httpd task's stack; the ESP-IDF default (4096) overflowed, see start */
#define SENDER_STACK      4096
#define AP_CHANNEL           1

typedef struct {
    char  *frame;    /* heap; the sender frees it */
    size_t len;
    int    fd;       /* >=0: reply to that client only. -1: broadcast. */
} out_item_t;

typedef struct {
    uint16_t len;
    int      fd;     /* the socket the command arrived on, so its reply can go back */
    char     data[TRUING_WIRE_COMMAND_MAX + 1u];
} in_item_t;

static httpd_handle_t   s_server;
static QueueHandle_t    s_outbound;
static QueueHandle_t    s_inbound;
/* The socket whose command is being processed right now, or -1 when nothing is.
 * Everything the session emits while dispatching a command is a REPLY to that command
 * -- its ack, and the snapshot answering a state query -- and belongs to the client that
 * asked. Telemetry emitted outside a dispatch has no originator and is broadcast. Safe
 * as a plain static: poll_inbound and the session both run on the orchestrator task. */
static int              s_reply_fd = -1;
static truing_wire_sink_t s_sink;
static truing_net_stats_t s_stats;
static bool             s_started;

/* ---- websocket plumbing ----------------------------------------------------------- */

/* Active websocket descriptors, asked of the server rather than tracked by hand: the
 * server already knows when a socket closes, and duplicating that is how stale
 * descriptors get written to. */
static size_t ws_clients(int *fds, size_t cap)
{
    if (s_server == NULL) {
        return 0u;
    }
    int all[MAX_OPEN_SOCKETS];
    size_t n = MAX_OPEN_SOCKETS;
    if (httpd_get_client_list(s_server, &n, all) != ESP_OK) {
        /* Not silently: reporting zero clients here disables the whole outbound path, and
         * that is indistinguishable from nobody being connected. */
        static uint32_t s_enum_fail;
        if ((s_enum_fail++ % 64u) == 0u) {
            ESP_LOGW(TAG, "client enumeration failed (%" PRIu32 " times); telemetry is being dropped", s_enum_fail);
        }
        return 0u;
    }
    size_t found = 0u;
    for (size_t i = 0u; i < n && found < cap; ++i) {
        if (httpd_ws_get_fd_info(s_server, all[i]) == HTTPD_WS_CLIENT_WEBSOCKET) {
            fds[found++] = all[i];
        }
    }
    return found;
}

bool truing_net_client_connected(void)
{
    int fds[MAX_WS_CLIENTS];
    return ws_clients(fds, MAX_WS_CLIENTS) > 0u;
}

/* The only place that performs a blocking socket write. Runs on its own task so the
 * orchestrator never waits on the network (SPEC §12.2). */
static void sender_task(void *arg)
{
    (void)arg;
    for (;;) {
        out_item_t item;
        if (xQueueReceive(s_outbound, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        int fds[MAX_WS_CLIENTS];
        size_t n = ws_clients(fds, MAX_WS_CLIENTS);
        if (item.fd >= 0) {
            /* A reply goes to the one client that asked. Still checked against the live
             * client list: the socket may have closed between the command and the ack. */
            size_t keep = 0u;
            for (size_t i = 0u; i < n; ++i) {
                if (fds[i] == item.fd) {
                    fds[0] = item.fd;
                    keep = 1u;
                    break;
                }
            }
            n = keep;
        }
        for (size_t i = 0u; i < n; ++i) {
            httpd_ws_frame_t f;
            memset(&f, 0, sizeof(f));
            f.type = HTTPD_WS_TYPE_TEXT;
            f.payload = (uint8_t *)item.frame;
            f.len = item.len;
            if (httpd_ws_send_frame_async(s_server, fds[i], &f) == ESP_OK) {
                s_stats.frames_sent++;
            } else {
                s_stats.frames_dropped++;
            }
        }
        free(item.frame);
    }
}

static bool sink_send(truing_wire_sink_t *self, const char *frame, size_t len)
{
    (void)self;
    if (s_outbound == NULL || len == 0u) {
        return false;
    }
    /* No client, no copy: this is the common case while the machine works alone, and
     * SPEC §12.2 is explicit that autonomous operations do not depend on the host. */
    if (!truing_net_client_connected()) {
        s_stats.frames_dropped++;
        return false;
    }
    out_item_t item;
    item.frame = malloc(len + 1u);
    if (item.frame == NULL) {
        s_stats.frames_dropped++;
        return false;
    }
    memcpy(item.frame, frame, len);
    item.frame[len] = '\0';
    item.len = len;
    item.fd = s_reply_fd;   /* set only while a command is being dispatched */
    /* Zero ticks: enqueueing must never wait on the control path. */
    if (xQueueSend(s_outbound, &item, 0) != pdTRUE) {
        free(item.frame);
        s_stats.frames_dropped++;
        return false;
    }
    return true;
}

truing_wire_sink_t *truing_net_sink(void)
{
    s_sink.impl_name = "wifi_ap_ws";
    s_sink.send = sink_send;
    s_sink.ctx = NULL;
    return &s_sink;
}

size_t truing_net_poll_inbound(char *out, size_t cap)
{
    if (s_inbound == NULL || out == NULL || cap == 0u) {
        return 0u;
    }
    static in_item_t item;   /* static: 513 bytes is too much for a caller's stack budget */
    if (xQueueReceive(s_inbound, &item, 0) != pdTRUE) {
        s_reply_fd = -1;     /* nothing in flight: anything emitted now is telemetry */
        return 0u;
    }
    /* Held across the caller's handling of this frame, which is where the ack is emitted. */
    s_reply_fd = item.fd;
    if ((size_t)item.len + 1u > cap) {
        return 0u;
    }
    memcpy(out, item.data, item.len);
    out[item.len] = '\0';
    return item.len;
}

void truing_net_get_stats(truing_net_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    *out = s_stats;
    int fds[MAX_WS_CLIENTS];
    out->clients = (uint32_t)ws_clients(fds, MAX_WS_CLIENTS);
}

bool truing_net_started(void)
{
    return s_started;
}

/* ---- SPEC §9.4 RF load generator ---------------------------------------------------- */

static volatile bool     s_rf_stop;
static volatile uint32_t s_rf_frames;
static volatile uint32_t s_rf_failures;
static TaskHandle_t      s_rf_task;

/* A minimal 802.11 action frame from this AP to itself: enough of a header for the
 * driver to accept, and enough body to occupy real airtime. */
#define RF_FRAME_BYTES 220

static void rf_load_task(void *arg)
{
    (void)arg;
    static uint8_t frame[RF_FRAME_BYTES];
    uint8_t mac[6] = { 0 };
    (void)esp_wifi_get_mac(WIFI_IF_AP, mac);
    memset(frame, 0, sizeof(frame));
    frame[0] = 0xD0u;                      /* type/subtype: management, action */
    memcpy(&frame[4], mac, 6);             /* addr1 (destination): this board */
    memcpy(&frame[10], mac, 6);            /* addr2 (source) */
    memcpy(&frame[16], mac, 6);            /* addr3 (BSSID) */
    frame[24] = 0x7Fu;                     /* action category: vendor specific */
    for (size_t i = 25u; i < sizeof(frame); ++i) {
        frame[i] = (uint8_t)i;
    }
    while (!s_rf_stop) {
        /* en_sys_seq: let the driver own the sequence numbers. Managing them here made it
         * reject the great majority of frames. */
        if (esp_wifi_80211_tx(WIFI_IF_AP, frame, sizeof(frame), true) == ESP_OK) {
            s_rf_frames++;
            taskYIELD();          /* keep the radio fed without starving its own tasks */
        } else {
            s_rf_failures++;
            vTaskDelay(1);        /* TX buffers are full: back off rather than spin */
        }
    }
    s_rf_task = NULL;
    vTaskDelete(NULL);
}

bool truing_net_rf_load_start(void)
{
    if (!s_started || s_rf_task != NULL) {
        return false;
    }
    s_rf_stop = false;
    s_rf_frames = 0u;
    s_rf_failures = 0u;
    /* Core 0: the radio's own core (SPEC §4.5 keeps core 1 for audio), which is the
     * arrangement the capture has to survive. */
    if (xTaskCreatePinnedToCore(rf_load_task, "rf_load", 3072, NULL, tskIDLE_PRIORITY + 2, &s_rf_task, 0) != pdPASS) {
        s_rf_task = NULL;
        return false;
    }
    return true;
}

void truing_net_rf_load_stop(uint32_t *frames_injected, uint32_t *inject_failures)
{
    s_rf_stop = true;
    for (int i = 0; i < 50 && s_rf_task != NULL; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (frames_injected != NULL) {
        *frames_injected = s_rf_frames;
    }
    if (inject_failures != NULL) {
        *inject_failures = s_rf_failures;
    }
}

/* ---- handlers --------------------------------------------------------------------- */

/* Two boards on this bench serve this page on the same 192.168.4.1, so a cached copy is
 * indistinguishable from the other board's copy, and both failure modes look like "the old
 * UI came back". The page is 21 KB off local flash over a link with no other traffic;
 * caching it buys nothing and costs the one property that matters here, which is that what
 * you are looking at is what the board is actually running. */
static void no_store(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, must-revalidate");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
}

static esp_err_t ui_get_handler(httpd_req_t *req)
{
    no_store(req);
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, TRUING_WEB_UI_HTML, HTTPD_RESP_USE_STRLEN);
}

/* Which board, which build, which UI. The UI hash is sha256(src/web_ui.h) truncated by the
 * build script, so the page can prove it is the page that was compiled in rather than a
 * copy the browser kept or the other board's. `clients` is the live websocket client count:
 * awareness only — the SPEC §12.3 wait_id contract is what keeps two clients safe, and no
 * ownership or locking is offered or implied. */
#if TRUING_FAST_DEMO
/* Choose the acquisition path for the NEXT session: GET /demo/acquisition?mode=auto|manual.
 *
 * Deliberately NOT an operator intent. It answers no wait, carries no wait_id and cannot
 * reach a running session - truing_demo_request_acquisition() refuses while one is active,
 * and the change is applied by the orchestrator's own task between steps. So the SPEC §12.3
 * contract is untouched and there is nothing here that could alter a session in flight.
 *
 * It exists only in this image. The interactive firmware is not hardened against this URL;
 * it simply does not serve it, and its truing_demo_request_acquisition() refuses outright. */
static esp_err_t acq_post_handler(httpd_req_t *req)
{
    char query[64];
    char mode[16] = { 0 };
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        (void)httpd_query_key_value(query, "mode", mode, sizeof(mode));
    }
    const bool want_auto = strcmp(mode, "auto") == 0;
    const bool want_manual = strcmp(mode, "manual") == 0;
    no_store(req);
    httpd_resp_set_type(req, "application/json");
    if (!want_auto && !want_manual) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"detail\":\"mode must be auto or manual\"}");
    }
    const char *detail = "";
    char body[192];
    if (!truing_demo_request_acquisition(want_auto, &detail)) {
        httpd_resp_set_status(req, "409 Conflict");
        const int m = snprintf(body, sizeof(body), "{\"ok\":false,\"detail\":\"%s\"}", detail);
        return httpd_resp_send(req, body, m > 0 ? (size_t)m : 0u);
    }
    ESP_LOGW(TAG, "acquisition path requested: %s", want_auto ? "AUTOMATIC (synthetic)" : "MANUAL (operator)");
    const int m = snprintf(body, sizeof(body), "{\"ok\":true,\"requested\":\"%s\"}",
                           want_auto ? "auto" : "manual");
    return httpd_resp_send(req, body, m > 0 ? (size_t)m : 0u);
}
#endif

/* ---- SPEC §12.5 debug channel: dump internals -------------------------------------------
 *
 * GET /debug/capture.json   what the last acoustic measurement was and what it concluded
 * GET /debug/capture.pcm    the samples it concluded that from, exactly as captured
 *
 * A bad pluck is unrepeatable - the next one is a different pluck - so the only way to work
 * on one is to keep its samples. These two hand them over; tools/capture_fetch.py turns them
 * into a fixture and test_acoustic_replay pushes that fixture back through the same layers on
 * a host. Nothing here is on any control path: no measurement waits on it, no result depends
 * on whether anyone ever asks, and a session that is never dumped behaves identically
 * (SPEC §13.3).
 *
 * The buffer being read is the live one, so both responses carry X-Truing-Capture-Seq. The
 * client reads the metadata, the samples and the metadata again, and keeps the bundle only if
 * all three agree. A dump that loses a race is discarded rather than silently half-and-half. */
static void capture_seq_header(httpd_req_t *req, uint32_t seq)
{
    /* httpd_resp_set_hdr stores the POINTER, not the string: the value must outlive the
     * response. A stack buffer here produced a header pointing into freed stack that the JSON
     * body then wrote over, so the client received the body as the header value. Static is
     * safe because the server runs one task and finishes a response before starting the next. */
    static char v[16];
    (void)snprintf(v, sizeof(v), "%" PRIu32, seq);
    (void)httpd_resp_set_hdr(req, "X-Truing-Capture-Seq", v);
}

static esp_err_t capture_meta_handler(httpd_req_t *req)
{
    truing_acoustic_capture_view_t v;
    no_store(req);
    httpd_resp_set_type(req, "application/json");
    /* Checked before anything else: an attempt can be mid-flight (words landing, outcome not
     * yet settled), which is a different state from nothing-ever-captured. 409, not a 404 or a
     * half-finished 200 (SPEC §12.5/§13.3). */
    if (truing_demo_capture_pending()) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"detail\":\"capture outcome not yet settled\"}");
    }
    if (!truing_demo_last_capture(&v)) {
        httpd_resp_set_status(req, "404 Not Found");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"detail\":\"nothing captured yet\"}");
    }
    capture_seq_header(req, v.seq);

    uint8_t digest[TRUING_SHA256_DIGEST_BYTES];
    truing_chain_profile_digest(truing_demo_chain_profile(), digest);
    uint8_t exc_digest[TRUING_SHA256_DIGEST_BYTES];
    truing_excitation_profile_digest(truing_demo_excitation_profile(), exc_digest);

    /* The writer, not snprintf: a NaN has no JSON form and it writes null, which is what an
     * unmeasured frequency actually means. Hand-formatting these has produced `nan` in a
     * JSON document before. */
    /* 1024 was already tight before the five audio-diagnostic fields below (SPEC §12.5); sized
     * up rather than risking truing_json_finish() failing the response on a long build/UI hash. */
    char body[1536];
    truing_json_writer_t w;
    truing_json_init(&w, body, sizeof(body));
    truing_json_obj_open(&w, NULL);
    truing_json_str(&w, "schema", "truing.acoustic.capture/2");
    truing_json_u32(&w, "seq", v.seq);
    truing_json_u32(&w, "n_words", v.n_words);
    truing_json_u32(&w, "sample_rate_hz", TRUING_AUDIO_SAMPLE_RATE_HZ);
    truing_json_str(&w, "pcm_format", "int32 LE, sample24 << 8 (I2S 24-in-32)");
    truing_json_hex(&w, "chain_digest", digest, sizeof(digest));
    truing_json_hex(&w, "excitation_digest", exc_digest, sizeof(exc_digest));
    /* Provenance: which board, which build, and above all whether these words came off a
     * microphone or out of the synthetic source (SPEC §6.2). */
    truing_json_str(&w, "source", truing_source_impl_str(truing_demo_acoustic_source()));
    /* Unconditional, never gated like the optional audio-diagnostic block below: a safety
     * discriminator so campaign tooling, or any future report generator reading this document,
     * can never mistake a bench-triggered MEASURE_ONCE capture (SPEC §12.5) for real session
     * evidence by its absence. */
    truing_json_bool(&w, "debug_triggered", v.diag.debug_triggered);
    truing_json_str(&w, "board", BOARD_NAME);
    truing_json_str(&w, "build", TRUING_BUILD_REV);
    truing_json_str(&w, "ui", TRUING_UI_HASH);
    truing_json_str(&w, "mode", TRUING_BUILD_MODE_STR);
    truing_json_u32(&w, "uptime_s", (uint32_t)(esp_timer_get_time() / 1000000));
    /* Which measurement this is evidence of, and what it concluded. */
    truing_json_u32(&w, "spoke_id", v.spoke_id);
    truing_json_u32(&w, "cycle_index", v.cycle_index);
    truing_json_u32(&w, "attempt", v.attempt);
    truing_json_str(&w, "status", truing_status_str(v.status));
    truing_json_str(&w, "reason", truing_reason_str(v.reason));
    /* Which station's actuator excited these words, and with what pulse (plan A7). */
    truing_json_str(&w, "station", truing_station_str((truing_station_id_t)v.diag.station));
    truing_json_bool(&w, "fired", v.diag.fired);
    truing_json_f32(&w, "pulse_ms", v.diag.pulse_ms, 3u);
    truing_json_str(&w, "capture_result", truing_audio_result_str(v.diag.capture_result));
    /* The board's own diagnostics, written with the expect_ prefix the replay harness reads:
     * a fixture built from this is a host/target parity check on identical bytes. */
    truing_json_str(&w, "expect_origin", "board");
    truing_json_str(&w, "expect_status", truing_status_str(v.status));
    truing_json_str(&w, "expect_reason", truing_reason_str(v.reason));
    truing_json_u32(&w, "expect_onsets", v.diag.onsets);
    truing_json_u32(&w, "expect_onset_sample", v.diag.onset_sample);
    truing_json_u32(&w, "expect_window_start_sample", v.diag.window.start_sample);
    truing_json_u32(&w, "expect_window_n_samples", v.diag.window.n_samples);
    truing_json_str(&w, "expect_window_truncated_by", truing_window_truncation_str(v.diag.window.truncated_by));
    truing_json_u32(&w, "expect_n_fft", v.diag.n_fft);
    truing_json_u32(&w, "expect_n_strong_peaks", v.diag.n_strong_peaks);
    truing_json_u32(&w, "expect_n_peaks_in_band", v.diag.n_peaks_in_band);
    truing_json_f32(&w, "expect_f1_hz", v.diag.f1_hz, 6u);
    truing_json_f32(&w, "expect_snr_db", v.diag.snr_db, 6u);
    truing_json_f32(&w, "f2_hz", v.diag.f2_hz, 6u);
    truing_json_f32(&w, "l_eff_m", v.diag.l_eff_m, 6u);
    truing_json_f32(&w, "onset_threshold", v.diag.onset_threshold, 8u);
    truing_json_u32(&w, "capture_us", v.diag.capture_us);
    truing_json_u32(&w, "analysis_us", v.diag.analysis_us);
    /* The front end's own per-capture diagnostics (audio_source_if.h capture_report), when it
     * reported any -- additive detail alongside capture_result, never a new decision. */
    if (v.diag.audio_report) {
        truing_json_u32(&w, "pre_roll_words_delivered", v.diag.pre_roll_words_delivered);
        truing_json_u32(&w, "pre_roll_words_configured", v.diag.pre_roll_words_configured);
        truing_json_u32(&w, "capture_overrun_events", v.diag.capture_overrun_events);
        truing_json_u32(&w, "ring_age_us", v.diag.ring_age_us);
        truing_json_u32(&w, "worst_read_gap_us", v.diag.worst_read_gap_us);
        /* Dual-mic builds only: which microphone these words are, and the other one's record
         * (/debug/capture_far.pcm). The words above are still the ONLY input the DSP saw. */
        if (v.diag.n_inputs >= 2u) {
            truing_json_u32(&w, "mic_inputs", v.diag.n_inputs);
            truing_json_str(&w, "mic_input", v.diag.input_analysed == 0u ? "left_slot" : "right_slot");
            const int32_t *far_words = NULL;
            uint32_t far_n = 0u;
            uint8_t far_input = 0u;
            if (truing_demo_far_capture(&far_words, &far_n, &far_input)) {
                truing_json_str(&w, "far_input", far_input == 0u ? "left_slot" : "right_slot");
                truing_json_u32(&w, "far_n_words", far_n);
            }
        }
    }
    truing_json_obj_close(&w);
    size_t len = 0u;
    if (!truing_json_finish(&w, &len)) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"detail\":\"capture metadata did not fit\"}");
    }
    const esp_err_t sent = httpd_resp_send(req, body, len);
    ESP_LOGI(TAG, "capture.json served: httpd stack high-water %u of %u bytes free",
             (unsigned)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t)), (unsigned)HTTPD_STACK_BYTES);
    return sent;
}

/* Chunked, because a capture is a couple of hundred kilobytes and the HTTP task's stack is not.
 * Between chunks the measuring task may start another capture into this very buffer; when it
 * does, the transfer is abandoned mid-stream so the client sees a broken response rather than
 * two halves of different plucks spliced together. */
static esp_err_t stream_capture_words(httpd_req_t *req, const int32_t *words, uint32_t n_words, uint32_t seq)
{
    capture_seq_header(req, seq);
    httpd_resp_set_type(req, "application/octet-stream");
    const uint8_t *p = (const uint8_t *)words;
    size_t remaining = (size_t)n_words * sizeof(int32_t);
    while (remaining > 0u) {
        const size_t chunk = remaining > 4096u ? 4096u : remaining;
        if (truing_demo_capture_seq() != seq) {
            ESP_LOGW(TAG, "capture dump abandoned: a new measurement overwrote the buffer");
            return ESP_FAIL;
        }
        if (httpd_resp_send_chunk(req, (const char *)p, chunk) != ESP_OK) {
            return ESP_FAIL;
        }
        p += chunk;
        remaining -= chunk;
    }
    if (truing_demo_capture_seq() != seq) {
        ESP_LOGW(TAG, "capture dump abandoned at the end: the buffer changed during the transfer");
        return ESP_FAIL;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t capture_pcm_handler(httpd_req_t *req)
{
    truing_acoustic_capture_view_t v;
    no_store(req);
    /* Same gate as capture_meta_handler, checked first: an attempt mid-flight is a different
     * state from nothing-ever-captured, and streaming its half-written words would be exactly
     * the half-finished read this seam exists to refuse (SPEC §12.5/§13.3). */
    if (truing_demo_capture_pending()) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"detail\":\"capture outcome not yet settled\"}");
    }
    if (!truing_demo_last_capture(&v)) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"detail\":\"nothing captured yet\"}");
    }
    return stream_capture_words(req, v.words, v.n_words, v.seq);
}

#if TRUING_DUAL_MIC
/* GET /debug/capture_far.pcm: the other microphone over the same frames as /debug/capture.pcm,
 * same format and the same seq guard. Never analysed on the board -- the workbench's reference. */
static esp_err_t capture_far_pcm_handler(httpd_req_t *req)
{
    truing_acoustic_capture_view_t v;
    no_store(req);
    if (truing_demo_capture_pending()) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"detail\":\"capture outcome not yet settled\"}");
    }
    const int32_t *far_words = NULL;
    uint32_t far_n = 0u;
    uint8_t far_input = 0u;
    if (!truing_demo_last_capture(&v) || !truing_demo_far_capture(&far_words, &far_n, &far_input)) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"detail\":\"no far-mic record for the last capture\"}");
    }
    return stream_capture_words(req, far_words, far_n, v.seq);
}
#endif

#if TRUING_CAMPAIGN_DEBUG && TRUING_REAL_FRONT_END
#define LISTEN_MAX_SECONDS 30u
/* GET /debug/listen.pcm?s=<seconds>|ms=<milliseconds>&station=<0|1|2>: a microphone live, as raw capture
 * words (little-endian int32, 48 kHz) -- what the workbench's `listen` turns into one long listening wav.
 * s is 1..30, or ms is 100..30000. station 0/1 is that station's mic; station 2 is BOTH, interleaved
 * (station 0's word, then station 1's, per frame), which is what the heartbeat uses: a fraction of a
 * second of both mics with none of the acoustic analysis a capture drags along. A capture is bounded to a
 * 1.2 s window by the acoustic chain; this follows the ring instead, so it is as long as asked.
 * Read-only on the ring, so it cannot disturb a measurement, but it occupies the server for its whole
 * length: nothing else is served meanwhile, and a second listener is refused. Samples the reader could
 * not keep up with are counted, not hidden: the response ends early rather than splicing across a gap. */
static esp_err_t listen_pcm_handler(httpd_req_t *req)
{
    static bool busy;
    static int32_t a[512], b[512], out[1024];
    char q[112], v[12];
    uint32_t ms = 5000u, station = 0u;
    /* An optional bench burst fired while the stream runs, to watch the microphones through it:
     * fn shots (0 = none, max 40) of fp ms (5..60) with fg ms between shot starts, on station fs (0 LEFT,
     * 1 RIGHT, 2 alternating), the first after a 400 ms lead-in. The gap is held to at least 4 x the pulse
     * and 100 ms, so a coil is never asked for more than a 25 % duty. */
    uint32_t fire_n = 0u, fire_ms = 20u, fire_gap = 250u, fire_st = 2u;
    no_store(req);
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        if (httpd_query_key_value(q, "s", v, sizeof(v)) == ESP_OK) ms = (uint32_t)strtoul(v, NULL, 10) * 1000u;
        if (httpd_query_key_value(q, "ms", v, sizeof(v)) == ESP_OK) ms = (uint32_t)strtoul(v, NULL, 10);
        if (httpd_query_key_value(q, "station", v, sizeof(v)) == ESP_OK) station = (uint32_t)strtoul(v, NULL, 10);
        if (httpd_query_key_value(q, "fn", v, sizeof(v)) == ESP_OK) fire_n = (uint32_t)strtoul(v, NULL, 10);
        if (httpd_query_key_value(q, "fp", v, sizeof(v)) == ESP_OK) fire_ms = (uint32_t)strtoul(v, NULL, 10);
        if (httpd_query_key_value(q, "fg", v, sizeof(v)) == ESP_OK) fire_gap = (uint32_t)strtoul(v, NULL, 10);
        if (httpd_query_key_value(q, "fs", v, sizeof(v)) == ESP_OK) fire_st = (uint32_t)strtoul(v, NULL, 10);
    }
    if (ms < 100u || ms > LISTEN_MAX_SECONDS * 1000u || station > 2u || fire_n > 40u || fire_st > 2u ||
        (fire_n != 0u && (fire_ms < 5u || fire_ms > 60u || fire_gap < 4u * fire_ms || fire_gap < 100u || fire_gap > 2000u))) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"detail\":\"s 1..30 or ms 100..30000, station 0-2; burst fn<=40 fp 5..60 fg>=4*fp,>=100\"}");
    }
    if (fire_n != 0u && ((fire_st != 1u && !truing_demo_fire_ready(0u)) || (fire_st != 0u && !truing_demo_fire_ready(1u)))) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"detail\":\"no solenoid enabled on that station in this image (PRESENT=0)\"}");
    }
    uint32_t pos = 0u, lost = 0u;
    if (busy || !truing_demo_listen_start(&pos)) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, busy ? "{\"ok\":false,\"detail\":\"already listening\"}"
                                            : "{\"ok\":false,\"detail\":\"no live front end\"}");
    }
    uint32_t pos_b = pos;   /* station 2 reads the second mic's ring from the same instant */
    busy = true;
    httpd_resp_set_type(req, "application/octet-stream");
    const uint32_t want = ms * (TRUING_AUDIO_SAMPLE_RATE_HZ / 1000u);   /* frames */
    const int64_t deadline_us = esp_timer_get_time() + ((int64_t)ms + 3000) * 1000;
    uint32_t sent = 0u, fired = 0u, refused = 0u;
    int64_t next_fire_us = esp_timer_get_time() + 400000;
    esp_err_t rc = ESP_OK;
    while (sent < want && rc == ESP_OK && lost == 0u && esp_timer_get_time() < deadline_us) {
        if (fired < fire_n && esp_timer_get_time() >= next_fire_us) {
            /* Blocks for the pulse; the ring keeps filling meanwhile (a second deep), so nothing is lost. */
            if (!truing_demo_fire(fire_st < 2u ? fire_st : (fired & 1u), (float)fire_ms)) refused++;
            fired++;
            next_fire_us += (int64_t)fire_gap * 1000;
        }
        const uint32_t room = want - sent;
        const uint32_t take = room < 512u ? room : 512u;
        uint32_t n;
        if (station < 2u) {
            n = truing_demo_listen_read(station, &pos, a, take, &lost);
            if (n != 0u) rc = httpd_resp_send_chunk(req, (const char *)a, (ssize_t)(n * sizeof(int32_t)));
        } else {
            n = truing_demo_listen_read(0u, &pos, a, take, &lost);
            if (n != 0u) {
                /* Same head, same count: the second ring has at least the n frames the first just gave. */
                if (truing_demo_listen_read(1u, &pos_b, b, n, &lost) != n) {
                    lost += 1u;
                    break;
                }
                for (uint32_t i = 0; i < n; ++i) {
                    out[2u * i] = a[i];
                    out[2u * i + 1u] = b[i];
                }
                rc = httpd_resp_send_chunk(req, (const char *)out, (ssize_t)(n * 2u * sizeof(int32_t)));
            }
        }
        if (n == 0u) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }
        sent += n;
    }
    busy = false;
    if (fire_n != 0u) {
        ESP_LOGI(TAG, "listen burst: %" PRIu32 " of %" PRIu32 " shots fired (%" PRIu32 " refused), %" PRIu32 " ms each, %" PRIu32 " ms apart",
                 fired, fire_n, refused, fire_ms, fire_gap);
    }
    if (lost != 0u) {
        ESP_LOGW(TAG, "listen ended early at %" PRIu32 " of %" PRIu32 " samples: the reader fell %" PRIu32 " behind",
                 sent, want, lost);
    }
    if (rc != ESP_OK) return ESP_FAIL;
    return httpd_resp_send_chunk(req, NULL, 0);
}
#endif

#if TRUING_BENCH_DRIVE
/* GET /debug/drive.json: the lab image's bench drive, read-only (control is the DRIVE_* debug codes
 * over /ws). Plain reads of the driver's fields -- see truing_demo_bench_drive_state(). */
static esp_err_t drive_json_handler(httpd_req_t *req)
{
    no_store(req);
    httpd_resp_set_type(req, "application/json");
    truing_demo_drive_state_t d;
    if (!truing_demo_bench_drive_state(&d)) {
        httpd_resp_set_status(req, "404 Not Found");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"detail\":\"no bench drive (init failed)\"}");
    }
    char body[640];
    truing_json_writer_t w;
    truing_json_init(&w, body, sizeof(body));
    truing_json_obj_open(&w, NULL);
    truing_json_str(&w, "schema", "truing.bench.drive/1");
    truing_json_bool(&w, "drv_ok", d.drv_ok);
    truing_json_u32(&w, "drv_status", d.drv_status);
    truing_json_u32(&w, "gstat", d.gstat);
    truing_json_u32(&w, "cs_actual", d.cs_actual);
    truing_json_str(&w, "drv_flags", d.drv_flags);
    truing_json_bool(&w, "enabled", d.enabled);
    truing_json_bool(&w, "moving", d.moving);
    truing_json_bool(&w, "faulted", d.faulted);
    truing_json_u32(&w, "fault_detail", d.fault_detail);
    truing_json_bool(&w, "diag_high", d.diag_high);
    truing_json_bool(&w, "uart_ok", d.uart_ok);
    truing_json_i32(&w, "position_steps", d.position_steps);
    truing_json_u32(&w, "max_step_hz", d.max_step_hz);
    truing_json_u32(&w, "accel_steps_s2", d.accel_steps_s2);
    truing_json_u32(&w, "microsteps_want", d.microsteps_want);
    truing_json_bool(&w, "mres_ok", d.mres_ok);
    truing_json_u32(&w, "mres_repairs", d.mres_repairs);
    truing_json_obj_close(&w);
    size_t len = 0u;
    if (!truing_json_finish(&w, &len)) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"detail\":\"drive state did not fit\"}");
    }
    return httpd_resp_send(req, body, len);
}

static void laser_trial_json(truing_json_writer_t *w, const char *key, const truing_beam_trial_t *t)
{
    truing_json_obj_open(w, key);
    truing_json_u32(w, "trial", t->trial);
    truing_json_str(w, "result", truing_beam_result_str(t->result));
    truing_json_bool(w, "fired", t->fired);
    truing_json_bool(w, "ended", t->ended);
    truing_json_bool(w, "blocked_at_arm", t->blocked_at_arm);
    truing_json_u32(w, "max_step_hz", t->max_step_hz);
    truing_json_u32(w, "accel_steps_s2", t->accel_steps_s2);
    truing_json_i32(w, "trigger_pos", t->trigger_pos);
    truing_json_i32(w, "final_pos", t->final_pos);
    /* end_t_us is when the 2 ms poll saw the pulse train over: the stop time is good to about 2 ms */
    truing_json_u32(w, "stop_us", t->fired && t->ended ? t->end_t_us - t->trigger_t_us : 0u);
    truing_json_bool(w, "left_beam", t->left_beam);
    truing_json_i32(w, "leave_pos", t->leave_pos);
    truing_json_u32(w, "edges_after", t->edges_after);
    truing_json_bool(w, "blocked_at_end", t->blocked_at_end);
    truing_json_bool(w, "coils_at_end", t->coils_at_end);
    truing_json_bool(w, "fault_at_end", t->fault_at_end);
    truing_json_obj_close(w);
}

/* GET /debug/laser.json[?arm=1|0][&probe=1][&since=<edge seq>]: the bench laser receiver and laser-stop
 * (laser_bench.h). Plain state always; arm/disarm/probe are the only actions, and arming can only ever
 * request the drive's own controlled stop, so it needs no admission. `since` returns the beam edges newer
 * than that sequence number (at most 16 per call; `log_lost` counts any the ring already overwrote). */
static esp_err_t laser_json_handler(httpd_req_t *req)
{
    static truing_beam_stop_t bs;          /* the httpd task is the only caller: static keeps it off its stack */
    static truing_beam_edge_rec_t recs[16];
    static char body[2560];
    char q[64], v[12];
    const char *action = NULL, *detail = NULL;
    bool action_ok = true;
    uint32_t since = UINT32_MAX;
    no_store(req);
    httpd_resp_set_type(req, "application/json");
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        if (httpd_query_key_value(q, "arm", v, sizeof(v)) == ESP_OK) {
            const bool on = strcmp(v, "0") != 0;
            action = on ? "arm" : "disarm";
            action_ok = truing_laser_bench_arm(on, &detail);
        }
        if (httpd_query_key_value(q, "probe", v, sizeof(v)) == ESP_OK && strcmp(v, "1") == 0) {
            const char *pd = NULL;
            const bool pok = truing_laser_bench_probe(&pd);
            if (action == NULL) {
                action = "probe";
                action_ok = pok;
                detail = pd;
            }
        }
        if (httpd_query_key_value(q, "since", v, sizeof(v)) == ESP_OK) {
            since = (uint32_t)strtoul(v, NULL, 10);
        }
    }
    truing_laser_bench_io_t io;
    if (!truing_laser_bench_snapshot(&bs, &io)) {
        httpd_resp_set_status(req, "404 Not Found");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"detail\":\"no laser receiver input (lab image only, needs the bench drive)\"}");
    }
    truing_json_writer_t w;
    truing_json_init(&w, body, sizeof(body));
    truing_json_obj_open(&w, NULL);
    truing_json_str(&w, "schema", "truing.bench.laser/1");
    if (action != NULL) {
        truing_json_str(&w, "action", action);
        truing_json_bool(&w, "action_ok", action_ok);
        truing_json_str(&w, "detail", detail != NULL ? detail : "");
    }
    truing_json_u32(&w, "gpio", (uint32_t)io.gpio);
    truing_json_u32(&w, "level", (uint32_t)io.level);
    truing_json_str(&w, "beam", io.level == BOARD_SPOKE_BEAM_BLOCKED_LEVEL ? "BLOCKED" : "CLEAR");
    truing_json_str(&w, "input", io.input == TRUING_LASER_INPUT_DRIVEN     ? "driven"
                                 : io.input == TRUING_LASER_INPUT_FLOATING ? "floating"
                                                                            : "unprobed");
    truing_json_u32(&w, "probed_age_ms", io.probed_age_ms);
    truing_json_u32(&w, "edges", bs.edges);
    truing_json_u32(&w, "enters", bs.enters);
    truing_json_u32(&w, "leaves", bs.leaves);
    truing_json_u32(&w, "glitches", bs.glitches);
    truing_json_u32(&w, "resyncs", bs.resyncs);
    if (bs.edges != 0u) {
        truing_json_u32(&w, "last_edge_age_ms", (io.now_us - bs.last_edge_t_us) / 1000u);
    } else {
        truing_json_null(&w, "last_edge_age_ms");
    }
    truing_json_bool(&w, "armed", bs.armed);
    truing_json_u32(&w, "trials", bs.trials);
    truing_json_u32(&w, "stops_requested", bs.stops_requested);
    if (bs.trials != 0u) {
        laser_trial_json(&w, "trial", &bs.cur);
    }
    if (bs.last.result != TRUING_BEAM_RESULT_NONE) {
        laser_trial_json(&w, "last", &bs.last);
    }
    truing_json_u32(&w, "log_seq", bs.log_seq);
    if (since != UINT32_MAX) {
        uint32_t lost = 0u;
        const size_t n = truing_beam_stop_edges_since(&bs, since, recs, sizeof(recs) / sizeof(recs[0]), &lost);
        truing_json_u32(&w, "log_lost", lost);
        truing_json_arr_open(&w, "log");
        for (size_t i = 0; i < n; ++i) {
            truing_json_obj_open(&w, NULL);
            truing_json_u32(&w, "seq", recs[i].seq);
            truing_json_str(&w, "edge", recs[i].edge == TRUING_SPOKE_EDGE_ENTER ? "ENTER" : "LEAVE");
            truing_json_u32(&w, "t_us", recs[i].t_us);
            truing_json_i32(&w, "pos", recs[i].pos);
            truing_json_bool(&w, "moving", recs[i].moving);
            truing_json_obj_close(&w);
        }
        truing_json_arr_close(&w);
    }
    truing_json_obj_close(&w);
    size_t len = 0u;
    if (!truing_json_finish(&w, &len)) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"detail\":\"laser state did not fit\"}");
    }
    return httpd_resp_send(req, body, len);
}
#endif

#if TRUING_BENCH_DRIVE
static uint32_t q_u32(const char *q, const char *key, uint32_t dflt)
{
    char v[16];
    return httpd_query_key_value(q, key, v, sizeof(v)) == ESP_OK ? (uint32_t)strtoul(v, NULL, 10) : dflt;
}

static int32_t q_i32(const char *q, const char *key, int32_t dflt)
{
    char v[16];
    return httpd_query_key_value(q, key, v, sizeof(v)) == ESP_OK ? (int32_t)strtol(v, NULL, 10) : dflt;
}

/* GET /debug/align.json: the alignment campaign's real-time controller (truing_hal/beam_align.h, run by
 * laser_bench.c on the board). Plain state always, including the run record, its phase events and its beam
 * edges after ?since=<index> (at most 24 per call). Actions:
 *   ?start=count|align&dir=+-1&n=<break>&max=<steps>&settle=<ms>&strat=STOP|REVERSE_FIXED|RETURN_TO_EDGE|REACQUIRE|REACQUIRE_CLEAR
 *          &rev=<steps>&off=<steps>&center=0|1&shz=<steps/s>&sacc=<steps/s2>&rmax=<steps>
 *   ?abort=1
 * Every parameter is the operator's campaign choice for that run; nothing here has a physical default. */
static esp_err_t align_json_handler(httpd_req_t *req)
{
    static truing_beam_align_t a;   /* the httpd task is the only caller */
    static char body[7168];
    char q[320], v[20];
    const char *action = NULL, *detail = NULL;
    bool action_ok = true;
    uint32_t since = UINT32_MAX;
    no_store(req);
    httpd_resp_set_type(req, "application/json");
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        if (httpd_query_key_value(q, "abort", v, sizeof(v)) == ESP_OK) {
            action = "abort";
            action_ok = truing_laser_align_abort(&detail);
        } else if (httpd_query_key_value(q, "start", v, sizeof(v)) == ESP_OK) {
            truing_align_config_t c;
            memset(&c, 0, sizeof(c));
            action = "start";
            c.mode = strcmp(v, "count") == 0 ? TRUING_ALIGN_MODE_COUNT
                     : strcmp(v, "align") == 0 ? TRUING_ALIGN_MODE_ALIGN : (truing_align_mode_t)0;
            c.dir = (int8_t)q_i32(q, "dir", 0);
            c.target_break = (uint16_t)q_u32(q, "n", 0u);
            c.max_steps = q_u32(q, "max", 0u);
            c.settle_ms = q_u32(q, "settle", 0u);
            c.reverse_steps = q_u32(q, "rev", 0u);
            c.offset_steps = q_i32(q, "off", 0);
            c.center = q_u32(q, "center", 0u) != 0u;
            c.slow_hz = q_u32(q, "shz", 0u);
            c.slow_accel = q_u32(q, "sacc", 0u);
            c.reacq_max_steps = q_u32(q, "rmax", 0u);
            c.strategy = TRUING_ALIGN_STRAT_STOP;
            if (httpd_query_key_value(q, "strat", v, sizeof(v)) == ESP_OK && !truing_align_strategy_parse(v, &c.strategy)) {
                action_ok = false;
                detail = "unknown strategy (STOP, REVERSE_FIXED, RETURN_TO_EDGE, REACQUIRE, REACQUIRE_CLEAR)";
            }
            if (action_ok) {
                action_ok = truing_laser_align_start(&c, &detail);
            }
        }
        if (httpd_query_key_value(q, "since", v, sizeof(v)) == ESP_OK) {
            since = (uint32_t)strtoul(v, NULL, 10);
        }
    }
    if (!truing_laser_align_snapshot(&a)) {
        httpd_resp_set_status(req, "404 Not Found");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"detail\":\"no alignment controller (lab image only, needs the laser input)\"}");
    }
    truing_json_writer_t w;
    truing_json_init(&w, body, sizeof(body));
    truing_json_obj_open(&w, NULL);
    truing_json_str(&w, "schema", "truing.bench.align/1");
    if (action != NULL) {
        truing_json_str(&w, "action", action);
        truing_json_bool(&w, "action_ok", action_ok);
        truing_json_str(&w, "detail", detail != NULL ? detail : "");
    }
    truing_json_u32(&w, "run", a.run);
    truing_json_str(&w, "phase", truing_align_phase_str(a.phase));
    truing_json_bool(&w, "active", truing_beam_align_active(&a));
    truing_json_str(&w, "result", truing_align_result_str(a.result));
    truing_json_str(&w, "result_detail", truing_align_detail_str(a.detail));
    truing_json_bool(&w, "beam_blocked", a.blocked);
    truing_json_obj_open(&w, "cfg");
    truing_json_str(&w, "mode", a.cfg.mode == TRUING_ALIGN_MODE_COUNT ? "count" : "align");
    truing_json_str(&w, "strategy", truing_align_strategy_str(a.cfg.strategy));
    truing_json_i32(&w, "dir", a.cfg.dir);
    truing_json_u32(&w, "target_break", a.cfg.target_break);
    truing_json_u32(&w, "max_steps", a.cfg.max_steps);
    truing_json_u32(&w, "settle_ms", a.cfg.settle_ms);
    truing_json_u32(&w, "reverse_steps", a.cfg.reverse_steps);
    truing_json_i32(&w, "offset_steps", a.cfg.offset_steps);
    truing_json_bool(&w, "center", a.cfg.center);
    truing_json_u32(&w, "slow_hz", a.cfg.slow_hz);
    truing_json_u32(&w, "slow_accel", a.cfg.slow_accel);
    truing_json_u32(&w, "reacq_max_steps", a.cfg.reacq_max_steps);
    truing_json_obj_close(&w);
    truing_json_u32(&w, "approach_hz", a.approach_hz);
    truing_json_u32(&w, "approach_accel", a.approach_accel);
    truing_json_i32(&w, "start_pos", a.start_pos);
    truing_json_bool(&w, "start_blocked", a.start_blocked);
    truing_json_u32(&w, "start_t_us", a.start_t_us);
    truing_json_u32(&w, "breaks", a.breaks);
    truing_json_bool(&w, "fired", a.fired);
    truing_json_i32(&w, "trigger_pos", a.trigger_pos);
    truing_json_u32(&w, "trigger_t_us", a.trigger_t_us);
    truing_json_i32(&w, "stop_pos", a.stop_pos);
    truing_json_u32(&w, "stop_t_us", a.stop_t_us);
    truing_json_bool(&w, "blocked_after_stop", a.blocked_after_stop);
    truing_json_i32(&w, "zone_after_stop", a.zone_after_stop);
    truing_json_u32(&w, "edges_in_stop", a.edges_in_stop);
    truing_json_bool(&w, "target_left", a.target_left);
    truing_json_i32(&w, "target_leave_pos", a.target_leave_pos);
    truing_json_i32(&w, "pitch_med", a.pitch_med);
    truing_json_i32(&w, "pitch_min", a.pitch_min);
    truing_json_i32(&w, "width_med", a.width_med);
    truing_json_i32(&w, "width_used", a.width_used);
    truing_json_i32(&w, "corr_cmd", a.corr_cmd);
    truing_json_u32(&w, "corr_edges", a.corr_edges);
    truing_json_bool(&w, "reacquired", a.reacquired);
    truing_json_u32(&w, "reacquisitions", a.reacquisitions);
    truing_json_i32(&w, "reacq_pos", a.reacq_pos);
    truing_json_bool(&w, "cleared", a.cleared);
    truing_json_i32(&w, "clear_pos", a.clear_pos);
    truing_json_bool(&w, "same_spoke_checked", a.same_spoke_checked);
    truing_json_bool(&w, "same_spoke_ok", a.same_spoke_ok);
    truing_json_i32(&w, "trim_cmd", a.trim_cmd);
    truing_json_i32(&w, "final_pos", a.final_pos);
    truing_json_bool(&w, "final_blocked", a.final_blocked);
    truing_json_i32(&w, "final_zone", a.final_zone);
    truing_json_u32(&w, "end_t_us", a.end_t_us);
    truing_json_u32(&w, "edges_still", a.edges_still);
    truing_json_bool(&w, "parity_bad", a.parity_bad);
    truing_json_arr_open(&w, "events");
    for (uint16_t i = 0; i < a.n_events; ++i) {
        truing_json_obj_open(&w, NULL);
        truing_json_str(&w, "phase", truing_align_phase_str((truing_align_phase_t)a.events[i].phase));
        truing_json_u32(&w, "t_us", a.events[i].t_us);
        truing_json_i32(&w, "pos", a.events[i].pos);
        truing_json_bool(&w, "blocked", a.events[i].blocked);
        truing_json_obj_close(&w);
    }
    truing_json_arr_close(&w);
    truing_json_u32(&w, "n_edges", a.n_edges);
    truing_json_u32(&w, "edges_dropped", a.edges_dropped);
    if (since != UINT32_MAX) {
        truing_json_u32(&w, "since", since);
        truing_json_arr_open(&w, "edges");
        for (uint32_t i = since, n = 0u; i < a.n_edges && n < 24u; ++i, ++n) {
            const truing_align_edge_t *e = &a.edges[i];
            truing_json_obj_open(&w, NULL);
            truing_json_u32(&w, "i", i);
            truing_json_bool(&w, "enter", e->enter);
            truing_json_u32(&w, "t_us", e->t_us);
            truing_json_i32(&w, "pos", e->pos);
            truing_json_i32(&w, "sign", e->sign);
            truing_json_str(&w, "phase", truing_align_phase_str((truing_align_phase_t)e->phase));
            truing_json_i32(&w, "zone", e->zone);
            truing_json_obj_close(&w);
        }
        truing_json_arr_close(&w);
    }
    truing_json_obj_close(&w);
    size_t len = 0u;
    if (!truing_json_finish(&w, &len)) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"detail\":\"alignment state did not fit\"}");
    }
    return httpd_resp_send(req, body, len);
}
#endif

#if TRUING_DEMO_ROLLER_JOG
/* The pipeline-demonstration build's MANUAL roller: GET /demo/jog?steps=<signed int> turns the wheel by
 * that many steps (energising the coils if needed); GET /demo/jog?enable=0 stops and releases it; with
 * no query it only reports. It is NOT an operator intent and answers no wait: the operator still
 * confirms the spoke at the station by eye, and the step count is a jog distance, never a position
 * (build_mode.h). Unlike /demo/acquisition it does actuate hardware during a session, so it is fenced in
 * firmware (truing_demo_jog, orch_demo.c): a move only while the machine is idle or asking the operator
 * to position a spoke, never while a strike is prepared or captured, and every strike waits for a jog to
 * finish and the wheel to settle. */
static esp_err_t jog_handler(httpd_req_t *req)
{
    char q[48], v[16];
    bool have_steps = false, release = false;
    long steps = 0;
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
        if (httpd_query_key_value(q, "steps", v, sizeof(v)) == ESP_OK) {
            steps = strtol(v, NULL, 10);
            have_steps = true;
        }
        if (httpd_query_key_value(q, "enable", v, sizeof(v)) == ESP_OK && strcmp(v, "0") == 0) {
            release = true;
        }
    }
    const char *detail = "status";
    bool ok = true;
    if (release || have_steps) {
        ok = truing_demo_jog((int32_t)steps, release, &detail);
        if (ok) {
            ESP_LOGI(TAG, "operator jog: %s", release ? "release" : "move");
        }
    }
    truing_demo_jog_state_t st;
    const bool have = truing_demo_jog_state(&st);
    char body[192];
    const int m = snprintf(body, sizeof(body),
                           "{\"ok\":%s,\"enabled\":%s,\"moving\":%s,\"faulted\":%s,\"position_steps\":%ld,\"detail\":\"%s\"}",
                           ok && have ? "true" : "false", have && st.enabled ? "true" : "false",
                           have && st.moving ? "true" : "false", have && st.faulted ? "true" : "false",
                           have ? (long)st.position_steps : 0L, have ? detail : "this image has no manual roller");
    no_store(req);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, m > 0 ? (size_t)m : 0u);
}
#endif

static esp_err_t id_get_handler(httpd_req_t *req)
{
    char body[448];   /* the reply is ~330 bytes; a truncated JSON body would be silently wrong */
    uint8_t mac[6] = { 0 };
    (void)esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    truing_net_stats_t st;
    truing_net_get_stats(&st);
    /* real_front_end and acoustic_demo_spokes are reported as facts rather than left for the
     * page to infer from the mode string: the page must not have to parse a name to know
     * whether the microphone is real or how many spokes this image will pluck. */
    const int n = snprintf(body, sizeof(body),
        "{\"board\":\"%s\",\"firmware\":\"%s\",\"build\":\"%s\",\"ui\":\"%s\",\"mode\":\"%s\","
        "\"acquisition\":\"%s\",\"acquisition_selectable\":%s,"
        "\"real_front_end\":%s,\"acoustic_demo_spokes\":%d,\"composite_navigation\":%s,"
        "\"always_read\":%s,\"jog\":%s,"
        "\"ssid\":\"truing-%02x%02x%02x\",\"uptime_s\":%lld,\"clients\":%u}",
        BOARD_NAME, TRUING_FIRMWARE_VERSION, TRUING_BUILD_REV, TRUING_UI_HASH, TRUING_BUILD_MODE_STR,
        truing_demo_acquisition_is_automatic() ? "auto" : "manual", TRUING_FAST_DEMO ? "true" : "false",
        TRUING_REAL_FRONT_END ? "true" : "false", (int)TRUING_ACOUSTIC_DEMO_SPOKES,
        truing_demo_navigation_is_composite() ? "true" : "false",
        truing_demo_always_read_active() ? "true" : "false", TRUING_DEMO_ROLLER_JOG ? "true" : "false",
        mac[3], mac[4], mac[5], (long long)(esp_timer_get_time() / 1000000), (unsigned)st.clients);
    no_store(req);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, n > 0 ? (size_t)n : 0u);
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "websocket client connected (fd %d)", httpd_req_to_sockfd(req));
        return ESP_OK;   /* handshake */
    }
    httpd_ws_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    frame.type = HTTPD_WS_TYPE_TEXT;
    /* First call with len 0 reports the payload size. */
    esp_err_t e = httpd_ws_recv_frame(req, &frame, 0);
    if (e != ESP_OK) {
        return e;
    }
    if (frame.type != HTTPD_WS_TYPE_TEXT || frame.len == 0u) {
        return ESP_OK;
    }
    /* SPEC §12.3: reject, do not accommodate. An oversized frame is dropped unread
     * rather than truncated into something that might parse as a different command. */
    if (frame.len > TRUING_WIRE_COMMAND_MAX) {
        s_stats.inbound_dropped++;
        ESP_LOGW(TAG, "inbound frame of %u bytes exceeds the wire limit; dropped", (unsigned)frame.len);
        return ESP_OK;
    }
    static in_item_t item;   /* the httpd task owns this handler; one buffer suffices */
    frame.payload = (uint8_t *)item.data;
    e = httpd_ws_recv_frame(req, &frame, TRUING_WIRE_COMMAND_MAX);
    if (e != ESP_OK) {
        return e;
    }
    item.fd = httpd_req_to_sockfd(req);
    item.len = (uint16_t)frame.len;
    item.data[item.len] = '\0';
    if (xQueueSend(s_inbound, &item, 0) != pdTRUE) {
        s_stats.inbound_dropped++;
    } else {
        s_stats.frames_received++;
    }
    return ESP_OK;
}

/* ---- start-up --------------------------------------------------------------------- */

/* Derived from the board's MAC, so every unit differs and nothing is committed. */
static void derive_credentials(char *ssid, size_t ssid_cap, char *pass, size_t pass_cap)
{
    uint8_t mac[6] = { 0 };
    (void)esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    (void)snprintf(ssid, ssid_cap, "truing-%02x%02x%02x", mac[3], mac[4], mac[5]);
    (void)snprintf(pass, pass_cap, "truing-%02x%02x%02x%02x", mac[2], mac[3], mac[4], mac[5]);
}

static bool wifi_start_ap(char *ssid, size_t ssid_cap, char *pass, size_t pass_cap)
{
    /* The WiFi driver keeps its calibration and configuration in NVS, and the transport
     * now starts before the bring-up's own NVS section. Idempotent: the later call is a
     * no-op, and both go through the same helper so the erase-and-retry path is shared. */
    if (truing_config_store_init() != ESP_OK) {
        return false;
    }
    if (esp_netif_init() != ESP_OK) {
        return false;
    }
    const esp_err_t loop = esp_event_loop_create_default();
    if (loop != ESP_OK && loop != ESP_ERR_INVALID_STATE) {
        return false;
    }
    if (esp_netif_create_default_wifi_ap() == NULL) {
        return false;
    }
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&cfg) != ESP_OK) {
        return false;
    }
    derive_credentials(ssid, ssid_cap, pass, pass_cap);

    wifi_config_t wc;
    memset(&wc, 0, sizeof(wc));
    strncpy((char *)wc.ap.ssid, ssid, sizeof(wc.ap.ssid) - 1u);
    wc.ap.ssid_len = (uint8_t)strlen(ssid);
    strncpy((char *)wc.ap.password, pass, sizeof(wc.ap.password) - 1u);
    wc.ap.channel = AP_CHANNEL;
    wc.ap.max_connection = MAX_WS_CLIENTS;
    wc.ap.authmode = WIFI_AUTH_WPA2_PSK;
    wc.ap.pmf_cfg.required = false;

    if (esp_wifi_set_mode(WIFI_MODE_AP) != ESP_OK ||
        esp_wifi_set_config(WIFI_IF_AP, &wc) != ESP_OK ||
        esp_wifi_start() != ESP_OK) {
        return false;
    }
    return true;
}

bool truing_net_start(void)
{
    if (s_started) {
        return true;
    }
    char ssid[24];
    char pass[24];
    if (!wifi_start_ap(ssid, sizeof(ssid), pass, sizeof(pass))) {
        ESP_LOGE(TAG, "soft-AP failed to start; the machine runs without a host (SPEC 12.2)");
        return false;
    }

    s_outbound = xQueueCreate(OUTBOUND_SLOTS, sizeof(out_item_t));
    s_inbound = xQueueCreate(INBOUND_SLOTS, sizeof(in_item_t));
    if (s_outbound == NULL || s_inbound == NULL) {
        ESP_LOGE(TAG, "transport queues could not be allocated");
        return false;
    }

    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.max_open_sockets = MAX_OPEN_SOCKETS;   /* ws_clients() sizes its array by the same number */
    /* The ESP-IDF default is 4096, which capture_meta_handler does not fit in: a 1.5 KB body, the
     * capture view, two digests and newlib's float formatting all live on this task's stack, and a
     * fetch of a finished capture overflowed it and rebooted the board (2026-09-19, first B3.0
     * attempt). The high-water mark is logged where that handler finishes so the margin is measured
     * on target rather than assumed. */
    hc.stack_size = HTTPD_STACK_BYTES;
    hc.lru_purge_enable = true;
    /* The default is 8; the lab image registers eleven (ui, id, ws, capture x3, listen, drive, laser, align,
     * acquisition). */
    hc.max_uri_handlers = 12;
    /* Core 1 is the audio core (SPEC §4.5); the web server belongs with the control
     * work on core 0 so it cannot preempt a capture. */
    hc.core_id = 0;
    if (httpd_start(&s_server, &hc) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server failed to start");
        return false;
    }
    static const httpd_uri_t ui_uri = { .uri = "/", .method = HTTP_GET, .handler = ui_get_handler };
    static const httpd_uri_t id_uri = { .uri = "/id", .method = HTTP_GET, .handler = id_get_handler };
    static const httpd_uri_t ws_uri = {
        .uri = "/ws", .method = HTTP_GET, .handler = ws_handler, .is_websocket = true,
    };
    static const httpd_uri_t cap_meta_uri = { .uri = "/debug/capture.json", .method = HTTP_GET,
                                              .handler = capture_meta_handler };
    static const httpd_uri_t cap_pcm_uri = { .uri = "/debug/capture.pcm", .method = HTTP_GET,
                                             .handler = capture_pcm_handler };
    (void)httpd_register_uri_handler(s_server, &ui_uri);
    (void)httpd_register_uri_handler(s_server, &id_uri);
    (void)httpd_register_uri_handler(s_server, &ws_uri);
    /* Checked, unlike the three above: an unregistered debug URL answers 404, which reads
     * exactly like "nothing captured yet" from the client end. */
    if (httpd_register_uri_handler(s_server, &cap_meta_uri) != ESP_OK ||
        httpd_register_uri_handler(s_server, &cap_pcm_uri) != ESP_OK) {
        ESP_LOGW(TAG, "capture dump endpoints not registered; /debug/capture.* will 404");
    }
#if TRUING_DUAL_MIC
    static const httpd_uri_t cap_far_uri = { .uri = "/debug/capture_far.pcm", .method = HTTP_GET,
                                             .handler = capture_far_pcm_handler };
    if (httpd_register_uri_handler(s_server, &cap_far_uri) != ESP_OK) {
        ESP_LOGW(TAG, "far-mic dump endpoint not registered; /debug/capture_far.pcm will 404");
    }
#endif
#if TRUING_CAMPAIGN_DEBUG && TRUING_REAL_FRONT_END
    static const httpd_uri_t listen_uri = { .uri = "/debug/listen.pcm", .method = HTTP_GET,
                                            .handler = listen_pcm_handler };
    if (httpd_register_uri_handler(s_server, &listen_uri) != ESP_OK) {
        ESP_LOGW(TAG, "listen endpoint not registered; /debug/listen.pcm will 404");
    }
#endif
#if TRUING_BENCH_DRIVE
    static const httpd_uri_t drive_uri = { .uri = "/debug/drive.json", .method = HTTP_GET,
                                           .handler = drive_json_handler };
    if (httpd_register_uri_handler(s_server, &drive_uri) != ESP_OK) {
        ESP_LOGW(TAG, "bench-drive endpoint not registered; /debug/drive.json will 404");
    }
    static const httpd_uri_t laser_uri = { .uri = "/debug/laser.json", .method = HTTP_GET,
                                           .handler = laser_json_handler };
    if (httpd_register_uri_handler(s_server, &laser_uri) != ESP_OK) {
        ESP_LOGW(TAG, "bench laser endpoint not registered; /debug/laser.json will 404");
    }
    static const httpd_uri_t align_uri = { .uri = "/debug/align.json", .method = HTTP_GET,
                                           .handler = align_json_handler };
    if (httpd_register_uri_handler(s_server, &align_uri) != ESP_OK) {
        ESP_LOGW(TAG, "alignment campaign endpoint not registered; /debug/align.json will 404");
    }
#endif
#if TRUING_FAST_DEMO
    /* Only this image serves it at all. */
    static const httpd_uri_t acq_uri = { .uri = "/demo/acquisition", .method = HTTP_GET,
                                         .handler = acq_post_handler };
    (void)httpd_register_uri_handler(s_server, &acq_uri);
#endif
#if TRUING_DEMO_ROLLER_JOG
    static const httpd_uri_t jog_uri = { .uri = "/demo/jog", .method = HTTP_GET, .handler = jog_handler };
    if (httpd_register_uri_handler(s_server, &jog_uri) != ESP_OK) {
        ESP_LOGW(TAG, "manual roller endpoint not registered; /demo/jog will 404");
    }
#endif

    if (xTaskCreatePinnedToCore(sender_task, "wire_tx", SENDER_STACK, NULL, tskIDLE_PRIORITY + 3, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "sender task could not be created");
        return false;
    }

    s_started = true;
    ESP_LOGI(TAG, "=========================================================");
    ESP_LOGI(TAG, " SPEC 12.1 transport up. Join the network and browse to:");
    ESP_LOGI(TAG, "   SSID       %s", ssid);
    ESP_LOGI(TAG, "   passphrase %s", pass);
    ESP_LOGI(TAG, "   URL        http://192.168.4.1/");
    ESP_LOGI(TAG, " (derived from this board's MAC; not stored in the repository)");
    /* Printed here and served at /id and shown in the page footer. Both boards answer on
     * 192.168.4.1, so this is how you tell which one you reached. */
    ESP_LOGI(TAG, "   board %s | firmware %s | build %s | ui %s | mode %s",
             BOARD_NAME, TRUING_FIRMWARE_VERSION, TRUING_BUILD_REV, TRUING_UI_HASH, TRUING_BUILD_MODE_STR);
    if (TRUING_FAST_DEMO) {
        ESP_LOGW(TAG, "   FAST DEMO image: acquisition is SYNTHETIC; results are not physical wheel validation");
    }
    ESP_LOGI(TAG, "=========================================================");
    return true;
}
