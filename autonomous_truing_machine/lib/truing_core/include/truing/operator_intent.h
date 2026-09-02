/**
 * @file operator_intent.h
 * Operator intents as events (SPEC §12.4), WAIT_FOR_OPERATOR prompts, wait-instance
 * correlation (SPEC §7.3), and per-intent admissibility (SPEC §12.3).
 *
 * Wait-instance correlation is the mechanism that prevents a delayed duplicate
 * confirmation from satisfying a LATER wait: every wait gets a monotonically
 * increasing wait_id, the prompt carries it, an answering intent must echo it,
 * and a mismatch is discarded and logged as STALE_INTENT. One counter, one echoed
 * field — no retry, acknowledgement or session-recovery machinery.
 *
 * Positioning prompts are the Stage 1 (manual) resolution of the Wheel
 * Navigation capability (SPEC §10A): the prompt names the wheel feature AND the
 * station that requires it, because stations are machine-profile data, not one
 * universal location.
 */
#ifndef TRUING_OPERATOR_INTENT_H
#define TRUING_OPERATOR_INTENT_H

#include <stdbool.h>
#include <stdint.h>

#include "truing/config.h"
#include "truing/params.h"
#include "truing/state_ids.h"
#include "truing/status.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TRUING_INTENT_UNSET = 0,
    TRUING_INTENT_START_TRUING,
    TRUING_INTENT_SET_PARAMETER,
    TRUING_INTENT_CONFIRM_POSITIONED,
    TRUING_INTENT_SUBMIT_RUNOUT,
    TRUING_INTENT_CONFIRM_ADJUSTMENT_DONE,
    TRUING_INTENT_ABORT,
    TRUING_INTENT_DEBUG,          /* explicitly-marked debug channel (SPEC §12.5) */
    TRUING_INTENT__COUNT
} truing_intent_type_t;

typedef struct {
    truing_intent_type_t type;
    uint32_t             wait_id;    /* 0 unless answering a wait */
    union {
        struct {
            truing_param_id_t id;
            float             value;
        } set_parameter;
        struct {
            float lateral_mm;
            float radial_mm;
        } runout;
        struct {
            uint16_t code;
            int32_t  arg;
        } debug;
    } payload;
} truing_intent_t;

/* What the operator is being asked to do (typed prompt, SPEC §7.3). */
typedef enum {
    TRUING_WAIT_KIND_UNSET = 0,
    TRUING_WAIT_CONFIRM_SPOKE0_AT_STATION,  /* SPEC §6.5 reference: spoke 0 at `station` */
    TRUING_WAIT_POSITION_TO_SPOKE,          /* rotate so spoke `target_index` is at `station` */
    TRUING_WAIT_POSITION_TO_RIM_INDEX,      /* rotate so rim index `target_index` is at `station` */
    TRUING_WAIT_POSITION_TO_RIM_ANGLE,      /* rotate so rim angle `target_angle_rad` is at `station` */
    TRUING_WAIT_ENTER_RUNOUT,               /* read dials for rim index `target_index` */
    TRUING_WAIT_APPLY_ADJUSTMENT,           /* turn nipple `target_index` by `display_turns_rev` */
    TRUING_WAIT_KIND__COUNT
} truing_wait_kind_t;

typedef struct {
    uint32_t             wait_id;              /* assigned by the correlator */
    truing_wait_kind_t   kind;
    truing_intent_type_t expected_intent;      /* derived from kind by the correlator */
    truing_station_id_t  station;              /* positioning waits: which station requires the feature */
    uint8_t              target_index;         /* spoke or rim index */
    float                target_angle_rad;     /* POSITION_TO_RIM_ANGLE: wheel-coordinate angle */
    float                display_turns_rev;    /* APPLY_ADJUSTMENT: positive = tightening */
    uint32_t             timeout_ms;           /* 0 = wait indefinitely (default) */
} truing_wait_prompt_t;

typedef struct {
    uint32_t             next_wait_id;
    bool                 active;
    truing_wait_prompt_t prompt;
    uint32_t             stale_intents_discarded;   /* logged count (SPEC §13.2 STALE_INTENT) */
} truing_wait_correlator_t;

typedef enum {
    TRUING_WAIT_MATCH_OK = 0,
    TRUING_WAIT_MATCH_NO_ACTIVE_WAIT,
    TRUING_WAIT_MATCH_STALE_INTENT,       /* wait_id does not match the active wait */
    TRUING_WAIT_MATCH_WRONG_INTENT_TYPE,  /* right wait, wrong kind of answer */
} truing_wait_match_t;

typedef enum {
    TRUING_INTENT_ADMIT_ACCEPT = 0,
    TRUING_INTENT_REJECT_STATE,             /* not admissible in this state */
    TRUING_INTENT_REJECT_NO_ACTIVE_WAIT,
    TRUING_INTENT_REJECT_STALE_INTENT,      /* log with STALE_INTENT */
    TRUING_INTENT_REJECT_WRONG_INTENT_FOR_WAIT,
    TRUING_INTENT_REJECT_PARAMETER,         /* consult truing_set_parameter_admissible() */
    TRUING_INTENT_REJECT_DEBUG_DISABLED,
    TRUING_INTENT_REJECT_UNKNOWN,
    TRUING_INTENT_REJECT_SESSION_ADMISSION, /* admissible in this state, but a precondition failed
                                               (e.g. START_TRUING with an incomplete profile, SPEC §11.3.1) */
} truing_intent_verdict_t;

void truing_wait_correlator_init(truing_wait_correlator_t *c);
/* Issues a new wait from `request` (kind, station, target, turns, timeout). The correlator
 * assigns wait_id (monotonic, starting at 1) and expected_intent. Returns 0 on error. */
uint32_t truing_wait_issue(truing_wait_correlator_t *c, const truing_wait_prompt_t *request);
void truing_wait_clear(truing_wait_correlator_t *c);
truing_intent_type_t truing_wait_expected_intent(truing_wait_kind_t kind);
bool truing_wait_kind_is_positioning(truing_wait_kind_t kind);
/* Pure check: does this intent answer the active wait? Does not mutate. */
truing_wait_match_t truing_wait_match(const truing_wait_correlator_t *c, const truing_intent_t *intent);

/* Per-intent admissibility (SPEC §12.3). Mutates the correlator only to count discarded stale intents. */
truing_intent_verdict_t truing_intent_admissible(truing_state_t state, truing_wait_correlator_t *c,
                                                 const truing_intent_t *intent, bool debug_channel_enabled);

/* Reason code for a verdict: STALE_INTENT where SPEC §13.2 defines one, else NONE. */
truing_reason_t truing_intent_verdict_reason(truing_intent_verdict_t v);

const char *truing_intent_type_str(truing_intent_type_t t);
const char *truing_wait_kind_str(truing_wait_kind_t k);
const char *truing_intent_verdict_str(truing_intent_verdict_t v);

#ifdef __cplusplus
}
#endif
#endif /* TRUING_OPERATOR_INTENT_H */
