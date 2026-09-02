#include "artifact_store.h"

#include <inttypes.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "truing_fixtures/fixtures.h"

static const char *TAG = "artifact";

/* ~50 KB expanded: static, internal RAM, so the per-cycle solve never touches the heap. */
static truing_artifact_t s_art;
static bool s_loaded;

const truing_artifact_t *truing_artifact_store_load_fixture(const truing_wheel_class_config_t *wheel,
                                                            const truing_solver_config_t *solver,
                                                            truing_artifact_store_status_t *status)
{
    truing_artifact_store_status_t st;
    memset(&st, 0, sizeof(st));
    st.blob_bytes = (uint32_t)fixture_sym32_artifact_blob_len;
    st.expanded_bytes = (uint32_t)sizeof(truing_artifact_t);
    const int64_t t0 = esp_timer_get_time();
    st.result = truing_artifact_load(fixture_sym32_artifact_blob, fixture_sym32_artifact_blob_len, wheel, solver, &s_art, &st.detail);
    st.load_us = (uint32_t)(esp_timer_get_time() - t0);
    s_loaded = st.result == TRUING_ART_OK;
    if (s_loaded) {
        char fp[65];
        truing_fingerprint_hex(&s_art.generating_fingerprint, fp);
        ESP_LOGW(TAG, "GOLDEN FIXTURE artifact '%s' (id %" PRIu32 ", fp %.16s...) loaded from flash: SYNTHETIC sym32 fixture wheel, "
                      "not a measured wheel", s_art.name, s_art.artifact_id, fp);
        ESP_LOGI(TAG, "n=%u nra=%u N_lat=%u N_rad=%u n_mt_identified=%d | FULL rank %u cond %.1f | TENSION_ABSENT rank %u cond %.1f | "
                      "%" PRIu32 " B compact -> %" PRIu32 " B expanded in %" PRIu32 " us",
                 (unsigned)s_art.n_spokes, (unsigned)s_art.n_rim_angles, (unsigned)s_art.N_lat, (unsigned)s_art.N_rad,
                 (int)s_art.n_mt_identified, (unsigned)s_art.layouts[0].effective_rank, (double)s_art.layouts[0].effective_condition_number,
                 (unsigned)s_art.layouts[1].effective_rank, (double)s_art.layouts[1].effective_condition_number, st.blob_bytes,
                 st.expanded_bytes, st.load_us);
    } else {
        ESP_LOGE(TAG, "artifact REJECTED: %s (%s)", truing_artifact_result_str(st.result), st.detail != NULL ? st.detail : "");
    }
    if (status != NULL) {
        *status = st;
    }
    return s_loaded ? &s_art : NULL;
}

const truing_artifact_t *truing_artifact_store_get(void)
{
    return s_loaded ? &s_art : NULL;
}
