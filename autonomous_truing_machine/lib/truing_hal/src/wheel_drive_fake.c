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
static truing_status_t fake_enable(truing_wheel_drive_if_t *self, bool on, truing_reason_t *reason_out)
{
    truing_wheel_drive_fake_ctx_t *ctx = (truing_wheel_drive_fake_ctx_t *)self->ctx;
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
    ctx->position_steps += steps;   /* instantaneous, deterministic */
    ctx->last_move_steps = steps;
    ctx->moves++;
    if (reason_out != NULL) {
        *reason_out = TRUING_REASON_NONE;
    }
    return TRUING_STATUS_VALID;
}

static void fake_stop(truing_wheel_drive_if_t *self)
{
    truing_wheel_drive_fake_ctx_t *ctx = (truing_wheel_drive_fake_ctx_t *)self->ctx;
    ctx->stops++;
}

static bool fake_moving(const truing_wheel_drive_if_t *self)
{
    (void)self;
    return false;   /* moves complete instantly */
}

static int32_t fake_position(const truing_wheel_drive_if_t *self)
{
    const truing_wheel_drive_fake_ctx_t *ctx = (const truing_wheel_drive_fake_ctx_t *)self->ctx;
    return ctx->position_steps;
}

void truing_wheel_drive_fake_init(truing_wheel_drive_if_t *self, truing_wheel_drive_fake_ctx_t *ctx)
{
    if (self == NULL || ctx == NULL) {
        return;
    }
    memset(ctx, 0, sizeof(*ctx));
    self->impl_name = "wheel_drive_fake";
    self->source_impl = TRUING_SOURCE_SYNTHETIC;
    self->enable = fake_enable;
    self->move_relative = fake_move;
    self->stop = fake_stop;
    self->is_moving = fake_moving;
    self->position_steps = fake_position;
    self->ctx = ctx;
}
