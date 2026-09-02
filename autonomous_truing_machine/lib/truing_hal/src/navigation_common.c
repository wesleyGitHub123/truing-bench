/* Wrappers and helpers shared by every Wheel Navigation implementation (SPEC §10A). */
#include <math.h>
#include <stddef.h>
#include <string.h>

#include "truing/wheel_geometry.h"
#include "truing_hal/navigation_if.h"

static const char *const k_outcome_str[] = { "IDLE", "DONE", "PENDING_OPERATOR", "IN_MOTION", "REFUSED", "FAULT" };
static const char *const k_target_str[] = { "unset", "spoke", "rim_index", "rim_angle" };

const char *truing_nav_outcome_str(truing_nav_outcome_t o)
{
    return (unsigned)o < sizeof(k_outcome_str) / sizeof(k_outcome_str[0]) ? k_outcome_str[o] : "?";
}

const char *truing_nav_target_kind_str(truing_nav_target_kind_t k)
{
    return (unsigned)k < sizeof(k_target_str) / sizeof(k_target_str[0]) ? k_target_str[k] : "?";
}

static void refused(truing_nav_result_t *out, truing_reason_t reason, const truing_nav_target_t *target)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->outcome = TRUING_NAV_REFUSED;
    out->reason = reason;
    if (target != NULL) {
        out->target = *target;
    }
}

void truing_navigation_request(truing_navigation_if_t *self, const truing_nav_target_t *target, truing_nav_result_t *out)
{
    if (out == NULL) {
        return;
    }
    if (self == NULL || self->request == NULL || target == NULL) {
        refused(out, TRUING_REASON_NOT_IMPLEMENTED, target);
        return;
    }
    self->request(self, target, out);
}

void truing_navigation_poll(truing_navigation_if_t *self, truing_nav_result_t *out)
{
    if (out == NULL) {
        return;
    }
    if (self == NULL || self->poll == NULL) {
        refused(out, TRUING_REASON_NOT_IMPLEMENTED, NULL);
        return;
    }
    self->poll(self, out);
}

void truing_navigation_confirm(truing_navigation_if_t *self, truing_nav_result_t *out)
{
    if (out == NULL) {
        return;
    }
    if (self == NULL || self->note_operator_confirmation == NULL) {
        refused(out, TRUING_REASON_NOT_IMPLEMENTED, NULL);
        return;
    }
    self->note_operator_confirmation(self, out);
}

void truing_navigation_establish_reference(truing_navigation_if_t *self, truing_nav_result_t *out)
{
    if (out == NULL) {
        return;
    }
    if (self == NULL || self->establish_reference == NULL) {
        refused(out, TRUING_REASON_NOT_IMPLEMENTED, NULL);
        return;
    }
    self->establish_reference(self, out);
}

void truing_navigation_query(truing_navigation_if_t *self, truing_wheel_position_t *out)
{
    if (out == NULL) {
        return;
    }
    if (self == NULL || self->query == NULL) {
        memset(out, 0, sizeof(*out));
        out->status = TRUING_STATUS_UNAVAILABLE;
        out->reason = TRUING_REASON_NOT_IMPLEMENTED;
        return;
    }
    self->query(self, out);
}

void truing_navigation_stop(truing_navigation_if_t *self)
{
    if (self != NULL && self->stop != NULL) {
        self->stop(self);
    }
}

float truing_nav_target_wheel_angle(const truing_nav_target_t *t, uint8_t n_spokes, uint8_t n_rim_angles)
{
    if (t == NULL) {
        return NAN;
    }
    switch (t->kind) {
    case TRUING_NAV_TARGET_SPOKE:
        return truing_spoke_angle(n_spokes, t->index);
    case TRUING_NAV_TARGET_RIM_INDEX:
        return truing_rim_index_angle(n_rim_angles, t->index);
    case TRUING_NAV_TARGET_RIM_ANGLE:
        return (isfinite(t->rim_angle_rad) && t->rim_angle_rad >= 0.0f && t->rim_angle_rad < TRUING_TWO_PI)
                   ? t->rim_angle_rad
                   : NAN;
    default:
        return NAN;
    }
}

truing_wait_kind_t truing_nav_target_wait_kind(truing_nav_target_kind_t kind)
{
    switch (kind) {
    case TRUING_NAV_TARGET_SPOKE:
        return TRUING_WAIT_POSITION_TO_SPOKE;
    case TRUING_NAV_TARGET_RIM_INDEX:
        return TRUING_WAIT_POSITION_TO_RIM_INDEX;
    case TRUING_NAV_TARGET_RIM_ANGLE:
        return TRUING_WAIT_POSITION_TO_RIM_ANGLE;
    default:
        return TRUING_WAIT_KIND_UNSET;
    }
}

bool truing_nav_result_to_prompt(const truing_nav_result_t *r, truing_wait_prompt_t *prompt_out)
{
    if (r == NULL || prompt_out == NULL || r->outcome != TRUING_NAV_PENDING_OPERATOR) {
        return false;
    }
    memset(prompt_out, 0, sizeof(*prompt_out));
    prompt_out->kind = r->wait_kind;
    prompt_out->station = r->target.station;
    prompt_out->target_index = r->target.index;
    prompt_out->target_angle_rad = r->target.rim_angle_rad;
    return prompt_out->kind != TRUING_WAIT_KIND_UNSET;
}
