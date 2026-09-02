/**
 * @file state_ids.h
 * State identifiers of the orchestrator state machine (SPEC §7.1) and their
 * classification for command admissibility (SPEC §12.3, §12.3.1).
 *
 * Transitions are implemented by the orchestrator (Phase 1d). This header only
 * names the states so that admissibility rules and telemetry can refer to them.
 */
#ifndef TRUING_STATE_IDS_H
#define TRUING_STATE_IDS_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TRUING_STATE_BOOT = 0,
    TRUING_STATE_INITIALIZE,
    TRUING_STATE_READY,
    /* outer cycle loop */
    TRUING_STATE_MEASURE_WHEEL_STATE,
    TRUING_STATE_POSITION,
    TRUING_STATE_MEASURE_SPOKE_TENSION,
    TRUING_STATE_READ_RUNOUT,
    TRUING_STATE_WAIT_FOR_OPERATOR,
    TRUING_STATE_CHECK_SOLVER_ADMISSION,
    TRUING_STATE_REMEASURE_GAPS,
    TRUING_STATE_COMPUTE_ADJUSTMENTS,
    TRUING_STATE_COMPUTE_TARGETS,
    /* inner per-spoke apply loop */
    TRUING_STATE_POSITION_TO_SPOKE,
    TRUING_STATE_APPLY_ADJUSTMENT,
    TRUING_STATE_VERIFY,
    TRUING_STATE_EVALUATE_CONVERGENCE,
    /* terminal: carries a truing_terminal_result_t */
    TRUING_STATE_TERMINAL,
    TRUING_STATE__COUNT
} truing_state_t;

typedef enum {
    TRUING_STATE_CLASS_INACTIVE = 0,     /* BOOT, INITIALIZE, TERMINAL */
    TRUING_STATE_CLASS_READY,            /* before START_TRUING */
    TRUING_STATE_CLASS_WAIT_FOR_OPERATOR,/* session active, machine stopped at a wait */
    TRUING_STATE_CLASS_AUTONOMOUS,       /* session active, operation in flight */
} truing_state_class_t;

truing_state_class_t truing_state_class(truing_state_t s);

/* "Operating" states in which ABORT must be accepted (SPEC §12.3): every session-active state. */
bool truing_state_is_operational(truing_state_t s);

const char *truing_state_str(truing_state_t s);

#ifdef __cplusplus
}
#endif
#endif /* TRUING_STATE_IDS_H */
