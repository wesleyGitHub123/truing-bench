/* Wheel Navigation — AUTOMATED roller implementation, open loop (see navigation_roller.h). */
#include <math.h>
#include <stddef.h>
#include <string.h>

#include "truing/wheel_geometry.h"
#include "truing_hal/navigation_roller.h"

/* The deadline estimate only needs the pulse-train shape; any valid timer resolution gives the
 * same duration, so the one the target driver uses is named here. */
#define ROLLER_ESTIMATE_TICK_HZ 1000000u

bool truing_roller_calibration_valid(const truing_roller_calibration_t *c)
{
    return c != NULL && isfinite(c->steps_per_wheel_rev) && fabsf(c->steps_per_wheel_rev) >= 1.0f &&
           c->max_step_hz > 0u && c->max_step_hz <= ROLLER_ESTIMATE_TICK_HZ / 2u && c->accel_steps_s2 > 0u &&
           c->microsteps > 0u && c->evidence != NULL && c->evidence[0] != '\0';
}

static void set_refused(truing_nav_result_t *out, truing_reason_t reason, const truing_nav_target_t *target)
{
    memset(out, 0, sizeof(*out));
    out->outcome = TRUING_NAV_REFUSED;
    out->reason = reason;
    if (target != NULL) {
        out->target = *target;
    }
}

static void lose_reference(truing_navigation_roller_ctx_t *ctx)
{
    ctx->position.status = TRUING_STATUS_UNAVAILABLE;
    ctx->position.reason = TRUING_REASON_WHEEL_REFERENCE_LOST;
    ctx->position.reference_established = false;
    ctx->position.operator_confirmed = false;
    ctx->position.sensor_confirmed = false;
    ctx->position.rotation_rad = NAN;
    ctx->position.timestamp_ms = truing_clock_now_ms(&ctx->clock);
}

/* The wheel's position can no longer be vouched for (SPEC §10A.5, §10A.7). */
static void end_in_fault(truing_navigation_roller_ctx_t *ctx)
{
    truing_wheel_drive_stop(ctx->drive);
    ctx->faults++;
    lose_reference(ctx);
    ctx->active.outcome = TRUING_NAV_FAULT;
    ctx->active.reason = TRUING_REASON_WHEEL_REFERENCE_LOST;
}

float truing_navigation_roller_rotation_from_steps(const truing_navigation_roller_ctx_t *ctx, int32_t steps)
{
    if (ctx == NULL || !ctx->position.reference_established || !truing_roller_calibration_valid(&ctx->cal)) {
        return NAN;
    }
    const float turns = (float)((int64_t)steps - (int64_t)ctx->anchor_steps) / ctx->cal.steps_per_wheel_rev;
    return truing_wrap_angle(ctx->anchor_rotation_rad + TRUING_TWO_PI * turns);
}

/* Record where the wheel is believed to be: from the pulses emitted, not the target. */
static void settle(truing_navigation_roller_ctx_t *ctx)
{
    ctx->position.rotation_rad =
        truing_navigation_roller_rotation_from_steps(ctx, truing_wheel_drive_position_steps(ctx->drive));
    ctx->position.status = TRUING_STATUS_VALID;
    ctx->position.reason = TRUING_REASON_NONE;
    ctx->position.operator_confirmed = false;   /* commanded motion since the operator's anchor */
    ctx->position.sensor_confirmed = false;     /* nothing senses the wheel */
    ctx->position.timestamp_ms = truing_clock_now_ms(&ctx->clock);
}

static void roller_request(truing_navigation_if_t *self, const truing_nav_target_t *target, truing_nav_result_t *out)
{
    truing_navigation_roller_ctx_t *ctx = (truing_navigation_roller_ctx_t *)self->ctx;
    if (ctx == NULL || target == NULL || ctx->drive == NULL) {
        set_refused(out, TRUING_REASON_NOT_IMPLEMENTED, target);
        return;
    }
    ctx->requests++;
    const truing_station_geometry_t *station = truing_machine_profile_station(ctx->machine, target->station);
    if (station == NULL) {
        set_refused(out, TRUING_REASON_CALIBRATION_MISSING, target);
        return;
    }
    const float theta = truing_nav_target_wheel_angle(target, ctx->n_spokes, ctx->n_rim_angles);
    if (!isfinite(theta)) {
        set_refused(out, TRUING_REASON_VALUE_OUT_OF_RANGE, target);
        return;
    }
    if (!truing_roller_calibration_valid(&ctx->cal)) {
        /* Actuator-to-wheel calibration unknown: refuse rather than guess (SPEC §10A.4). */
        set_refused(out, TRUING_REASON_CALIBRATION_MISSING, target);
        return;
    }
    if (!ctx->position.reference_established) {
        set_refused(out, TRUING_REASON_WHEEL_REFERENCE_LOST, target);
        return;
    }
    if (ctx->active.outcome == TRUING_NAV_IN_MOTION || truing_wheel_drive_is_moving(ctx->drive)) {
        set_refused(out, TRUING_REASON_VALUE_OUT_OF_RANGE, target);   /* one move at a time */
        return;
    }

    memset(&ctx->active, 0, sizeof(ctx->active));
    ctx->active.target = *target;
    uint32_t detail = 0u;
    if (truing_wheel_drive_fault(ctx->drive, &detail)) {
        ctx->last_fault_detail = detail;
        end_in_fault(ctx);
        *out = ctx->active;
        return;
    }

    const float wanted = truing_rotation_for_feature_at(theta, station->angle_rad);
    const float delta = truing_shortest_delta(ctx->position.rotation_rad, wanted);
    const int32_t steps = (int32_t)lroundf(delta / TRUING_TWO_PI * ctx->cal.steps_per_wheel_rev);
    if (steps == 0) {
        settle(ctx);   /* already there, to the drive's resolution */
        ctx->active.outcome = TRUING_NAV_DONE;
        *out = ctx->active;
        return;
    }
    truing_reason_t reason = TRUING_REASON_NONE;
    if (truing_wheel_drive_move_relative(ctx->drive, steps, &reason) != TRUING_STATUS_VALID) {
        set_refused(out, reason != TRUING_REASON_NONE ? reason : TRUING_REASON_SENSOR_TIMEOUT, target);
        ctx->active = *out;
        return;
    }
    ctx->moves++;
    const truing_ramp_profile_t prof = { ROLLER_ESTIMATE_TICK_HZ, ctx->cal.max_step_hz, ctx->cal.accel_steps_s2 };
    const float est_s = truing_ramp_duration_s(&prof, (uint32_t)(steps < 0 ? -steps : steps));
    ctx->move_deadline_ms = truing_clock_now_ms(&ctx->clock) +
                            (uint32_t)(est_s * 1000.0f * TRUING_NAV_ROLLER_DEADLINE_FACTOR) +
                            TRUING_NAV_ROLLER_DEADLINE_MARGIN_MS;
    ctx->active.outcome = TRUING_NAV_IN_MOTION;
    *out = ctx->active;
}

static void roller_poll(truing_navigation_if_t *self, truing_nav_result_t *out)
{
    truing_navigation_roller_ctx_t *ctx = (truing_navigation_roller_ctx_t *)self->ctx;
    if (ctx == NULL) {
        set_refused(out, TRUING_REASON_NOT_IMPLEMENTED, NULL);
        return;
    }
    if (ctx->active.outcome == TRUING_NAV_IN_MOTION) {
        uint32_t detail = 0u;
        if (truing_wheel_drive_fault(ctx->drive, &detail)) {
            ctx->last_fault_detail = detail;
            end_in_fault(ctx);
        } else if (!truing_wheel_drive_is_moving(ctx->drive)) {
            settle(ctx);
            ctx->active.outcome = TRUING_NAV_DONE;
        } else if ((int32_t)(truing_clock_now_ms(&ctx->clock) - ctx->move_deadline_ms) > 0) {
            end_in_fault(ctx);   /* hung or stalled drive: never wait forever */
        }
    }
    *out = ctx->active;
}

static void roller_confirm(truing_navigation_if_t *self, truing_nav_result_t *out)
{
    truing_navigation_roller_ctx_t *ctx = (truing_navigation_roller_ctx_t *)self->ctx;
    if (ctx == NULL) {
        set_refused(out, TRUING_REASON_NOT_IMPLEMENTED, NULL);
        return;
    }
    if (ctx->active.outcome != TRUING_NAV_PENDING_OPERATOR) {
        /* Only the reference step asks the operator; any other confirmation is stale. */
        set_refused(out, TRUING_REASON_STALE_INTENT, &ctx->active.target);
        return;
    }
    const truing_nav_target_t *t = &ctx->active.target;
    const truing_station_geometry_t *station = truing_machine_profile_station(ctx->machine, t->station);
    const float theta = truing_nav_target_wheel_angle(t, ctx->n_spokes, ctx->n_rim_angles);
    if (station == NULL || !isfinite(theta)) {
        set_refused(out, TRUING_REASON_VALUE_OUT_OF_RANGE, t);
        return;
    }
    truing_reason_t reason = TRUING_REASON_NONE;
    if (truing_wheel_drive_enable(ctx->drive, true, &reason) != TRUING_STATUS_VALID) {
        set_refused(out, reason != TRUING_REASON_NONE ? reason : TRUING_REASON_SENSOR_TIMEOUT, t);
        ctx->active = *out;
        return;
    }
    uint32_t detail = 0u;
    if (truing_wheel_drive_fault(ctx->drive, &detail)) {
        ctx->last_fault_detail = detail;
        end_in_fault(ctx);
        *out = ctx->active;
        return;
    }
    /* The operator's placement anchors the rotation state to the step counter. */
    ctx->anchor_steps = truing_wheel_drive_position_steps(ctx->drive);
    ctx->anchor_rotation_rad = truing_rotation_for_feature_at(theta, station->angle_rad);
    ctx->position.status = TRUING_STATUS_VALID;
    ctx->position.reason = TRUING_REASON_NONE;
    ctx->position.reference_established = true;
    ctx->position.rotation_rad = ctx->anchor_rotation_rad;
    ctx->position.operator_confirmed = true;
    ctx->position.sensor_confirmed = false;
    ctx->position.timestamp_ms = truing_clock_now_ms(&ctx->clock);
    ctx->active.outcome = TRUING_NAV_DONE;
    *out = ctx->active;
}

static void roller_establish_reference(truing_navigation_if_t *self, truing_nav_result_t *out)
{
    truing_navigation_roller_ctx_t *ctx = (truing_navigation_roller_ctx_t *)self->ctx;
    if (ctx == NULL || ctx->machine == NULL || ctx->drive == NULL) {
        set_refused(out, TRUING_REASON_NOT_IMPLEMENTED, NULL);
        return;
    }
    truing_nav_target_t t;
    memset(&t, 0, sizeof(t));
    t.kind = TRUING_NAV_TARGET_SPOKE;
    t.index = 0u;
    t.station = ctx->machine->reference_station;
    if (truing_machine_profile_station(ctx->machine, t.station) == NULL) {
        set_refused(out, TRUING_REASON_CALIBRATION_MISSING, &t);
        return;
    }
    ctx->requests++;
    /* Release the wheel so the operator can turn it by hand, and forget the old anchor. */
    truing_wheel_drive_stop(ctx->drive);
    truing_wheel_drive_enable(ctx->drive, false, NULL);
    lose_reference(ctx);
    memset(&ctx->active, 0, sizeof(ctx->active));
    ctx->active.outcome = TRUING_NAV_PENDING_OPERATOR;
    ctx->active.wait_kind = TRUING_WAIT_CONFIRM_SPOKE0_AT_STATION;
    ctx->active.target = t;
    *out = ctx->active;
}

static void roller_query(truing_navigation_if_t *self, truing_wheel_position_t *out)
{
    truing_navigation_roller_ctx_t *ctx = (truing_navigation_roller_ctx_t *)self->ctx;
    if (ctx == NULL) {
        memset(out, 0, sizeof(*out));
        out->status = TRUING_STATUS_UNAVAILABLE;
        out->reason = TRUING_REASON_NOT_IMPLEMENTED;
        return;
    }
    *out = ctx->position;
}

static void roller_stop(truing_navigation_if_t *self)
{
    truing_navigation_roller_ctx_t *ctx = (truing_navigation_roller_ctx_t *)self->ctx;
    if (ctx == NULL) {
        return;
    }
    truing_wheel_drive_stop(ctx->drive);
    if (ctx->active.outcome == TRUING_NAV_IN_MOTION) {
        /* Stopped mid-move: where it came to rest is not a position anyone vouches for. */
        lose_reference(ctx);
        ctx->active.outcome = TRUING_NAV_FAULT;
        ctx->active.reason = TRUING_REASON_WHEEL_REFERENCE_LOST;
    } else if (ctx->active.outcome == TRUING_NAV_PENDING_OPERATOR) {
        memset(&ctx->active, 0, sizeof(ctx->active));
    }
}

void truing_navigation_roller_init(truing_navigation_if_t *self, truing_navigation_roller_ctx_t *ctx,
                                   truing_clock_if_t clock, uint8_t n_spokes, uint8_t n_rim_angles,
                                   const truing_machine_profile_t *machine, truing_wheel_drive_if_t *drive,
                                   const truing_roller_calibration_t *cal)
{
    if (self == NULL || ctx == NULL) {
        return;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->clock = clock;
    ctx->n_spokes = n_spokes;
    ctx->n_rim_angles = n_rim_angles;
    ctx->machine = machine;
    ctx->drive = drive;
    if (cal != NULL) {
        ctx->cal = *cal;
    }
    ctx->position.status = TRUING_STATUS_UNAVAILABLE;
    ctx->position.reason = TRUING_REASON_WHEEL_REFERENCE_LOST;
    ctx->position.rotation_rad = NAN;
    memset(self, 0, sizeof(*self));
    self->impl_name = "navigation_roller";
    self->source_impl = (drive != NULL && drive->source_impl == TRUING_SOURCE_REAL) ? TRUING_SOURCE_REAL
                                                                                  : TRUING_SOURCE_SYNTHETIC;
    self->request = roller_request;
    self->poll = roller_poll;
    self->note_operator_confirmation = roller_confirm;
    self->establish_reference = roller_establish_reference;
    self->query = roller_query;
    self->stop = roller_stop;
    self->ctx = ctx;
}
