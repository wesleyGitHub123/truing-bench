#include "truing/wheel_state.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

static const char *const k_ws_str[] = {
    "OK", "ERR_NULL", "ERR_DIMENSIONS", "ERR_INDEX", "ERR_RECORD", "ERR_CYCLE_MISMATCH", "ERR_RIM_ANGLE_MISMATCH",
};

const char *truing_ws_result_str(truing_ws_result_t r)
{
    return (unsigned)r < sizeof(k_ws_str) / sizeof(k_ws_str[0]) ? k_ws_str[r] : "?";
}

bool truing_wheel_dims_supported(uint8_t n_spokes, uint8_t n_rim_angles)
{
    const bool spokes_ok = (n_spokes == 32u || n_spokes == 36u) && n_spokes <= TRUING_MAX_SPOKES;
    const bool angles_ok = n_rim_angles >= 1u && n_rim_angles <= TRUING_MAX_RIM_ANGLES;
    return spokes_ok && angles_ok;
}

static void clear_measurements(truing_wheel_state_t *ws)
{
    memset(ws->spokes, 0, sizeof(ws->spokes));
    memset(ws->runout, 0, sizeof(ws->runout));
    for (uint8_t i = 0; i < TRUING_MAX_SPOKES; ++i) {
        ws->spokes[i].spoke_index = i;
    }
    /* A cleared runout slot still knows its rim angle (mandatory field, SPEC §6.3). */
    for (uint8_t k = 0; k < TRUING_MAX_RIM_ANGLES; ++k) {
        ws->runout[k].rim_angle_rad = truing_wheel_state_rim_angle(ws, k);
    }
}

truing_ws_result_t truing_wheel_state_init(truing_wheel_state_t *ws, uint8_t n_spokes,
                                           uint8_t n_rim_angles, uint8_t cycle_index)
{
    if (ws == NULL) {
        return TRUING_WS_ERR_NULL;
    }
    if (!truing_wheel_dims_supported(n_spokes, n_rim_angles)) {
        return TRUING_WS_ERR_DIMENSIONS;
    }
    ws->n_spokes = n_spokes;
    ws->n_rim_angles = n_rim_angles;
    ws->cycle_index = cycle_index;
    clear_measurements(ws);
    return TRUING_WS_OK;
}

void truing_wheel_state_begin_cycle(truing_wheel_state_t *ws, uint8_t cycle_index)
{
    if (ws == NULL) {
        return;
    }
    ws->cycle_index = cycle_index;
    clear_measurements(ws);
}

float truing_wheel_state_rim_angle(const truing_wheel_state_t *ws, uint8_t rim_index)
{
    if (ws == NULL || ws->n_rim_angles == 0u) {
        return NAN;
    }
    if (rim_index >= ws->n_rim_angles) {
        return NAN;
    }
    return TRUING_TWO_PI * (float)rim_index / (float)ws->n_rim_angles;
}

truing_ws_result_t truing_wheel_state_set_spoke(truing_wheel_state_t *ws, uint8_t spoke_index,
                                                const truing_tension_estimate_t *estimate,
                                                truing_check_t *check_detail)
{
    if (ws == NULL || estimate == NULL) {
        return TRUING_WS_ERR_NULL;
    }
    if (spoke_index >= ws->n_spokes) {
        return TRUING_WS_ERR_INDEX;
    }
    const truing_check_t c = truing_tension_estimate_check(estimate);
    if (check_detail != NULL) {
        *check_detail = c;
    }
    if (c != TRUING_CHECK_OK) {
        return TRUING_WS_ERR_RECORD;
    }
    if (estimate->meta.cycle_index != ws->cycle_index) {
        return TRUING_WS_ERR_CYCLE_MISMATCH;
    }
    ws->spokes[spoke_index].spoke_index = spoke_index;
    ws->spokes[spoke_index].tension = *estimate;
    return TRUING_WS_OK;
}

truing_ws_result_t truing_wheel_state_set_runout(truing_wheel_state_t *ws, uint8_t rim_index,
                                                 const truing_runout_measurement_t *measurement,
                                                 truing_check_t *check_detail)
{
    if (ws == NULL || measurement == NULL) {
        return TRUING_WS_ERR_NULL;
    }
    if (rim_index >= ws->n_rim_angles) {
        return TRUING_WS_ERR_INDEX;
    }
    const truing_check_t c = truing_runout_measurement_check(measurement);
    if (check_detail != NULL) {
        *check_detail = c;
    }
    if (c != TRUING_CHECK_OK) {
        return TRUING_WS_ERR_RECORD;
    }
    if (measurement->meta.cycle_index != ws->cycle_index) {
        return TRUING_WS_ERR_CYCLE_MISMATCH;
    }
    const float grid = truing_wheel_state_rim_angle(ws, rim_index);
    if (fabsf(measurement->rim_angle_rad - grid) > TRUING_RIM_ANGLE_GRID_EPS_RAD) {
        return TRUING_WS_ERR_RIM_ANGLE_MISMATCH;
    }
    ws->runout[rim_index] = *measurement;
    return TRUING_WS_OK;
}

const truing_tension_estimate_t *truing_wheel_state_spoke(const truing_wheel_state_t *ws, uint8_t spoke_index)
{
    if (ws == NULL || spoke_index >= ws->n_spokes) {
        return NULL;
    }
    if (ws->spokes[spoke_index].tension.meta.status == TRUING_STATUS_UNSET) {
        return NULL;
    }
    return &ws->spokes[spoke_index].tension;
}

const truing_runout_measurement_t *truing_wheel_state_runout(const truing_wheel_state_t *ws, uint8_t rim_index)
{
    if (ws == NULL || rim_index >= ws->n_rim_angles) {
        return NULL;
    }
    if (ws->runout[rim_index].meta.status == TRUING_STATUS_UNSET) {
        return NULL;
    }
    return &ws->runout[rim_index];
}

bool truing_wheel_state_spoke_measured(const truing_wheel_state_t *ws, uint8_t spoke_index)
{
    return truing_wheel_state_spoke(ws, spoke_index) != NULL;
}

bool truing_wheel_state_runout_measured(const truing_wheel_state_t *ws, uint8_t rim_index)
{
    return truing_wheel_state_runout(ws, rim_index) != NULL;
}

uint8_t truing_wheel_state_n_spokes_measured(const truing_wheel_state_t *ws)
{
    if (ws == NULL) {
        return 0;
    }
    uint8_t n = 0;
    for (uint8_t i = 0; i < ws->n_spokes; ++i) {
        if (ws->spokes[i].tension.meta.status != TRUING_STATUS_UNSET) {
            ++n;
        }
    }
    return n;
}

uint8_t truing_wheel_state_n_runout_measured(const truing_wheel_state_t *ws)
{
    if (ws == NULL) {
        return 0;
    }
    uint8_t n = 0;
    for (uint8_t k = 0; k < ws->n_rim_angles; ++k) {
        if (ws->runout[k].meta.status != TRUING_STATUS_UNSET) {
            ++n;
        }
    }
    return n;
}

bool truing_wheel_state_is_complete(const truing_wheel_state_t *ws)
{
    if (ws == NULL) {
        return false;
    }
    return truing_wheel_state_n_spokes_measured(ws) == ws->n_spokes &&
           truing_wheel_state_n_runout_measured(ws) == ws->n_rim_angles;
}

void truing_wheel_state_summarize(const truing_wheel_state_t *ws, truing_wheel_state_summary_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (ws == NULL) {
        return;
    }
    out->n_spokes = ws->n_spokes;
    out->n_rim_angles = ws->n_rim_angles;
    for (uint8_t i = 0; i < ws->n_spokes; ++i) {
        const truing_status_t s = ws->spokes[i].tension.meta.status;
        const unsigned idx = (unsigned)s < TRUING_STATUS__COUNT ? (unsigned)s : (unsigned)TRUING_STATUS_UNSET;
        out->spokes_by_status[idx]++;
        if (truing_status_is_solver_admissible(s)) {
            out->spokes_solver_admissible++;
        }
        if (truing_status_is_verification_grade(s)) {
            out->spokes_verification_grade++;
        }
    }
    for (uint8_t k = 0; k < ws->n_rim_angles; ++k) {
        const truing_status_t s = ws->runout[k].meta.status;
        const unsigned idx = (unsigned)s < TRUING_STATUS__COUNT ? (unsigned)s : (unsigned)TRUING_STATUS_UNSET;
        out->runout_by_status[idx]++;
        if (truing_status_is_solver_admissible(s)) {
            out->runout_solver_admissible++;
        }
    }
}
