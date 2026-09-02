#include "truing/state_ids.h"

static const char *const k_state_str[TRUING_STATE__COUNT] = {
    "BOOT",
    "INITIALIZE",
    "READY",
    "MEASURE_WHEEL_STATE",
    "POSITION",
    "MEASURE_SPOKE_TENSION",
    "READ_RUNOUT",
    "WAIT_FOR_OPERATOR",
    "CHECK_SOLVER_ADMISSION",
    "REMEASURE_GAPS",
    "COMPUTE_ADJUSTMENTS",
    "COMPUTE_TARGETS",
    "POSITION_TO_SPOKE",
    "APPLY_ADJUSTMENT",
    "VERIFY",
    "EVALUATE_CONVERGENCE",
    "TERMINAL",
};

const char *truing_state_str(truing_state_t s)
{
    return (unsigned)s < TRUING_STATE__COUNT ? k_state_str[s] : "?";
}

truing_state_class_t truing_state_class(truing_state_t s)
{
    switch (s) {
    case TRUING_STATE_BOOT:
    case TRUING_STATE_INITIALIZE:
    case TRUING_STATE_TERMINAL:
        return TRUING_STATE_CLASS_INACTIVE;
    case TRUING_STATE_READY:
        return TRUING_STATE_CLASS_READY;
    case TRUING_STATE_WAIT_FOR_OPERATOR:
        return TRUING_STATE_CLASS_WAIT_FOR_OPERATOR;
    case TRUING_STATE_MEASURE_WHEEL_STATE:
    case TRUING_STATE_POSITION:
    case TRUING_STATE_MEASURE_SPOKE_TENSION:
    case TRUING_STATE_READ_RUNOUT:
    case TRUING_STATE_CHECK_SOLVER_ADMISSION:
    case TRUING_STATE_REMEASURE_GAPS:
    case TRUING_STATE_COMPUTE_ADJUSTMENTS:
    case TRUING_STATE_COMPUTE_TARGETS:
    case TRUING_STATE_POSITION_TO_SPOKE:
    case TRUING_STATE_APPLY_ADJUSTMENT:
    case TRUING_STATE_VERIFY:
    case TRUING_STATE_EVALUATE_CONVERGENCE:
        return TRUING_STATE_CLASS_AUTONOMOUS;
    default:
        /* Unknown states are treated conservatively as inactive: nothing is admissible. */
        return TRUING_STATE_CLASS_INACTIVE;
    }
}

bool truing_state_is_operational(truing_state_t s)
{
    const truing_state_class_t c = truing_state_class(s);
    return c == TRUING_STATE_CLASS_WAIT_FOR_OPERATOR || c == TRUING_STATE_CLASS_AUTONOMOUS;
}
