#include <stddef.h>
#include <string.h>

#include "truing_hal/wheel_drive_if.h"

/* ---- null-safe wrappers ----------------------------------------------------------- */
truing_status_t truing_wheel_drive_enable(truing_wheel_drive_if_t *self, bool on, truing_reason_t *reason_out)
{
    if (self == NULL || self->enable == NULL) {
        if (reason_out != NULL) {
            *reason_out = TRUING_REASON_NOT_IMPLEMENTED;
        }
        return TRUING_STATUS_UNAVAILABLE;
    }
    return self->enable(self, on, reason_out);
}

truing_status_t truing_wheel_drive_move_relative(truing_wheel_drive_if_t *self, int32_t steps, truing_reason_t *reason_out)
{
    if (self == NULL || self->move_relative == NULL) {
        if (reason_out != NULL) {
            *reason_out = TRUING_REASON_NOT_IMPLEMENTED;
        }
        return TRUING_STATUS_UNAVAILABLE;
    }
    return self->move_relative(self, steps, reason_out);
}

void truing_wheel_drive_stop(truing_wheel_drive_if_t *self)
{
    if (self != NULL && self->stop != NULL) {
        self->stop(self);
    }
}

bool truing_wheel_drive_is_moving(const truing_wheel_drive_if_t *self)
{
    return (self != NULL && self->is_moving != NULL) ? self->is_moving(self) : false;
}

int32_t truing_wheel_drive_position_steps(const truing_wheel_drive_if_t *self)
{
    return (self != NULL && self->position_steps != NULL) ? self->position_steps(self) : 0;
}

bool truing_wheel_drive_fault(const truing_wheel_drive_if_t *self, uint32_t *detail_out)
{
    if (detail_out != NULL) {
        *detail_out = 0u;
    }
    return (self != NULL && self->fault != NULL) ? self->fault(self, detail_out) : false;
}

truing_status_t truing_wheel_drive_set_motion(truing_wheel_drive_if_t *self, uint32_t max_step_hz,
                                              uint32_t accel_steps_s2, truing_reason_t *reason_out)
{
    if (self == NULL || self->set_motion == NULL) {
        if (reason_out != NULL) {
            *reason_out = TRUING_REASON_NOT_IMPLEMENTED;
        }
        return TRUING_STATUS_UNAVAILABLE;
    }
    return self->set_motion(self, max_step_hz, accel_steps_s2, reason_out);
}

truing_status_t truing_wheel_drive_zero(truing_wheel_drive_if_t *self, truing_reason_t *reason_out)
{
    if (self == NULL || self->zero == NULL) {
        if (reason_out != NULL) {
            *reason_out = TRUING_REASON_NOT_IMPLEMENTED;
        }
        return TRUING_STATUS_UNAVAILABLE;
    }
    return self->zero(self, reason_out);
}

/* ---- stub -------------------------------------------------------------------------- */
static truing_status_t stub_enable(truing_wheel_drive_if_t *self, bool on, truing_reason_t *reason_out)
{
    (void)on;
    truing_wheel_drive_stub_ctx_t *ctx = (truing_wheel_drive_stub_ctx_t *)self->ctx;
    if (ctx != NULL) {
        ctx->calls++;
    }
    if (reason_out != NULL) {
        *reason_out = TRUING_REASON_NOT_IMPLEMENTED;
    }
    return TRUING_STATUS_UNAVAILABLE;
}

static truing_status_t stub_move(truing_wheel_drive_if_t *self, int32_t steps, truing_reason_t *reason_out)
{
    (void)steps;
    return stub_enable(self, false, reason_out);
}

static void stub_stop(truing_wheel_drive_if_t *self)
{
    (void)self;
}

static bool stub_moving(const truing_wheel_drive_if_t *self)
{
    (void)self;
    return false;
}

static int32_t stub_position(const truing_wheel_drive_if_t *self)
{
    (void)self;
    return 0;
}

void truing_wheel_drive_stub_init(truing_wheel_drive_if_t *self, truing_wheel_drive_stub_ctx_t *ctx)
{
    if (self == NULL || ctx == NULL) {
        return;
    }
    memset(ctx, 0, sizeof(*ctx));
    memset(self, 0, sizeof(*self));
    self->impl_name = "wheel_drive_stub";
    self->source_impl = TRUING_SOURCE_SYNTHETIC;
    self->enable = stub_enable;
    self->move_relative = stub_move;
    self->stop = stub_stop;
    self->is_moving = stub_moving;
    self->position_steps = stub_position;
    self->ctx = ctx;
}

/* ---- fake -------------------------------------------------------------------------- */
static void fake_land(truing_wheel_drive_fake_ctx_t *ctx, int32_t steps)
{
    ctx->position_steps += steps;
    ctx->pending_steps = 0;
    ctx->polls_left = 0u;
    ctx->moving = false;
}

static truing_status_t fake_enable(truing_wheel_drive_if_t *self, bool on, truing_reason_t *reason_out)
{
    truing_wheel_drive_fake_ctx_t *ctx = (truing_wheel_drive_fake_ctx_t *)self->ctx;
    if (!on && ctx->moving) {
        fake_land(ctx, 0);   /* hard disable: no further pulses */
    }
    if (on) {
        ctx->faulted = false;   /* every enable(true) clears a latched fault, not just a fresh transition */
    }
    ctx->enabled = on;
    if (reason_out != NULL) {
        *reason_out = TRUING_REASON_NONE;
    }
    return TRUING_STATUS_VALID;
}

static truing_status_t fake_move(truing_wheel_drive_if_t *self, int32_t steps, truing_reason_t *reason_out)
{
    truing_wheel_drive_fake_ctx_t *ctx = (truing_wheel_drive_fake_ctx_t *)self->ctx;
    if (!ctx->enabled) {
        if (reason_out != NULL) {
            *reason_out = TRUING_REASON_SENSOR_TIMEOUT;   /* driver disabled: motion refused */
        }
        return TRUING_STATUS_UNAVAILABLE;
    }
    if (ctx->moving) {
        ctx->refused_busy++;
        if (reason_out != NULL) {
            *reason_out = TRUING_REASON_VALUE_OUT_OF_RANGE;   /* never queued or merged */
        }
        return TRUING_STATUS_UNAVAILABLE;
    }
    if (ctx->fault_next) {
        ctx->fault_next = false;
        ctx->faulted = true;
    }
    ctx->last_move_steps = steps;
    ctx->moves++;
    if (ctx->polls_per_move == 0u && !ctx->stall) {
        fake_land(ctx, steps);   /* instantaneous, deterministic */
    } else {
        ctx->moving = true;
        ctx->pending_steps = steps;
        ctx->polls_left = ctx->polls_per_move;
    }
    if (reason_out != NULL) {
        *reason_out = TRUING_REASON_NONE;
    }
    return TRUING_STATUS_VALID;
}

static void fake_stop(truing_wheel_drive_if_t *self)
{
    truing_wheel_drive_fake_ctx_t *ctx = (truing_wheel_drive_fake_ctx_t *)self->ctx;
    ctx->stops++;
    if (ctx->moving) {
        fake_land(ctx, ctx->pending_steps / 2);   /* decelerated partway */
    }
}

/* The move lands on the polls_per_move-th call. */
static bool fake_moving(const truing_wheel_drive_if_t *self)
{
    truing_wheel_drive_fake_ctx_t *ctx = (truing_wheel_drive_fake_ctx_t *)self->ctx;
    if (!ctx->moving) {
        return false;
    }
    if (ctx->stall) {
        return true;
    }
    if (ctx->polls_left > 0u) {
        ctx->polls_left--;
    }
    if (ctx->polls_left == 0u) {
        fake_land(ctx, ctx->pending_steps);
        return false;
    }
    return true;
}

static int32_t fake_position(const truing_wheel_drive_if_t *self)
{
    const truing_wheel_drive_fake_ctx_t *ctx = (const truing_wheel_drive_fake_ctx_t *)self->ctx;
    return ctx->position_steps;
}

static bool fake_fault(const truing_wheel_drive_if_t *self, uint32_t *detail_out)
{
    const truing_wheel_drive_fake_ctx_t *ctx = (const truing_wheel_drive_fake_ctx_t *)self->ctx;
    if (detail_out != NULL) {
        *detail_out = ctx->faulted ? ctx->fault_detail : 0u;
    }
    return ctx->faulted;
}

static truing_status_t fake_set_motion(truing_wheel_drive_if_t *self, uint32_t max_step_hz, uint32_t accel_steps_s2,
                                       truing_reason_t *reason_out)
{
    truing_wheel_drive_fake_ctx_t *ctx = (truing_wheel_drive_fake_ctx_t *)self->ctx;
    if (ctx->moving || max_step_hz == 0u || accel_steps_s2 == 0u) {
        if (reason_out != NULL) {
            *reason_out = TRUING_REASON_VALUE_OUT_OF_RANGE;
        }
        return TRUING_STATUS_UNAVAILABLE;
    }
    ctx->motion_max_step_hz = max_step_hz;
    ctx->motion_accel_steps_s2 = accel_steps_s2;
    if (reason_out != NULL) {
        *reason_out = TRUING_REASON_NONE;
    }
    return TRUING_STATUS_VALID;
}

static truing_status_t fake_zero(truing_wheel_drive_if_t *self, truing_reason_t *reason_out)
{
    truing_wheel_drive_fake_ctx_t *ctx = (truing_wheel_drive_fake_ctx_t *)self->ctx;
    if (ctx->moving) {
        if (reason_out != NULL) {
            *reason_out = TRUING_REASON_VALUE_OUT_OF_RANGE;
        }
        return TRUING_STATUS_UNAVAILABLE;
    }
    ctx->position_steps = 0;
    ctx->zeroed++;
    if (reason_out != NULL) {
        *reason_out = TRUING_REASON_NONE;
    }
    return TRUING_STATUS_VALID;
}

void truing_wheel_drive_fake_init(truing_wheel_drive_if_t *self, truing_wheel_drive_fake_ctx_t *ctx)
{
    if (self == NULL || ctx == NULL) {
        return;
    }
    memset(ctx, 0, sizeof(*ctx));
    memset(self, 0, sizeof(*self));
    self->impl_name = "wheel_drive_fake";
    self->source_impl = TRUING_SOURCE_SYNTHETIC;
    self->enable = fake_enable;
    self->move_relative = fake_move;
    self->stop = fake_stop;
    self->is_moving = fake_moving;
    self->position_steps = fake_position;
    self->fault = fake_fault;
    self->set_motion = fake_set_motion;
    self->zero = fake_zero;
    self->ctx = ctx;
}
