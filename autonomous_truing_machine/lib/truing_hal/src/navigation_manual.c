/* Wheel Navigation — MANUAL implementation (Stage 1, SPEC §10A).
 *
 * The human is the actuator. A request resolves to PENDING_OPERATOR with the prompt
 * kind the orchestrator must issue through WAIT_FOR_OPERATOR; the operator's
 * CONFIRM_POSITIONED (SPEC §7.3) is fed back through note_operator_confirmation(),
 * which anchors the wheel's logical rotation state from the confirmed feature and
 * the station's configured angle. Nothing here knows how the wheel was moved.
 *
 * Provenance: REAL. The position knowledge comes from a real operator on a real
 * wheel — this is the genuine Stage 1 implementation, not a placeholder. */
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

static bool target_valid(const truing_navigation_manual_ctx_t *ctx, const truing_nav_target_t *t, truing_reason_t *reason)
{
    if (truing_machine_profile_station(ctx->machine, t->station) == NULL) {
        *reason = TRUING_REASON_CALIBRATION_MISSING;   /* station not in the machine profile */
        return false;
    }
    if (!isfinite(truing_nav_target_wheel_angle(t, ctx->n_spokes, ctx->n_rim_angles))) {
        *reason = TRUING_REASON_VALUE_OUT_OF_RANGE;
        return false;
    }
    return true;
}

static void manual_request(truing_navigation_if_t *self, const truing_nav_target_t *target, truing_nav_result_t *out)
{
    truing_navigation_manual_ctx_t *ctx = (truing_navigation_manual_ctx_t *)self->ctx;
    if (ctx == NULL || target == NULL) {
        set_refused(out, TRUING_REASON_NOT_IMPLEMENTED, target);
        return;
    }
    ctx->requests++;
    truing_reason_t reason = TRUING_REASON_NONE;
    if (!target_valid(ctx, target, &reason)) {
        set_refused(out, reason, target);
        return;
    }
    memset(&ctx->active, 0, sizeof(ctx->active));
    ctx->active.outcome = TRUING_NAV_PENDING_OPERATOR;
    ctx->active.reason = TRUING_REASON_NONE;
    ctx->active.wait_kind = truing_nav_target_wait_kind(target->kind);
    ctx->active.target = *target;
    *out = ctx->active;
}

static void manual_poll(truing_navigation_if_t *self, truing_nav_result_t *out)
{
    truing_navigation_manual_ctx_t *ctx = (truing_navigation_manual_ctx_t *)self->ctx;
    if (ctx == NULL) {
        set_refused(out, TRUING_REASON_NOT_IMPLEMENTED, NULL);
        return;
    }
    *out = ctx->active;
}

static void manual_confirm(truing_navigation_if_t *self, truing_nav_result_t *out)
{
    truing_navigation_manual_ctx_t *ctx = (truing_navigation_manual_ctx_t *)self->ctx;
    if (ctx == NULL) {
        set_refused(out, TRUING_REASON_NOT_IMPLEMENTED, NULL);
        return;
    }
    if (ctx->active.outcome != TRUING_NAV_PENDING_OPERATOR) {
        /* A confirmation that answers no pending positioning request is stale. */
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
    ctx->confirmations++;
    /* The operator's confirmation anchors the logical rotation state. */
    ctx->position.status = TRUING_STATUS_VALID;
    ctx->position.reason = TRUING_REASON_NONE;
    ctx->position.reference_established = true;
    ctx->position.rotation_rad = truing_rotation_for_feature_at(theta, station->angle_rad);
    ctx->position.operator_confirmed = true;
    ctx->position.sensor_confirmed = false;   /* no sensor corroborates a manual placement */
    ctx->position.timestamp_ms = truing_clock_now_ms(&ctx->clock);
    ctx->active.outcome = TRUING_NAV_DONE;
    *out = ctx->active;
}

static void manual_establish_reference(truing_navigation_if_t *self, truing_nav_result_t *out)
{
    truing_navigation_manual_ctx_t *ctx = (truing_navigation_manual_ctx_t *)self->ctx;
    if (ctx == NULL || ctx->machine == NULL) {
        set_refused(out, TRUING_REASON_NOT_IMPLEMENTED, NULL);
        return;
    }
    /* SPEC §6.5: confirm spoke 0 is at the reference station before any cycle. */
    truing_nav_target_t t;
    memset(&t, 0, sizeof(t));
    t.kind = TRUING_NAV_TARGET_SPOKE;
    t.index = 0u;
    t.station = ctx->machine->reference_station;
    truing_reason_t reason = TRUING_REASON_NONE;
    if (!target_valid(ctx, &t, &reason)) {
        set_refused(out, reason, &t);
        return;
    }
    ctx->requests++;
    memset(&ctx->active, 0, sizeof(ctx->active));
    ctx->active.outcome = TRUING_NAV_PENDING_OPERATOR;
    ctx->active.wait_kind = TRUING_WAIT_CONFIRM_SPOKE0_AT_STATION;
    ctx->active.target = t;
    *out = ctx->active;
}

static void manual_query(truing_navigation_if_t *self, truing_wheel_position_t *out)
{
    truing_navigation_manual_ctx_t *ctx = (truing_navigation_manual_ctx_t *)self->ctx;
    if (ctx == NULL) {
        memset(out, 0, sizeof(*out));
        out->status = TRUING_STATUS_UNAVAILABLE;
        out->reason = TRUING_REASON_NOT_IMPLEMENTED;
        return;
    }
    *out = ctx->position;
}

static void manual_stop(truing_navigation_if_t *self)
{
    truing_navigation_manual_ctx_t *ctx = (truing_navigation_manual_ctx_t *)self->ctx;
    if (ctx == NULL) {
        return;
    }
    /* Nothing is in motion under software control; drop any pending request. */
    if (ctx->active.outcome == TRUING_NAV_PENDING_OPERATOR) {
        memset(&ctx->active, 0, sizeof(ctx->active));
    }
}

void truing_navigation_manual_init(truing_navigation_if_t *self, truing_navigation_manual_ctx_t *ctx,
                                   truing_clock_if_t clock, uint8_t n_spokes, uint8_t n_rim_angles,
                                   const truing_machine_profile_t *machine)
{
    if (self == NULL || ctx == NULL) {
        return;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->clock = clock;
    ctx->n_spokes = n_spokes;
    ctx->n_rim_angles = n_rim_angles;
    ctx->machine = machine;
    ctx->position.status = TRUING_STATUS_UNAVAILABLE;
    ctx->position.reason = TRUING_REASON_WHEEL_REFERENCE_LOST;   /* no reference yet */
    ctx->position.rotation_rad = NAN;
    self->impl_name = "navigation_manual";
    self->source_impl = TRUING_SOURCE_REAL;
    self->request = manual_request;
    self->poll = manual_poll;
    self->note_operator_confirmation = manual_confirm;
    self->establish_reference = manual_establish_reference;
    self->query = manual_query;
    self->stop = manual_stop;
    self->ctx = ctx;
}
