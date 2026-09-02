/**
 * @file wheel_state.h
 * WheelState — the current model of the wheel (SPEC §5.1, §6.3): per-spoke tension
 * estimates and per-rim-index runout measurements for the current cycle.
 *
 * Pure data plus accessors. It holds measurements; it does not acquire them.
 *
 * Partial state is LEGAL (n_measured < n_spokes, SPEC §7.4). Whether partial
 * state is SOLVABLE is decided elsewhere (SPEC §8.11) — see truing/row_layout.h.
 * An unmeasured slot reads back as "no record" (NULL), never as a zero value.
 */
#ifndef TRUING_WHEEL_STATE_H
#define TRUING_WHEEL_STATE_H

#include <stdbool.h>
#include <stdint.h>

#include "truing/measurements.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Numerical float-equality guard used when a stored RunoutMeasurement's rim angle is
 * compared with the index grid angle. This is NOT a physical tolerance (those are
 * configuration); it only absorbs float rounding of 2*pi*k/n. */
#define TRUING_RIM_ANGLE_GRID_EPS_RAD 1e-4f

typedef enum {
    TRUING_WS_OK = 0,
    TRUING_WS_ERR_NULL,
    TRUING_WS_ERR_DIMENSIONS,          /* n_spokes / n_rim_angles unsupported */
    TRUING_WS_ERR_INDEX,
    TRUING_WS_ERR_RECORD,              /* record failed its structural check */
    TRUING_WS_ERR_CYCLE_MISMATCH,      /* record.cycle_index != state.cycle_index */
    TRUING_WS_ERR_RIM_ANGLE_MISMATCH,  /* record angle is not this index's grid angle */
} truing_ws_result_t;

typedef struct {
    uint8_t n_spokes;
    uint8_t n_rim_angles;
    uint8_t cycle_index;
    truing_spoke_measurement_t  spokes[TRUING_MAX_SPOKES];
    truing_runout_measurement_t runout[TRUING_MAX_RIM_ANGLES];
} truing_wheel_state_t;

typedef struct {
    uint8_t n_spokes;
    uint8_t n_rim_angles;
    uint8_t spokes_by_status[TRUING_STATUS__COUNT];   /* index TRUING_STATUS_UNSET = unmeasured */
    uint8_t runout_by_status[TRUING_STATUS__COUNT];
    uint8_t spokes_solver_admissible;                 /* valid + suspect (SPEC §8.11) */
    uint8_t runout_solver_admissible;
    uint8_t spokes_verification_grade;                /* valid only (SPEC §8.6.3) */
} truing_wheel_state_summary_t;

/* SPEC §11.1: n_spokes is 32 or 36. n_rim_angles must fit the row bound. */
bool truing_wheel_dims_supported(uint8_t n_spokes, uint8_t n_rim_angles);

truing_ws_result_t truing_wheel_state_init(truing_wheel_state_t *ws, uint8_t n_spokes,
                                           uint8_t n_rim_angles, uint8_t cycle_index);
/* Clears every measurement and stamps the new cycle (each cycle re-measures, SPEC §7.1). */
void truing_wheel_state_begin_cycle(truing_wheel_state_t *ws, uint8_t cycle_index);

/* Rim angle of a rim index: 2*pi*k / n_rim_angles, CCW from Side A, valve stem = 0 (SPEC §6.4). */
float truing_wheel_state_rim_angle(const truing_wheel_state_t *ws, uint8_t rim_index);

truing_ws_result_t truing_wheel_state_set_spoke(truing_wheel_state_t *ws, uint8_t spoke_index,
                                                const truing_tension_estimate_t *estimate,
                                                truing_check_t *check_detail);
truing_ws_result_t truing_wheel_state_set_runout(truing_wheel_state_t *ws, uint8_t rim_index,
                                                 const truing_runout_measurement_t *measurement,
                                                 truing_check_t *check_detail);

/* NULL when unmeasured. Never a synthesized value. */
const truing_tension_estimate_t *truing_wheel_state_spoke(const truing_wheel_state_t *ws, uint8_t spoke_index);
const truing_runout_measurement_t *truing_wheel_state_runout(const truing_wheel_state_t *ws, uint8_t rim_index);

bool truing_wheel_state_spoke_measured(const truing_wheel_state_t *ws, uint8_t spoke_index);
bool truing_wheel_state_runout_measured(const truing_wheel_state_t *ws, uint8_t rim_index);
uint8_t truing_wheel_state_n_spokes_measured(const truing_wheel_state_t *ws);
uint8_t truing_wheel_state_n_runout_measured(const truing_wheel_state_t *ws);
/* Every spoke and rim index carries a record of ANY status. */
bool truing_wheel_state_is_complete(const truing_wheel_state_t *ws);

void truing_wheel_state_summarize(const truing_wheel_state_t *ws, truing_wheel_state_summary_t *out);

const char *truing_ws_result_str(truing_ws_result_t r);

#ifdef __cplusplus
}
#endif
#endif /* TRUING_WHEEL_STATE_H */
