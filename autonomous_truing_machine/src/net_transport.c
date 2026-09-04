/* SPEC §12.1 WiFi AP transport: soft-AP, HTTP server, one websocket endpoint. */
#include "net_transport.h"

#include <inttypes.h>
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

#include "config_store_nvs.h"
#include "truing_proto/wire.h"
#include "web_ui.h"

static const char *TAG = "net";

/* Deliberately small. A deep outbound queue would only let a dead client accumulate
 * stale telemetry that is worthless by the time it drains; dropping early is what
 * SPEC §12.2 asks for, and the current-state query is how a client resynchronises. */
#define OUTBOUND_SLOTS      12
#define INBOUND_SLOTS        4
#define MAX_WS_CLIENTS       4
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
    int all[MAX_WS_CLIENTS];
    size_t n = MAX_WS_CLIENTS;
    if (httpd_get_client_list(s_server, &n, all) != ESP_OK) {
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

static esp_err_t ui_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, TRUING_WEB_UI_HTML, HTTPD_RESP_USE_STRLEN);
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
    hc.max_open_sockets = MAX_WS_CLIENTS + 1;
    hc.lru_purge_enable = true;
    /* Core 1 is the audio core (SPEC §4.5); the web server belongs with the control
     * work on core 0 so it cannot preempt a capture. */
    hc.core_id = 0;
    if (httpd_start(&s_server, &hc) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server failed to start");
        return false;
    }
    static const httpd_uri_t ui_uri = { .uri = "/", .method = HTTP_GET, .handler = ui_get_handler };
    static const httpd_uri_t ws_uri = {
        .uri = "/ws", .method = HTTP_GET, .handler = ws_handler, .is_websocket = true,
    };
    (void)httpd_register_uri_handler(s_server, &ui_uri);
    (void)httpd_register_uri_handler(s_server, &ws_uri);

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
    ESP_LOGI(TAG, "=========================================================");
    return true;
}
