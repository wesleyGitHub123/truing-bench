#include "truing/operator_intent.h"

#include <stddef.h>
#include <string.h>

static const char *const k_intent_str[TRUING_INTENT__COUNT] = {
    "UNSET", "START_TRUING", "SET_PARAMETER", "CONFIRM_POSITIONED",
    "SUBMIT_RUNOUT", "CONFIRM_ADJUSTMENT_DONE", "ABORT", "DEBUG",
};
static const char *const k_wait_kind_str[TRUING_WAIT_KIND__COUNT] = {
    "UNSET", "CONFIRM_SPOKE0_AT_STATION", "POSITION_TO_SPOKE", "POSITION_TO_RIM_INDEX",
    "POSITION_TO_RIM_ANGLE", "ENTER_RUNOUT", "APPLY_ADJUSTMENT",
};
static const char *const k_verdict_str[] = {
    "ACCEPT", "REJECT_STATE", "REJECT_NO_ACTIVE_WAIT", "REJECT_STALE_INTENT",
    "REJECT_WRONG_INTENT_FOR_WAIT", "REJECT_PARAMETER", "REJECT_DEBUG_DISABLED", "REJECT_UNKNOWN",
    "REJECT_SESSION_ADMISSION",
};

const char *truing_intent_type_str(truing_intent_type_t t)
{
    return (unsigned)t < TRUING_INTENT__COUNT ? k_intent_str[t] : "?";
}

const char *truing_wait_kind_str(truing_wait_kind_t k)
{
    return (unsigned)k < TRUING_WAIT_KIND__COUNT ? k_wait_kind_str[k] : "?";
}

const char *truing_intent_verdict_str(truing_intent_verdict_t v)
{
    return (unsigned)v < sizeof(k_verdict_str) / sizeof(k_verdict_str[0]) ? k_verdict_str[v] : "?";
}

void truing_wait_correlator_init(truing_wait_correlator_t *c)
{
    if (c == NULL) {
        return;
    }
    memset(c, 0, sizeof(*c));
    c->next_wait_id = 1u;   /* 0 is reserved for "not answering a wait" */
}

truing_intent_type_t truing_wait_expected_intent(truing_wait_kind_t kind)
{
    switch (kind) {
    case TRUING_WAIT_CONFIRM_SPOKE0_AT_STATION:
    case TRUING_WAIT_POSITION_TO_SPOKE:
    case TRUING_WAIT_POSITION_TO_RIM_INDEX:
    case TRUING_WAIT_POSITION_TO_RIM_ANGLE:
        return TRUING_INTENT_CONFIRM_POSITIONED;
    case TRUING_WAIT_ENTER_RUNOUT:
        return TRUING_INTENT_SUBMIT_RUNOUT;
    case TRUING_WAIT_APPLY_ADJUSTMENT:
        return TRUING_INTENT_CONFIRM_ADJUSTMENT_DONE;
    default:
        return TRUING_INTENT_UNSET;
    }
}

bool truing_wait_kind_is_positioning(truing_wait_kind_t kind)
{
    return truing_wait_expected_intent(kind) == TRUING_INTENT_CONFIRM_POSITIONED;
}

uint32_t truing_wait_issue(truing_wait_correlator_t *c, const truing_wait_prompt_t *request)
{
    if (c == NULL || request == NULL) {
        return 0u;
    }
    const truing_intent_type_t expected = truing_wait_expected_intent(request->kind);
    if (expected == TRUING_INTENT_UNSET) {
        return 0u;
    }
    /* A positioning prompt without a station would silently reintroduce the
     * "one universal station" assumption (SPEC §10A). */
    if (truing_wait_kind_is_positioning(request->kind) && request->station == TRUING_STATION_UNSET) {
        return 0u;
    }
    if (c->next_wait_id == 0u) {
        /* Counter exhausted: the monotonic guarantee cannot be kept, so refuse. */
        return 0u;
    }
    const uint32_t id = c->next_wait_id++;
    c->active = true;
    c->prompt = *request;
    c->prompt.wait_id = id;
    c->prompt.expected_intent = expected;
    return id;
}

void truing_wait_clear(truing_wait_correlator_t *c)
{
    if (c == NULL) {
        return;
    }
    c->active = false;
    memset(&c->prompt, 0, sizeof(c->prompt));
}

truing_wait_match_t truing_wait_match(const truing_wait_correlator_t *c, const truing_intent_t *intent)
{
    if (c == NULL || intent == NULL || !c->active) {
        return TRUING_WAIT_MATCH_NO_ACTIVE_WAIT;
    }
    if (intent->wait_id != c->prompt.wait_id) {
        return TRUING_WAIT_MATCH_STALE_INTENT;
    }
    if (intent->type != c->prompt.expected_intent) {
        return TRUING_WAIT_MATCH_WRONG_INTENT_TYPE;
    }
    return TRUING_WAIT_MATCH_OK;
}

truing_intent_verdict_t truing_intent_admissible(truing_state_t state, truing_wait_correlator_t *c,
                                                 const truing_intent_t *intent, bool debug_channel_enabled)
{
    if (intent == NULL) {
        return TRUING_INTENT_REJECT_UNKNOWN;
    }
    switch (intent->type) {
    case TRUING_INTENT_START_TRUING:
        /* SPEC §12.3: READY only, rejected elsewhere. Carries no wait_id. */
        return state == TRUING_STATE_READY ? TRUING_INTENT_ADMIT_ACCEPT : TRUING_INTENT_REJECT_STATE;

    case TRUING_INTENT_ABORT:
        /* Safety intent: accepted whenever the machine is operating; effected at the next safe point. */
        return truing_state_is_operational(state) ? TRUING_INTENT_ADMIT_ACCEPT : TRUING_INTENT_REJECT_STATE;

    case TRUING_INTENT_SET_PARAMETER:
        return truing_set_parameter_admissible(state, intent->payload.set_parameter.id) == TRUING_SET_PARAM_ACCEPT
                   ? TRUING_INTENT_ADMIT_ACCEPT
                   : TRUING_INTENT_REJECT_PARAMETER;

    case TRUING_INTENT_CONFIRM_POSITIONED:
    case TRUING_INTENT_SUBMIT_RUNOUT:
    case TRUING_INTENT_CONFIRM_ADJUSTMENT_DONE: {
        /* Confirmations answer exactly one wait instance (SPEC §7.3, §12.3). */
        const truing_wait_match_t m = truing_wait_match(c, intent);
        if (m == TRUING_WAIT_MATCH_NO_ACTIVE_WAIT) {
            return TRUING_INTENT_REJECT_NO_ACTIVE_WAIT;
        }
        if (m == TRUING_WAIT_MATCH_STALE_INTENT) {
            if (c != NULL) {
                c->stale_intents_discarded++;
            }
            return TRUING_INTENT_REJECT_STALE_INTENT;
        }
        if (m == TRUING_WAIT_MATCH_WRONG_INTENT_TYPE) {
            return TRUING_INTENT_REJECT_WRONG_INTENT_FOR_WAIT;
        }
        if (state != TRUING_STATE_WAIT_FOR_OPERATOR) {
            return TRUING_INTENT_REJECT_STATE;
        }
        return TRUING_INTENT_ADMIT_ACCEPT;
    }

    case TRUING_INTENT_DEBUG:
        /* SPEC §12.5: development only; never in the demonstration flow. */
        return debug_channel_enabled ? TRUING_INTENT_ADMIT_ACCEPT : TRUING_INTENT_REJECT_DEBUG_DISABLED;

    default:
        return TRUING_INTENT_REJECT_UNKNOWN;
    }
}

truing_reason_t truing_intent_verdict_reason(truing_intent_verdict_t v)
{
    return v == TRUING_INTENT_REJECT_STALE_INTENT ? TRUING_REASON_STALE_INTENT : TRUING_REASON_NONE;
}
