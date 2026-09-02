/* Phase 1c on-target checks: the artifact's three SPEC 8.7 checks, the recorded R3 limits,
 * float32 parity of the real calculation with the host reference cases (SPEC 14.3.5), and the
 * measured cost of a per-cycle inversion on this silicon. Every number printed is measured here. */
#include "bringup_artifact.h"

#include <inttypes.h>
#include <math.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "artifact_store.h"
#include "truing/admission.h"
#include "truing/artifact.h"
#include "truing/wheel_state.h"
#include "truing_calc/calc_if.h"
#include "truing_fixtures/fixture_sym32_parity.h"
#include "truing_fixtures/fixtures.h"

static const char *TAG = "bringup";
static truing_wheel_state_t s_ws;   /* ~7 KB: static */

#define PARITY_REL 1e-4f

static void check(int *pass, int *fail, bool ok, const char *what)
{
    if (ok) {
        (*pass)++;
        ESP_LOGI(TAG, "  PASS  %s", what);
    } else {
        (*fail)++;
        ESP_LOGE(TAG, "  FAIL  %s", what);
    }
}

static bool build_state(const float *u, const float *v, const float *T)
{
    if (truing_wheel_state_init(&s_ws, 32u, 32u, 1u) != TRUING_WS_OK) {
        return false;
    }
    bool ok = true;
    for (uint8_t k = 0; k < 32u; ++k) {
        truing_runout_measurement_t m;
        memset(&m, 0, sizeof(m));
        m.meta.status = TRUING_STATUS_VALID;
        m.meta.reason_code = TRUING_REASON_NONE;
        m.meta.cycle_index = 1u;
        m.meta.timestamp_ms = 1000u;
        m.meta.source_impl = TRUING_SOURCE_SYNTHETIC;
        m.rim_angle_rad = truing_wheel_state_rim_angle(&s_ws, k);
        m.lateral_mm = u[k];
        m.radial_mm = v[k];
        ok = ok && truing_wheel_state_set_runout(&s_ws, k, &m, NULL) == TRUING_WS_OK;
    }
    for (uint8_t i = 0; i < 32u; ++i) {
        truing_tension_estimate_t e;
        memset(&e, 0, sizeof(e));
        e.meta.status = TRUING_STATUS_VALID;
        e.meta.reason_code = TRUING_REASON_NONE;
        e.meta.cycle_index = 1u;
        e.meta.timestamp_ms = 1000u;
        e.meta.source_impl = TRUING_SOURCE_SYNTHETIC;
        e.tension_n = T[i];
        e.sigma_n = NAN;
        e.model_name = TRUING_TENSION_MODEL_IDEAL_STRING;
        e.model_version = 1u;
        e.frequency.meta = e.meta;
        e.frequency.selected_frequency_hz = 480.0f;
        e.frequency.mode_identity = TRUING_MODE_ID_CONFIRMED_FUNDAMENTAL;
        e.frequency.n_candidates = 1u;
        e.frequency.candidates[0].frequency_hz = 480.0f;
        e.frequency.candidates[0].magnitude_db = -10.0f;
        e.frequency.candidates[0].prominence_db = 20.0f;
        e.frequency.snr_db = 30.0f;
        e.frequency.selection_rule_version = 1u;
        ok = ok && truing_wheel_state_set_spoke(&s_ws, i, &e, NULL) == TRUING_WS_OK;
    }
    return ok;
}

void truing_bringup_artifact_section(int *pass, int *fail, bool *ok_out)
{
    const int fail0 = *fail;
    ESP_LOGI(TAG, "-- influence artifact + real truing calculation (Phase 1c) --");
    truing_wheel_class_config_t wheel;
    truing_solver_config_t solver;
    truing_fixture_wheel_class_sym32(&wheel);
    truing_fixture_solver_config(&solver, 32u);

    truing_artifact_store_status_t st;
    const truing_artifact_t *art = truing_artifact_store_load_fixture(&wheel, &solver, &st);
    check(pass, fail, art != NULL, "golden artifact passes integrity, compatibility and shape checks on target (SPEC 8.7)");
    if (art == NULL) {
        *ok_out = false;
        return;
    }
    check(pass, fail, !art->n_mt_identified && art->T_target_present,
          "artifact records the unidentified common mode (Phase 1b finding) and carries T_target");
    check(pass, fail, art->layouts[0].effective_condition_number <= solver.max_condition_number &&
                      art->layouts[1].effective_condition_number <= solver.max_condition_number,
          "recorded per-layout conditioning within max_condition_number (SPEC 8.11 R3)");

    /* Corruption and incompatibility, on RAM copies (PSRAM so internal memory is untouched). */
    uint8_t *copy = heap_caps_malloc(fixture_sym32_artifact_blob_len, MALLOC_CAP_SPIRAM);
    truing_artifact_t *scratch = heap_caps_malloc(sizeof(truing_artifact_t), MALLOC_CAP_SPIRAM);
    if (copy != NULL && scratch != NULL) {
        const char *detail = NULL;
        memcpy(copy, fixture_sym32_artifact_blob, fixture_sym32_artifact_blob_len);
        copy[TRUING_ARTIFACT_HEADER_BYTES + 777u] ^= 0x10u;
        check(pass, fail, truing_artifact_load(copy, fixture_sym32_artifact_blob_len, &wheel, &solver, scratch, &detail) == TRUING_ART_ERR_INTEGRITY,
              "one flipped bit in the payload is rejected by content_hash (SHA-256 on target)");
        memcpy(copy, fixture_sym32_artifact_blob, fixture_sym32_artifact_blob_len);
        truing_wheel_class_config_t other = wheel;
        other.expected_influence_fingerprint.bytes[0] ^= 0x01u;
        check(pass, fail, truing_artifact_load(copy, fixture_sym32_artifact_blob_len, &other, &solver, scratch, &detail) == TRUING_ART_ERR_INCOMPATIBLE,
              "a different configured expected fingerprint is rejected (SPEC 11.4 sole authority)");
        truing_solver_config_t reshaped = solver;
        reshaped.N_rad = (uint8_t)(solver.N_rad + 1u);
        check(pass, fail, truing_artifact_load(copy, fixture_sym32_artifact_blob_len, &wheel, &reshaped, scratch, &detail) == TRUING_ART_ERR_SHAPE,
              "a shape mismatch is rejected, never reshaped (SPEC 8.7)");
    } else {
        ESP_LOGW(TAG, "  SKIP  corruption checks: PSRAM scratch allocation failed");
    }
    heap_caps_free(copy);
    heap_caps_free(scratch);

    /* Parity with the host reference cases, and the measured inversion time. */
    truing_row_dims_t d;
    truing_row_dims_init(&d, 32u, 32u);
    float worst_rel = 0.0f, worst_cost_rel = 0.0f;
    int64_t t_full = 0, t_ta = 0;
    unsigned n_full = 0u, n_ta = 0u;
    bool built = true;
    for (unsigned ci = 0; ci < FIXTURE_SYM32_PARITY_N_CASES; ++ci) {
        const fixture_sym32_parity_case_t *c = &fixture_sym32_parity_cases[ci];
        built = built && build_state(c->u_mm, c->v_mm, c->T_n);
        for (unsigned li = 0; li < 2u; ++li) {
            const fixture_sym32_parity_layout_case_t *L = &c->layouts[li];
            const truing_layout_id_t layout = L->layout == 1 ? TRUING_LAYOUT_FULL : TRUING_LAYOUT_TENSION_ABSENT;
            truing_row_mask_t mask;
            truing_row_mask_for_layout(layout, &d, &mask);
            truing_calc_residual_t res;
            float d_ls[TRUING_MAX_SPOKES];
            const int64_t t0 = esp_timer_get_time();
            const bool ok = truing_calc_residual(art, &s_ws, &mask, &res) && truing_calc_ls_invert(art, layout, &res, d_ls);
            const int64_t dt = esp_timer_get_time() - t0;
            if (layout == TRUING_LAYOUT_FULL) { t_full += dt; n_full++; } else { t_ta += dt; n_ta++; }
            built = built && ok && res.n_active_rows == (uint16_t)L->n_active_rows;
            const float cost = truing_calc_cost(&res);
            const float crel = fabsf(cost - L->cost_j) / L->cost_j;
            if (crel > worst_cost_rel) worst_cost_rel = crel;
            for (uint8_t i = 0; i < 32u; ++i) {
                if (fabsf(L->d_ls[i]) > 1e-3f) {
                    const float rel = fabsf(d_ls[i] - L->d_ls[i]) / fabsf(L->d_ls[i]);
                    if (rel > worst_rel) worst_rel = rel;
                }
            }
        }
    }
    check(pass, fail, built, "parity states built and inverted for every case and layout");
    check(pass, fail, worst_rel <= PARITY_REL && worst_cost_rel <= 1e-5f,
          "float32 inversion matches the host float64 reference (d_ls rel <= 1e-4, J rel <= 1e-5)");
    ESP_LOGI(TAG, "parity: %u cases x 2 layouts, worst |d_ls| rel deviation %.3g, worst J rel %.3g | residual+inversion on target: "
                  "FULL (32x96) %.0f us, TENSION_ABSENT (32x64) %.0f us per solve",
             (unsigned)FIXTURE_SYM32_PARITY_N_CASES, (double)worst_rel, (double)worst_cost_rel,
             n_full ? (double)t_full / n_full : 0.0, n_ta ? (double)t_ta / n_ta : 0.0);

    /* Contract behaviour and the exact linear world through the full solve. */
    truing_calc_if_t calc;
    truing_calc_artifact_ctx_t cctx;
    truing_calc_artifact_init(&calc, &cctx, art, &wheel);
    truing_reason_t policy = TRUING_REASON_NONE, why = TRUING_REASON_NONE;
    const truing_layout_id_t chosen = calc.select_layout(&calc, false, &policy);
    check(pass, fail, chosen == TRUING_LAYOUT_TENSION_ABSENT && policy == TRUING_REASON_MEAN_TENSION_MODEL_UNAVAILABLE,
          "unidentified n_mt -> TENSION_ABSENT with MEAN_TENSION_MODEL_UNAVAILABLE recorded (SPEC 8.4, 8.11)");
    const fixture_sym32_parity_case_t *c0 = &fixture_sym32_parity_cases[0];
    float u[32], v[32];
    for (uint8_t k = 0; k < 32u; ++k) {
        float uu = 0.0f, vv = 0.0f;
        for (uint8_t i = 0; i < 32u; ++i) {
            uu += art->phi_u[k][i] * c0->d_applied[i];
            vv += art->phi_v[k][i] * c0->d_applied[i];
        }
        u[k] = uu;
        v[k] = vv;
    }
    bool exact = build_state(u, v, c0->T_n);
    truing_admission_t adm;
    truing_admission_evaluate(&s_ws, &d, chosen, policy, &adm);
    truing_adjustment_plan_t plan;
    const int64_t t0 = esp_timer_get_time();
    exact = exact && adm.admissible && calc.solve(&calc, &s_ws, &adm, &solver, &plan, &why);
    const int64_t t_solve = esp_timer_get_time() - t0;
    float worst_turn = 0.0f;
    for (uint8_t i = 0; i < 32u; ++i) {
        const float e = fabsf(plan.turns_rev[i] + c0->d_applied[i]);
        if (e > worst_turn) worst_turn = e;
    }
    check(pass, fail, exact && worst_turn <= 1e-4f,
          "exact linear state: full-contract solve recovers -d_applied to 1e-4 rev (TENSION_ABSENT full column rank)");
    ESP_LOGI(TAG, "contract solve (admission + residual + inversion + plan) on target: %lld us; worst |turn + d_applied| %.2e rev",
             (long long)t_solve, (double)worst_turn);
    *ok_out = *fail == fail0;
}
