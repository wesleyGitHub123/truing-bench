#include "truing_orch/auto_operator.h"

#include <stddef.h>
#include <string.h>

void truing_auto_operator_init(truing_auto_operator_t *a)
{
    if (a != NULL) {
        memset(a, 0, sizeof(*a));
    }
}

void truing_auto_operator_set_wheel(truing_auto_operator_t *a, uint8_t n_rim_angles, const float *lateral_mm,
                                    const float *radial_mm, float mm_per_turn)
{
    if (a == NULL) {
        return;
    }
    a->wheel.enabled = true;
    a->wheel.mm_per_turn = mm_per_turn;
    for (uint8_t k = 0; k < TRUING_MAX_RIM_ANGLES; ++k) {
        a->wheel.lateral_mm[k] = (k < n_rim_angles && lateral_mm != NULL) ? lateral_mm[k] : 0.0f;
        a->wheel.radial_mm[k] = (k < n_rim_angles && radial_mm != NULL) ? radial_mm[k] : 0.0f;
    }
}

bool truing_auto_operator_answer(truing_auto_operator_t *a, const truing_orch_snapshot_t *snap, truing_intent_t *out)
{
    if (a == NULL || snap == NULL || out == NULL || !snap->waiting) {
        return false;
    }
    const truing_wait_prompt_t *w = &snap->active_wait;
    memset(out, 0, sizeof(*out));
    out->wait_id = w->wait_id;   /* SPEC §7.3: echo the wait being answered */
    switch (w->kind) {
    case TRUING_WAIT_CONFIRM_SPOKE0_AT_STATION:
    case TRUING_WAIT_POSITION_TO_SPOKE:
    case TRUING_WAIT_POSITION_TO_RIM_INDEX:
    case TRUING_WAIT_POSITION_TO_RIM_ANGLE:
        out->type = TRUING_INTENT_CONFIRM_POSITIONED;
        a->positions_confirmed++;
        break;
    case TRUING_WAIT_ENTER_RUNOUT: {
        out->type = TRUING_INTENT_SUBMIT_RUNOUT;
        float lat = 0.0f, rad = 0.0f;
        bool have = false;
        if (a->runout_fn != NULL) {
            have = a->runout_fn(a->user, w->target_index, snap->cycle_index, &lat, &rad);
        }
        if (!have && a->wheel.enabled && w->target_index < TRUING_MAX_RIM_ANGLES) {
            lat = a->wheel.lateral_mm[w->target_index];
            rad = a->wheel.radial_mm[w->target_index];
        }
        out->payload.runout.lateral_mm = lat;
        out->payload.runout.radial_mm = rad;
        a->runouts_entered++;
        break;
    }
    case TRUING_WAIT_APPLY_ADJUSTMENT:
        out->type = TRUING_INTENT_CONFIRM_ADJUSTMENT_DONE;
        if (w->target_index < TRUING_MAX_SPOKES) {
            a->turns_applied[w->target_index] += w->display_turns_rev;
            if (a->wheel.enabled && w->target_index < TRUING_MAX_RIM_ANGLES) {
                /* Synthetic response: lateral at the spoke's rim index moves by +mm_per_turn * turns,
                 * the sense the synthetic calculation double corrects with turns = -k * lateral.
                 * Test physics only: no side dependence, no rim coupling. */
                a->wheel.lateral_mm[w->target_index] += a->wheel.mm_per_turn * w->display_turns_rev;
            }
        }
        a->adjustments_applied++;
        break;
    default:
        return false;
    }
    a->answers++;
    return true;
}

truing_orch_step_t truing_orch_run_auto(truing_orchestrator_t *o, truing_auto_operator_t *a, uint32_t max_steps,
                                        uint32_t *steps_used)
{
    truing_orch_step_t r = TRUING_ORCH_IDLE;
    uint32_t steps = 0u;
    while (steps < max_steps) {
        r = truing_orch_step(o);
        ++steps;
        if (r == TRUING_ORCH_IDLE || r == TRUING_ORCH_TERMINAL) {
            break;
        }
        if (r == TRUING_ORCH_WAITING_OPERATOR) {
            truing_orch_snapshot_t snap;
            truing_intent_t intent;
            truing_orch_snapshot(o, &snap);
            if (!truing_auto_operator_answer(a, &snap, &intent)) {
                break;
            }
            if (truing_orch_submit_intent(o, &intent, NULL) != TRUING_INTENT_ADMIT_ACCEPT) {
                break;
            }
        }
        /* DELAY / POLL_NAVIGATION / ADVANCED: the host driver simply steps again. */
    }
    if (steps_used != NULL) {
        *steps_used = steps;
    }
    return r;
}
