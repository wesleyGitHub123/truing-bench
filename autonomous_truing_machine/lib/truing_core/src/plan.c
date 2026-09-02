#include "truing/plan.h"

#include <math.h>
#include <stddef.h>

float truing_plan_max_abs_turn(const truing_adjustment_plan_t *plan)
{
    if (plan == NULL || !plan->valid) {
        return 0.0f;
    }
    float m = 0.0f;
    for (uint8_t i = 0; i < plan->n_spokes && i < TRUING_MAX_SPOKES; ++i) {
        if (plan->skipped[i]) {
            continue;
        }
        const float a = fabsf(plan->turns_rev[i]);
        if (a > m) {
            m = a;
        }
    }
    return m;
}

bool truing_plan_is_unsafe(const truing_adjustment_plan_t *plan, const truing_solver_config_t *cfg, uint8_t *spoke_out)
{
    if (plan == NULL || cfg == NULL || !plan->valid) {
        return false;
    }
    for (uint8_t i = 0; i < plan->n_spokes && i < TRUING_MAX_SPOKES; ++i) {
        const float a = fabsf(plan->turns_rev[i]);
        /* A non-finite rotation is unsafe by definition: it cannot be displayed or applied. */
        if (!isfinite(a) || a > cfg->max_adjustment_revolutions) {
            if (spoke_out != NULL) {
                *spoke_out = i;
            }
            return true;
        }
    }
    return false;
}

uint8_t truing_plan_apply_deadband(truing_adjustment_plan_t *plan, const truing_solver_config_t *cfg,
                                   const truing_wheel_state_t *ws)
{
    if (plan == NULL || cfg == NULL || ws == NULL || !plan->valid) {
        return 0u;
    }
    uint8_t skipped = 0u;
    for (uint8_t i = 0; i < plan->n_spokes && i < TRUING_MAX_SPOKES; ++i) {
        plan->skipped[i] = false;
        if (fabsf(plan->turns_rev[i]) >= cfg->adjustment_deadband_rev) {
            continue;
        }
        /* Small adjustment: skip only if the local state is already within tolerance. */
        const truing_runout_measurement_t *m = (ws->n_rim_angles == ws->n_spokes) ? truing_wheel_state_runout(ws, i) : NULL;
        if (m != NULL && truing_status_is_solver_admissible(m->meta.status) &&
            fabsf(m->lateral_mm) <= cfg->tol_lateral_mm && fabsf(m->radial_mm) <= cfg->tol_radial_mm) {
            plan->skipped[i] = true;
            ++skipped;
        }
    }
    return skipped;
}

uint8_t truing_plan_count_pending(const truing_adjustment_plan_t *plan)
{
    if (plan == NULL || !plan->valid) {
        return 0u;
    }
    uint8_t n = 0u;
    for (uint8_t i = 0; i < plan->n_spokes && i < TRUING_MAX_SPOKES; ++i) {
        if (!plan->skipped[i] && plan->turns_rev[i] != 0.0f) {
            ++n;
        }
    }
    return n;
}
