/* Phase 1a bring-up (SPEC 4.1, 9.4.1, 11.5, 17.1.3).
 *
 * Verifies platform properties on the actual board, runs the framework-free core
 * against the same synthetic implementations the host tests use, executes the
 * SPEC 9.4.1 I2S channel-creation check, and reports the configuration
 * persistence state. Everything it prints is a measurement of THIS board and
 * THIS build; nothing here is assumed. */
#include "bringup.h"

#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"
#include "driver/i2s_std.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "nvs.h"

#include "board/board_profile.h"
#include "bringup_acoustic.h"
#include "bringup_artifact.h"
#include "config_store_nvs.h"
#include "firmware_version.h"
#include "truing/config.h"
#include "truing/config_blob.h"
#include "truing/limits.h"
#include "truing/operator_intent.h"
#include "truing/row_layout.h"
#include "truing/session.h"
#include "truing/wheel_geometry.h"
#include "truing/wheel_state.h"
#include "truing_fixtures/fixtures.h"
#include "truing_hal/acoustic_if.h"
#include "truing_hal/navigation_manual.h"
#include "truing_hal/navigation_synthetic.h"
#include "truing_hal/runout_if.h"
#include "truing_hal/wheel_drive_if.h"

static const char *TAG = "bringup";

typedef struct {
    int pass;
    int fail;
} tally_t;

static void check(tally_t *t, bool ok, const char *what)
{
    if (ok) {
        t->pass++;
        ESP_LOGI(TAG, "  PASS  %s", what);
    } else {
        t->fail++;
        ESP_LOGE(TAG, "  FAIL  %s", what);
    }
}

static uint32_t boot_clock_now(void *ctx)
{
    (void)ctx;
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* ---- platform ------------------------------------------------------------------- */
static void section_platform(tally_t *t, truing_bringup_report_t *r)
{
    ESP_LOGI(TAG, "== platform ==");
    esp_chip_info_t info;
    esp_chip_info(&info);
    ESP_LOGI(TAG, "firmware %s | board %s | IDF %s", TRUING_FIRMWARE_VERSION, truing_board_name(), esp_get_idf_version());
    ESP_LOGI(TAG, "chip model=%d revision=%u cores=%u features=0x%08" PRIx32,
             (int)info.model, (unsigned)info.revision, (unsigned)info.cores, (uint32_t)info.features);
    uint32_t flash = 0u;
    const esp_err_t e = esp_flash_get_size(NULL, &flash);
    ESP_LOGI(TAG, "flash: %" PRIu32 " bytes (%s); board profile expects %u", flash, esp_err_to_name(e),
             (unsigned)BOARD_FLASH_EXPECTED_BYTES);
    r->flash_ok = (e == ESP_OK) && (flash == BOARD_FLASH_EXPECTED_BYTES);
    check(t, r->flash_ok, "flash size matches the board profile");
}

static void section_memory(tally_t *t, truing_bringup_report_t *r)
{
    ESP_LOGI(TAG, "== memory (SPEC 4.1, 9.4.1) ==");
    const bool init = esp_psram_is_initialized();
    const size_t psram = esp_psram_get_size();
    ESP_LOGI(TAG, "psram: initialised=%d size=%u bytes; board profile expects %u",
             (int)init, (unsigned)psram, (unsigned)BOARD_PSRAM_EXPECTED_BYTES);
    ESP_LOGI(TAG, "heap SPIRAM: total=%u free=%u | internal: total=%u free=%u",
             (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_total_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    r->psram_ok = init && psram == BOARD_PSRAM_EXPECTED_BYTES;
    check(t, r->psram_ok, "PSRAM present and sized as the board profile expects (verified on the board)");

    void *ext = heap_caps_malloc(1u << 20, MALLOC_CAP_SPIRAM);
    check(t, ext != NULL && esp_ptr_external_ram(ext), "1 MiB SPIRAM allocation lands in external RAM");
    free(ext);

    const size_t dma_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    const size_t dma_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
#ifdef CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL
    ESP_LOGI(TAG, "internal DMA-capable: free=%u largest=%u (reserve=%d bytes, SPEC 9.4.1 invariant 3)",
             (unsigned)dma_free, (unsigned)dma_largest, (int)CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL);
#else
    ESP_LOGI(TAG, "internal DMA-capable: free=%u largest=%u", (unsigned)dma_free, (unsigned)dma_largest);
#endif
    void *dma = heap_caps_malloc(4092u, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    check(t, dma != NULL && esp_ptr_dma_capable(dma) && esp_ptr_internal(dma),
          "4092-byte DMA buffer allocates from internal DMA-capable RAM (SPEC 9.4.1 invariant 2)");
    free(dma);

    void *plain = malloc(200u * 1024u);
    ESP_LOGI(TAG, "plain malloc(200 KiB) -> %s  [SPEC 9.4.1: never rely on malloc placement for DMA]",
             plain == NULL ? "NULL" : (esp_ptr_external_ram(plain) ? "EXTERNAL RAM" : "internal RAM"));
    free(plain);
}

/* ---- I2S channel-creation probe (SPEC 9.4.1 bring-up check 5) ------------------------ */
/* Probe-only DMA sizing. Phase 2 derives the real sizing from the WiFi-load stress test. */
#define PROBE_DMA_DESC_NUM  6u
#define PROBE_DMA_FRAME_NUM 240u   /* multiple of 3 (24-bit data); 240 frames x 1 slot x 4 B = 960 B <= 4092 B */

static bool section_i2s_probe(tally_t *t)
{
    ESP_LOGI(TAG, "== I2S channel-creation probe (SPEC 9.4.1 #5) ==");
    i2s_chan_handle_t rx = NULL;
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.dma_desc_num = PROBE_DMA_DESC_NUM;
    cc.dma_frame_num = PROBE_DMA_FRAME_NUM;
    esp_err_t e = i2s_new_channel(&cc, NULL, &rx);
    ESP_LOGI(TAG, "i2s_new_channel(rx, dma_desc_num=%u, dma_frame_num=%u) -> %s", (unsigned)PROBE_DMA_DESC_NUM,
             (unsigned)PROBE_DMA_FRAME_NUM, esp_err_to_name(e));
    check(t, e == ESP_OK, "i2s_new_channel succeeds with PSRAM enabled (driver-owned DMA buffers internal)");
    if (e != ESP_OK) {
        return false;
    }
    i2s_std_config_t sc = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(TRUING_AUDIO_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_24BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = BOARD_I2S_MIC_BCLK_GPIO,
            .ws = BOARD_I2S_MIC_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din = BOARD_I2S_MIC_DIN_GPIO,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    sc.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;   /* 24-bit data in 32-bit slots (SPEC 9.3) */
    e = i2s_channel_init_std_mode(rx, &sc);
    ESP_LOGI(TAG, "i2s_channel_init_std_mode(48 kHz, 24-bit in 32-bit slots, bclk=%d ws=%d din=%d) -> %s",
             BOARD_I2S_MIC_BCLK_GPIO, BOARD_I2S_MIC_WS_GPIO, BOARD_I2S_MIC_DIN_GPIO, esp_err_to_name(e));
    check(t, e == ESP_OK, "I2S standard mode initialises in the fixed system audio format");
    const esp_err_t d = i2s_del_channel(rx);
    check(t, d == ESP_OK, "I2S channel deleted (probe leaves no driver state)");
    return e == ESP_OK && d == ESP_OK;
}

/* ---- core self-test: the framework-free core on the target ----------------------------- */
static truing_wheel_state_t s_ws;   /* ~7 KB: keep it off the task stack */

static bool section_core_selftest(tally_t *t)
{
    ESP_LOGI(TAG, "== core self-test (framework-free core + synthetic implementations) ==");
    const int fail_before = t->fail;
    const char *field = NULL;

    truing_row_dims_t d;
    check(t, truing_row_dims_init(&d, 32u, 32u) && d.n_full_rows == 96u, "row dims: 32 spokes -> 96 rows (SPEC 8.7)");
    check(t, truing_row_dims_init(&d, 36u, 36u) && d.n_full_rows == 108u, "row dims: 36 spokes -> 108 rows");

    truing_wheel_class_config_t wheel;
    truing_solver_config_t solver;
    truing_chain_profile_t chain;
    truing_tension_model_profile_t tmodel;
    truing_machine_profile_t machine;
    truing_fixture_wheel_class_sym32(&wheel);
    truing_fixture_solver_config(&solver, 32u);
    truing_fixture_chain_profile_inmp441(&chain);
    truing_fixture_tension_model_profile_complete(&tmodel);
    truing_fixture_machine_profile(&machine);
    check(t, truing_wheel_class_config_check(&wheel, &field) == TRUING_CFG_OK, "fixture wheel-class config validates");
    check(t, truing_solver_config_check(&solver, &field) == TRUING_CFG_OK, "fixture solver config validates");
    check(t, truing_config_check_pair(&wheel, &solver, &field) == TRUING_CFG_OK, "n_rim_angles = n_spokes (SPEC 8.9)");
    check(t, truing_chain_profile_check(&chain, &field) == TRUING_CFG_OK, "fixture chain profile validates (48 kHz / 24-bit)");
    check(t, truing_tension_model_profile_check(&tmodel, NULL, &field) == TRUING_CFG_OK, "complete tension-model profile validates");
    truing_tension_model_profile_t incomplete;
    uint32_t missing = 0u;
    truing_fixture_tension_model_profile_incomplete(&incomplete);
    check(t, truing_tension_model_profile_check(&incomplete, &missing, &field) == TRUING_CFG_ERR_CALIBRATION_MISSING &&
                 missing == TRUING_TMP_FIELD_L_EFF,
          "unestablished L_eff -> CALIBRATION_MISSING, never a default (SPEC 11.3.1)");
    check(t, truing_machine_profile_check(&machine, &field) == TRUING_CFG_OK, "fixture machine profile validates (SPEC 11.6)");

    uint8_t blob[TRUING_BLOB_MAX_BYTES];
    truing_wheel_class_config_t back;
    const size_t n = truing_blob_encode_wheel_class(&wheel, blob, sizeof(blob));
    check(t, n > 0u && truing_blob_decode_wheel_class(blob, n, &back) == TRUING_BLOB_OK && back.n_spokes == 32u,
          "wheel-class blob encodes and decodes on target (SPEC 11.5)");
    blob[TRUING_BLOB_HEADER_BYTES + 3u] ^= 0x01u;
    check(t, truing_blob_decode_wheel_class(blob, n, &back) == TRUING_BLOB_ERR_CRC, "corrupted blob rejected by CRC");

    /* Synthetic measurement pass through WheelState and the layout rules. */
    truing_clock_if_t clock = { boot_clock_now, NULL };
    truing_acoustic_if_t ac;
    truing_acoustic_synthetic_ctx_t actx;
    truing_acoustic_synthetic_init(&ac, &actx, clock, 32u, TRUING_TENSION_MODEL_IDEAL_STRING, 1u);
    actx.snr_db = 30.0f;
    truing_runout_if_t ro;
    truing_runout_synthetic_ctx_t rctx;
    truing_runout_synthetic_init(&ro, &rctx, clock, 32u);
    for (uint8_t i = 0; i < 32u; ++i) {
        truing_acoustic_synthetic_set_spoke(&actx, i, 1000.0f, 480.0f);
        truing_runout_synthetic_set_index(&rctx, i, 0.05f, -0.02f);
    }
    truing_wheel_state_init(&s_ws, 32u, 32u, 1u);
    bool stored = true;
    for (uint8_t i = 0; i < 32u; ++i) {
        truing_tension_estimate_t e;
        truing_runout_measurement_t m;
        truing_acoustic_measure(&ac, i, &wheel, 1u, &e);
        stored = stored && truing_wheel_state_set_spoke(&s_ws, i, &e, NULL) == TRUING_WS_OK;
        truing_runout_read_snapshot(&ro, i, truing_wheel_state_rim_angle(&s_ws, i), 1u, &m);
        stored = stored && truing_wheel_state_set_runout(&s_ws, i, &m, NULL) == TRUING_WS_OK;
    }
    check(t, stored && truing_wheel_state_is_complete(&s_ws), "32 synthetic tension + runout records stored");
    truing_row_mask_t avail;
    truing_row_dims_init(&d, 32u, 32u);
    truing_row_mask_available(&s_ws, &d, &avail);
    check(t, truing_layout_match(&avail, &d) == TRUING_LAYOUT_FULL, "complete state matches the FULL layout (SPEC 8.11 R1)");
    truing_wheel_state_summary_t summary;
    truing_wheel_state_summarize(&s_ws, &summary);
    check(t, summary.spokes_solver_admissible == 32u && summary.spokes_verification_grade == 0u,
          "provisional estimates are solver-admissible but not verification-grade (SPEC 8.6.3)");
    truing_acoustic_synthetic_set_failure(&actx, 3u, TRUING_STATUS_REJECTED, TRUING_REASON_LOW_SNR);
    {
        truing_tension_estimate_t e;
        truing_acoustic_measure(&ac, 3u, &wheel, 1u, &e);
        truing_wheel_state_set_spoke(&s_ws, 3u, &e, NULL);
    }
    truing_row_mask_available(&s_ws, &d, &avail);
    check(t, truing_layout_match(&avail, &d) == TRUING_LAYOUT_NONE, "one rejected tension row matches no layout -> PARTIAL_WHEEL_STATE");
    for (uint8_t i = 0; i < 32u; ++i) {
        truing_tension_estimate_t e;
        truing_acoustic_synthetic_set_failure(&actx, i, TRUING_STATUS_REJECTED, TRUING_REASON_NO_ONSET_DETECTED);
        truing_acoustic_measure(&ac, i, &wheel, 1u, &e);
        truing_wheel_state_set_spoke(&s_ws, i, &e, NULL);
    }
    truing_row_mask_available(&s_ws, &d, &avail);
    check(t, truing_layout_match(&avail, &d) == TRUING_LAYOUT_TENSION_ABSENT, "all tension rows rejected -> TENSION_ABSENT layout (SPEC 8.11 R4)");

    /* SPEC 7.3 wait-instance correlation. */
    truing_wait_correlator_t corr;
    truing_wait_correlator_init(&corr);
    truing_wait_prompt_t p;
    memset(&p, 0, sizeof(p));
    p.kind = TRUING_WAIT_APPLY_ADJUSTMENT;
    p.station = TRUING_STATION_ADJUSTMENT;
    p.target_index = 5u;
    p.display_turns_rev = 0.25f;
    const uint32_t wait5 = truing_wait_issue(&corr, &p);
    truing_intent_t confirm5;
    memset(&confirm5, 0, sizeof(confirm5));
    confirm5.type = TRUING_INTENT_CONFIRM_ADJUSTMENT_DONE;
    confirm5.wait_id = wait5;
    check(t, truing_intent_admissible(TRUING_STATE_WAIT_FOR_OPERATOR, &corr, &confirm5, false) == TRUING_INTENT_ADMIT_ACCEPT,
          "confirmation with the active wait_id accepted");
    truing_wait_clear(&corr);
    p.target_index = 6u;
    (void)truing_wait_issue(&corr, &p);
    check(t, truing_intent_admissible(TRUING_STATE_WAIT_FOR_OPERATOR, &corr, &confirm5, false) == TRUING_INTENT_REJECT_STALE_INTENT,
          "delayed duplicate confirmation discarded as STALE_INTENT (SPEC 7.3)");

    /* SPEC 10A: manual navigation anchors the wheel from station geometry. */
    truing_navigation_if_t nav;
    truing_navigation_manual_ctx_t nctx;
    truing_navigation_manual_init(&nav, &nctx, clock, 32u, 32u, &machine);
    truing_nav_result_t nr;
    truing_navigation_establish_reference(&nav, &nr);
    const bool ref_pending = nr.outcome == TRUING_NAV_PENDING_OPERATOR && nr.wait_kind == TRUING_WAIT_CONFIRM_SPOKE0_AT_STATION;
    truing_navigation_confirm(&nav, &nr);
    truing_nav_target_t target;
    memset(&target, 0, sizeof(target));
    target.kind = TRUING_NAV_TARGET_SPOKE;
    target.index = 4u;
    target.station = TRUING_STATION_RUNOUT;
    truing_navigation_request(&nav, &target, &nr);
    const bool pos_pending = nr.outcome == TRUING_NAV_PENDING_OPERATOR && nr.wait_kind == TRUING_WAIT_POSITION_TO_SPOKE;
    truing_navigation_confirm(&nav, &nr);
    truing_wheel_position_t wp;
    truing_navigation_query(&nav, &wp);
    check(t, ref_pending && pos_pending && wp.status == TRUING_STATUS_VALID && wp.operator_confirmed &&
                 truing_circular_distance(wp.rotation_rad, TRUING_PI / 4.0f) < 1e-4f,
          "manual navigation: prompts name feature + station; confirmation anchors rotation (SPEC 10A.6)");

    /* SPEC 10A.1: the synthetic automated implementation converts to actuator steps behind the contract. */
    truing_wheel_drive_if_t drive;
    truing_wheel_drive_fake_ctx_t dctx;
    truing_wheel_drive_fake_init(&drive, &dctx);
    truing_reason_t reason = TRUING_REASON_NONE;
    truing_wheel_drive_enable(&drive, true, &reason);
    truing_navigation_if_t anav;
    truing_navigation_synthetic_ctx_t asctx;
    truing_navigation_synthetic_init(&anav, &asctx, clock, 32u, 32u, &machine, &drive, 3200.0f, 0u);
    truing_navigation_establish_reference(&anav, &nr);
    truing_navigation_request(&anav, &target, &nr);
    check(t, nr.outcome == TRUING_NAV_DONE && dctx.last_move_steps == 400,
          "synthetic navigation: spoke 4 -> runout station = 400 of 3200 actuator steps (calibration knob, not a constant)");

    /* SPEC 6.2 session provenance. */
    truing_session_header_t hdr;
    const truing_impl_descriptor_t impls[] = {
        { ac.impl_name, ac.source_impl },
        { ro.impl_name, ro.source_impl },
        { nav.impl_name, nav.source_impl },
    };
    truing_session_header_init(&hdr, 1u, TRUING_FIRMWARE_VERSION, &wheel, &solver, chain.chain_id, machine.profile_id, 0u, NULL,
                               impls, sizeof(impls) / sizeof(impls[0]));
    ESP_LOGI(TAG, "session header: fw=%s tension_model_profile=%" PRIu32 " chain=%" PRIu32 " machine=%" PRIu32
                  " contains_non_real_implementations=%d",
             hdr.firmware_version, hdr.tension_model_profile_id, hdr.chain_profile_id, hdr.machine_profile_id,
             (int)hdr.contains_non_real_implementations);
    check(t, hdr.contains_non_real_implementations, "session using synthetic implementations is flagged (SPEC 6.2)");

    return t->fail == fail_before;
}

/* ---- configuration persistence (SPEC 11.5) -------------------------------------------- */
static bool decode_and_validate(truing_blob_kind_t kind, const uint8_t *blob, size_t len, char *summary, size_t cap)
{
    const char *field = "";
    truing_cfg_check_t c = TRUING_CFG_ERR_NULL;
    switch (kind) {
    case TRUING_BLOB_KIND_WHEEL_CLASS: {
        truing_wheel_class_config_t v;
        if (truing_blob_decode_wheel_class(blob, len, &v) != TRUING_BLOB_OK) {
            return false;
        }
        c = truing_wheel_class_config_check(&v, &field);
        snprintf(summary, cap, "n_spokes=%u asymmetric=%d", (unsigned)v.n_spokes, (int)v.asymmetric);
        break;
    }
    case TRUING_BLOB_KIND_SOLVER: {
        truing_solver_config_t v;
        if (truing_blob_decode_solver(blob, len, &v) != TRUING_BLOB_OK) {
            return false;
        }
        c = truing_solver_config_check(&v, &field);
        snprintf(summary, cap, "n_rim_angles=%u target_tension=%.0f profile=%" PRIu32, (unsigned)v.n_rim_angles,
                 (double)v.target_tension_n, v.tension_model_profile_id);
        break;
    }
    case TRUING_BLOB_KIND_CHAIN_PROFILE: {
        truing_chain_profile_t v;
        if (truing_blob_decode_chain_profile(blob, len, &v) != TRUING_BLOB_OK) {
            return false;
        }
        c = truing_chain_profile_check(&v, &field);
        snprintf(summary, cap, "chain_id=%" PRIu32 " %" PRIu32 " Hz / %u-bit", v.chain_id, v.sample_rate_hz, (unsigned)v.bit_depth);
        break;
    }
    case TRUING_BLOB_KIND_TENSION_MODEL_PROFILE: {
        truing_tension_model_profile_t v;
        if (truing_blob_decode_tension_model_profile(blob, len, &v) != TRUING_BLOB_OK) {
            return false;
        }
        uint32_t missing = 0u;
        c = truing_tension_model_profile_check(&v, &missing, &field);
        snprintf(summary, cap, "profile_id=%" PRIu32 " model=%s missing=0x%" PRIx32, v.profile_id,
                 truing_tension_model_str(v.model_name), missing);
        break;
    }
    case TRUING_BLOB_KIND_MACHINE_PROFILE: {
        truing_machine_profile_t v;
        if (truing_blob_decode_machine_profile(blob, len, &v) != TRUING_BLOB_OK) {
            return false;
        }
        c = truing_machine_profile_check(&v, &field);
        snprintf(summary, cap, "profile_id=%" PRIu32 " reference_station=%s", v.profile_id, truing_station_str(v.reference_station));
        break;
    }
    default:
        return false;
    }
    if (c != TRUING_CFG_OK) {
        snprintf(summary, cap, "INVALID: %s (%s)", truing_cfg_check_str(c), field);
        return false;
    }
    return true;
}

#if defined(TRUING_BRINGUP_PROVISION_FIXTURE_CONFIG) && TRUING_BRINGUP_PROVISION_FIXTURE_CONFIG
static size_t encode_fixture(truing_blob_kind_t kind, uint8_t *buf, size_t cap)
{
    switch (kind) {
    case TRUING_BLOB_KIND_WHEEL_CLASS: {
        truing_wheel_class_config_t v;
        truing_fixture_wheel_class_sym32(&v);
        return truing_blob_encode_wheel_class(&v, buf, cap);
    }
    case TRUING_BLOB_KIND_SOLVER: {
        truing_solver_config_t v;
        truing_fixture_solver_config(&v, 32u);
        return truing_blob_encode_solver(&v, buf, cap);
    }
    case TRUING_BLOB_KIND_CHAIN_PROFILE: {
        truing_chain_profile_t v;
        truing_fixture_chain_profile_inmp441(&v);
        return truing_blob_encode_chain_profile(&v, buf, cap);
    }
    case TRUING_BLOB_KIND_TENSION_MODEL_PROFILE: {
        truing_tension_model_profile_t v;
        truing_fixture_tension_model_profile_complete(&v);
        return truing_blob_encode_tension_model_profile(&v, buf, cap);
    }
    case TRUING_BLOB_KIND_MACHINE_PROFILE: {
        truing_machine_profile_t v;
        truing_fixture_machine_profile(&v);
        return truing_blob_encode_machine_profile(&v, buf, cap);
    }
    default:
        return 0u;
    }
}

static void provision_fixtures(tally_t *t)
{
    ESP_LOGW(TAG, "**** PROVISIONING SYNTHETIC FIXTURE CONFIGURATION INTO NVS ****");
    ESP_LOGW(TAG, "**** This describes NO real wheel and is NOT a demonstration configuration (SPEC P5). ****");
    for (unsigned k = 1u; k < (unsigned)TRUING_BLOB_KIND__COUNT; ++k) {
        const truing_blob_kind_t kind = (truing_blob_kind_t)k;
        uint8_t out[TRUING_BLOB_MAX_BYTES];
        uint8_t in[TRUING_BLOB_MAX_BYTES];
        const size_t n = encode_fixture(kind, out, sizeof(out));
        const esp_err_t se = truing_config_store_save(kind, out, n);
        size_t got = 0u;
        const esp_err_t le = truing_config_store_load(kind, in, sizeof(in), &got);
        const bool ok = n > 0u && se == ESP_OK && le == ESP_OK && got == n && memcmp(in, out, n) == 0;
        ESP_LOGI(TAG, "provision %s: %u bytes save=%s load=%s", truing_blob_kind_str(kind), (unsigned)n, esp_err_to_name(se),
                 esp_err_to_name(le));
        check(t, ok, "fixture blob persisted to NVS and read back byte-identical");
    }
}
#endif

static void section_config_store(tally_t *t, truing_bringup_report_t *r)
{
    ESP_LOGI(TAG, "== configuration persistence (SPEC 11.5) ==");
    const esp_err_t e = truing_config_store_init();
    r->nvs_ok = e == ESP_OK;
    ESP_LOGI(TAG, "nvs_flash_init -> %s", esp_err_to_name(e));
    check(t, r->nvs_ok, "NVS initialised");
    if (!r->nvs_ok) {
        return;
    }
#if defined(TRUING_BRINGUP_PROVISION_FIXTURE_CONFIG) && TRUING_BRINGUP_PROVISION_FIXTURE_CONFIG
    provision_fixtures(t);
#endif
    int present = 0;
    int valid = 0;
    for (unsigned k = 1u; k < (unsigned)TRUING_BLOB_KIND__COUNT; ++k) {
        const truing_blob_kind_t kind = (truing_blob_kind_t)k;
        uint8_t blob[TRUING_BLOB_MAX_BYTES];
        size_t len = 0u;
        const esp_err_t le = truing_config_store_load(kind, blob, sizeof(blob), &len);
        if (le == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "config %-22s not provisioned", truing_blob_kind_str(kind));
            continue;
        }
        if (le != ESP_OK) {
            ESP_LOGE(TAG, "config %-22s load failed: %s", truing_blob_kind_str(kind), esp_err_to_name(le));
            continue;
        }
        present++;
        uint32_t crc = 0u;
        (void)truing_blob_peek(blob, len, NULL, NULL, &crc);
        char summary[96];
        const bool ok = decode_and_validate(kind, blob, len, summary, sizeof(summary));
        if (ok) {
            valid++;
        }
        ESP_LOGI(TAG, "config %-22s %3u bytes crc=%08" PRIx32 " %s -> %s", truing_blob_kind_str(kind), (unsigned)len, crc,
                 ok ? "valid" : "REJECTED", summary);
    }
    r->config_provisioned = present == (int)(TRUING_BLOB_KIND__COUNT - 1u) && valid == present;
    ESP_LOGI(TAG, "configuration: %d/%u kinds provisioned, %d valid%s", present, (unsigned)(TRUING_BLOB_KIND__COUNT - 1u), valid,
             r->config_provisioned ? "" : " -- START_TRUING would not be admissible (SPEC 11.3.1)");
}

void truing_bringup_run(truing_bringup_report_t *report)
{
    tally_t t = { 0, 0 };
    truing_bringup_report_t r;
    memset(&r, 0, sizeof(r));

    ESP_LOGI(TAG, "==================== truing bring-up ====================");
    section_platform(&t, &r);
    section_memory(&t, &r);
    r.i2s_probe_ok = section_i2s_probe(&t);
    r.core_selftest_ok = section_core_selftest(&t);
    truing_bringup_artifact_section(&t.pass, &t.fail, &r.artifact_ok);
    truing_bringup_acoustic_section(&t.pass, &t.fail, &r.acoustic_ok);
    section_config_store(&t, &r);

    r.checks_passed = t.pass;
    r.checks_failed = t.fail;
    ESP_LOGI(TAG, "BRINGUP SUMMARY: passed=%d failed=%d | flash=%s psram=%s i2s_probe=%s nvs=%s core_selftest=%s artifact=%s "
                  "acoustic=%s config=%s",
             r.checks_passed, r.checks_failed, r.flash_ok ? "ok" : "FAIL", r.psram_ok ? "ok" : "FAIL",
             r.i2s_probe_ok ? "ok" : "FAIL", r.nvs_ok ? "ok" : "FAIL", r.core_selftest_ok ? "ok" : "FAIL",
             r.artifact_ok ? "ok" : "FAIL", r.acoustic_ok ? "ok" : "FAIL", r.config_provisioned ? "provisioned" : "not-provisioned");
    ESP_LOGI(TAG, "=========================================================");
    if (report != NULL) {
        *report = r;
    }
}
