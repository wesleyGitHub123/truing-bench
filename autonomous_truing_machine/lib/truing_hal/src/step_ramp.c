/* Trapezoidal step timing (see step_ramp.h). Integer recurrence after D. Austin / Atmel AVR446.
 *
 * truing_ramp_step() is the only function that runs in ISR context and it uses no floating
 * point. truing_ramp_plan() and truing_ramp_duration_s() run in task context and may. */
#include <math.h>
#include <stddef.h>
#include <string.h>

#include "truing_hal/step_ramp.h"

/* 2·delay + rest must stay inside int32_t: bound the longest interval the plan may produce. */
#define RAMP_MAX_DELAY_TICKS (1L << 29)

static bool profile_ok(const truing_ramp_profile_t *p)
{
    return p != NULL && p->tick_hz != 0u && p->max_step_hz != 0u && p->accel_steps_s2 != 0u &&
           p->max_step_hz <= p->tick_hz / 2u;   /* at least two ticks per pulse period */
}

bool truing_ramp_plan(truing_ramp_t *r, const truing_ramp_profile_t *p, uint32_t steps)
{
    if (r == NULL) {
        return false;
    }
    memset(r, 0, sizeof(*r));
    if (!profile_ok(p) || steps == 0u || steps > (uint32_t)INT32_MAX) {
        return false;
    }
    /* First interval from rest, with AVR446's 0.676 correction of the recurrence's first step. */
    const float c0f = 0.676f * (float)p->tick_hz * sqrtf(2.0f / (float)p->accel_steps_s2);
    if (!(c0f >= 1.0f) || c0f > (float)RAMP_MAX_DELAY_TICKS) {
        return false;
    }
    const int32_t c0 = (int32_t)c0f;
    const int32_t min_delay = (int32_t)(p->tick_hz / p->max_step_hz);

    /* Pulses to reach cruise from rest, and the pulse at which deceleration must begin: halfway
     * for a move too short to reach cruise (acceleration == deceleration), else max_s_lim from
     * the end. */
    const float s_acc = (float)p->max_step_hz * (float)p->max_step_hz / (2.0f * (float)p->accel_steps_s2);
    uint32_t max_s_lim = s_acc >= (float)INT32_MAX ? (uint32_t)INT32_MAX : (uint32_t)s_acc;
    if (max_s_lim == 0u) {
        max_s_lim = 1u;
    }
    uint32_t accel_lim = steps / 2u;
    if (accel_lim == 0u) {
        accel_lim = 1u;
    }
    int32_t decel_val = accel_lim <= max_s_lim ? -(int32_t)(steps - accel_lim) : -(int32_t)max_s_lim;
    if (decel_val == 0) {
        decel_val = -1;
    }

    r->total = steps;
    r->decel_val = decel_val;
    r->decel_start = (uint32_t)((int32_t)steps + decel_val);
    r->min_delay = min_delay;
    if (c0 <= min_delay) {
        /* Cruise is slow enough to start at: no acceleration phase. */
        r->delay = min_delay;
        r->last_accel_delay = min_delay;
        r->phase = TRUING_RAMP_RUN;
    } else {
        r->delay = c0;
        r->phase = TRUING_RAMP_ACCEL;
    }
    return true;
}

/* Shorten the move to a controlled stop that starts now, needing `need` further pulses. */
static void apply_stop(truing_ramp_t *r, uint32_t need)
{
    r->stop_req = false;
    if (need == 0u) {
        need = 1u;
    }
    if (r->done + need < r->total) {
        r->total = r->done + need;
        r->decel_start = r->done;
        r->decel_val = -(int32_t)need;
    }
}

uint32_t truing_ramp_step(truing_ramp_t *r)
{
    if (r == NULL || r->phase == TRUING_RAMP_IDLE) {
        return 0u;
    }
    r->done++;
    if (r->done >= r->total) {
        r->phase = TRUING_RAMP_IDLE;
        r->stop_req = false;
        return 0u;
    }

    int32_t next = r->delay;
    switch (r->phase) {
    case TRUING_RAMP_ACCEL: {
        r->accel_count++;
        const int32_t den = 4 * r->accel_count + 1;
        const int32_t num = 2 * r->delay + r->rest;
        next = r->delay - num / den;
        r->rest = num % den;
        if (r->stop_req) {
            /* Decelerating takes as many pulses as accelerating did. */
            apply_stop(r, (uint32_t)r->accel_count);
        }
        if (r->done >= r->decel_start) {
            r->accel_count = r->decel_val;
            r->phase = TRUING_RAMP_DECEL;
        } else if (next <= r->min_delay) {
            r->last_accel_delay = next;
            r->accel_steps = r->accel_count;
            next = r->min_delay;
            r->rest = 0;
            r->phase = TRUING_RAMP_RUN;
        }
        break;
    }
    case TRUING_RAMP_RUN:
        next = r->min_delay;
        if (r->stop_req) {
            apply_stop(r, (uint32_t)r->accel_steps);
        }
        if (r->done >= r->decel_start) {
            r->accel_count = r->decel_val;
            next = r->last_accel_delay;
            r->phase = TRUING_RAMP_DECEL;
        }
        break;
    case TRUING_RAMP_DECEL:
        r->stop_req = false;   /* already stopping */
        r->accel_count++;
        if (r->accel_count < 0) {
            /* den <= -3: the quotient is negative, so the interval grows. */
            const int32_t den = 4 * r->accel_count + 1;
            const int32_t num = 2 * r->delay + r->rest;
            next = r->delay - num / den;
            r->rest = num % den;
        }
        break;
    default:
        break;
    }
    if (next < r->min_delay) {
        next = r->min_delay;
    }
    if (next < 1) {
        next = 1;
    }
    if (next > RAMP_MAX_DELAY_TICKS) {
        next = RAMP_MAX_DELAY_TICKS;
    }
    r->delay = next;
    return (uint32_t)next;
}

void truing_ramp_request_stop(truing_ramp_t *r)
{
    if (r != NULL && r->phase != TRUING_RAMP_IDLE) {
        r->stop_req = true;
    }
}

bool truing_ramp_active(const truing_ramp_t *r)
{
    return r != NULL && r->phase != TRUING_RAMP_IDLE;
}

float truing_ramp_duration_s(const truing_ramp_profile_t *p, uint32_t steps)
{
    if (!profile_ok(p)) {
        return -1.0f;
    }
    const float v = (float)p->max_step_hz;
    const float a = (float)p->accel_steps_s2;
    const float s = (float)steps;
    const float s_acc = v * v / (2.0f * a);
    if (s <= 2.0f * s_acc) {
        return 2.0f * sqrtf(s / a);   /* triangle: never reaches cruise */
    }
    return 2.0f * v / a + (s - 2.0f * s_acc) / v;
}
