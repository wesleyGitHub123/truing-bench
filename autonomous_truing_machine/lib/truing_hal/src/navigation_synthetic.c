/* Wheel Navigation — SYNTHETIC automated implementation (SPEC §10A, §14.5).
 *
 * Simulates a Stage 2-shaped automated drive deterministically so navigation-
 * dependent orchestration can be host-tested. It also demonstrates the coordinate
 * separation the architecture requires: the implementation converts a wheel-angle
 * delta into ACTUATOR steps using a calibration knob it owns, commands the drive,
 * and reports what it BELIEVES the wheel did — while a scripted slip lets tests
 * make the true wheel angle differ, exactly the case a real friction drive faces.
 *
 * Nothing here is a validated model of any real mechanism. */
#include <math.h>
#include <stddef.h>
#include <string.h>

#include "truing/wheel_geometry.h"
#include "truing_hal/navigation_if.h"

static void set_refused(truing_nav_result_t *out, truing_reason_t reason, const truing_nav_target_t *target)
{
    memset(out, 0, sizeof(*out));
    out->outcome = TRUING_NAV_REFUSED;
    out->reason = reason;
    if (target != NULL) {
        out->target = *target;
    }
}

static void complete_move(truing_navigation_synthetic_ctx_t *ctx)
{
    if (ctx->fault_next) {
        ctx->fault_next = false;
        ctx->active.outcome = TRUING_NAV_FAULT;
        ctx->active.reason = TRUING_REASON_WHEEL_REFERENCE_LOST;
        ctx->position.status = TRUING_STATUS_UNAVAILABLE;
        ctx->position.reason = TRUING_REASON_WHEEL_REFERENCE_LOST;
        ctx->position.reference_established = false;
        ctx->position.timestamp_ms = truing_clock_now_ms(&ctx->clock);
        return;
    }
    /* What the wheel really did (test oracle) vs. what the implementation believes: the
     * believed state is the commanded target itself, so repeated moves never accumulate
     * float drift; the true state accumulates the physical motion including any slip. */
    ctx->true_rotation_rad = truing_wrap_angle(ctx->true_rotation_rad + ctx->pending_delta_rad + ctx->scripted_slip_rad);
    ctx->scripted_slip_rad = 0.0f;
    ctx->position.rotation_rad = ctx->pending_target_rotation_rad;
    ctx->position.status = TRUING_STATUS_VALID;
    ctx->position.reason = TRUING_REASON_NONE;
    ctx->position.operator_confirmed = false;
    ctx->position.sensor_confirmed = false;   /* no sensor is modelled: commanded motion only */
    ctx->position.timestamp_ms = truing_clock_now_ms(&ctx->clock);
    ctx->pending_delta_rad = 0.0f;
    ctx->active.outcome = TRUING_NAV_DONE;
}

static void syn_request(truing_navigation_if_t *self, const truing_nav_target_t *target, truing_nav_result_t *out)
{
    truing_navigation_synthetic_ctx_t *ctx = (truing_navigation_synthetic_ctx_t *)self->ctx;
    if (ctx == NULL || target == NULL) {
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
    if (!ctx->position.reference_established) {
        /* Without a reference the requested rotation is undefined (SPEC §10A). */
        set_refused(out, TRUING_REASON_WHEEL_REFERENCE_LOST, target);
        return;
    }
    const float wanted = truing_rotation_for_feature_at(theta, station->angle_rad);
    const float delta = truing_shortest_delta(ctx->position.rotation_rad, wanted);

    if (ctx->drive != NULL) {
        if (!(ctx->steps_per_wheel_rev > 0.0f)) {
            /* Actuator-to-wheel calibration unknown: refuse rather than guess. */
            set_refused(out, TRUING_REASON_CALIBRATION_MISSING, target);
            return;
        }
        const int32_t steps = (int32_t)lroundf(delta / TRUING_TWO_PI * ctx->steps_per_wheel_rev);
        truing_reason_t reason = TRUING_REASON_NONE;
        if (truing_wheel_drive_move_relative(ctx->drive, steps, &reason) != TRUING_STATUS_VALID) {
            set_refused(out, reason != TRUING_REASON_NONE ? reason : TRUING_REASON_SENSOR_TIMEOUT, target);
            return;
        }
    }

    memset(&ctx->active, 0, sizeof(ctx->active));
    ctx->active.target = *target;
    ctx->pending_delta_rad = delta;
    ctx->pending_target_rotation_rad = wanted;
    if (ctx->polls_per_move == 0u) {
        complete_move(ctx);
    } else {
        ctx->polls_remaining = ctx->polls_per_move;
        ctx->active.outcome = TRUING_NAV_IN_MOTION;
    }
    *out = ctx->active;
}

static void syn_poll(truing_navigation_if_t *self, truing_nav_result_t *out)
{
    truing_navigation_synthetic_ctx_t *ctx = (truing_navigation_synthetic_ctx_t *)self->ctx;
    if (ctx == NULL) {
        set_refused(out, TRUING_REASON_NOT_IMPLEMENTED, NULL);
        return;
    }
    if (ctx->active.outcome == TRUING_NAV_IN_MOTION) {
        if (ctx->polls_remaining > 0u) {
            ctx->polls_remaining--;
        }
        if (ctx->polls_remaining == 0u) {
            complete_move(ctx);
        }
    }
    *out = ctx->active;
}

static void syn_confirm(truing_navigation_if_t *self, truing_nav_result_t *out)
{
    truing_navigation_synthetic_ctx_t *ctx = (truing_navigation_synthetic_ctx_t *)self->ctx;
    /* An automated implementation has no operator step to confirm. */
    set_refused(out, TRUING_REASON_STALE_INTENT, ctx != NULL ? &ctx->active.target : NULL);
}

static void syn_establish_reference(truing_navigation_if_t *self, truing_nav_result_t *out)
{
    truing_navigation_synthetic_ctx_t *ctx = (truing_navigation_synthetic_ctx_t *)self->ctx;
    if (ctx == NULL) {
        set_refused(out, TRUING_REASON_NOT_IMPLEMENTED, NULL);
        return;
    }
    const truing_station_geometry_t *ref = truing_machine_profile_station(ctx->machine, ctx->machine != NULL ? ctx->machine->reference_station : TRUING_STATION_UNSET);
    if (ref == NULL) {
        set_refused(out, TRUING_REASON_CALIBRATION_MISSING, NULL);
        return;
    }
    /* Simulated homing: spoke 0 found at the reference station. */
    memset(&ctx->active, 0, sizeof(ctx->active));
    ctx->active.target.kind = TRUING_NAV_TARGET_SPOKE;
    ctx->active.target.index = 0u;
    ctx->active.target.station = ctx->machine->reference_station;
    ctx->position.rotation_rad = truing_rotation_for_feature_at(0.0f, ref->angle_rad);
    ctx->true_rotation_rad = ctx->position.rotation_rad;
    ctx->position.status = TRUING_STATUS_VALID;
    ctx->position.reason = TRUING_REASON_NONE;
    ctx->position.reference_established = true;
    ctx->position.operator_confirmed = false;
    ctx->position.sensor_confirmed = true;   /* the (simulated) reference sensor saw spoke 0 */
    ctx->position.timestamp_ms = truing_clock_now_ms(&ctx->clock);
    ctx->active.outcome = TRUING_NAV_DONE;
    *out = ctx->active;
}

static void syn_query(truing_navigation_if_t *self, truing_wheel_position_t *out)
{
    truing_navigation_synthetic_ctx_t *ctx = (truing_navigation_synthetic_ctx_t *)self->ctx;
    if (ctx == NULL) {
        memset(out, 0, sizeof(*out));
        out->status = TRUING_STATUS_UNAVAILABLE;
        out->reason = TRUING_REASON_NOT_IMPLEMENTED;
        return;
    }
    *out = ctx->position;
}

static void syn_stop(truing_navigation_if_t *self)
{
    truing_navigation_synthetic_ctx_t *ctx = (truing_navigation_synthetic_ctx_t *)self->ctx;
    if (ctx == NULL) {
        return;
    }
    truing_wheel_drive_stop(ctx->drive);
    if (ctx->active.outcome == TRUING_NAV_IN_MOTION) {
        /* Stopped mid-move: the wheel position is no longer vouched for. */
        ctx->pending_delta_rad = 0.0f;
        ctx->polls_remaining = 0u;
        ctx->active.outcome = TRUING_NAV_FAULT;
        ctx->active.reason = TRUING_REASON_WHEEL_REFERENCE_LOST;
        ctx->position.status = TRUING_STATUS_UNAVAILABLE;
        ctx->position.reason = TRUING_REASON_WHEEL_REFERENCE_LOST;
        ctx->position.reference_established = false;
    }
}

void truing_navigation_synthetic_init(truing_navigation_if_t *self, truing_navigation_synthetic_ctx_t *ctx,
                                      truing_clock_if_t clock, uint8_t n_spokes, uint8_t n_rim_angles,
                                      const truing_machine_profile_t *machine, truing_wheel_drive_if_t *drive,
                                      float steps_per_wheel_rev, uint8_t polls_per_move)
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
    ctx->steps_per_wheel_rev = steps_per_wheel_rev;
    ctx->polls_per_move = polls_per_move;
    ctx->position.status = TRUING_STATUS_UNAVAILABLE;
    ctx->position.reason = TRUING_REASON_WHEEL_REFERENCE_LOST;
    ctx->position.rotation_rad = NAN;
    ctx->true_rotation_rad = NAN;
    self->impl_name = "navigation_synthetic";
    self->source_impl = TRUING_SOURCE_SYNTHETIC;
    self->request = syn_request;
    self->poll = syn_poll;
    self->note_operator_confirmation = syn_confirm;
    self->establish_reference = syn_establish_reference;
    self->query = syn_query;
    self->stop = syn_stop;
    self->ctx = ctx;
}

float truing_navigation_synthetic_true_rotation(const truing_navigation_synthetic_ctx_t *ctx)
{
    return ctx != NULL ? ctx->true_rotation_rad : NAN;
}
