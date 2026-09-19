/* Wheel Navigation -- COMPOSITE (SPEC §10A; plan A6 / B1b). See navigation_composite.h for why it
 * exists and the rules it keeps. Everything below is routing and bookkeeping; no positioning maths
 * lives here, so the wheel/station relation stays in the one place it is implemented. */
#include <stdio.h>
#include <string.h>

#include "truing_hal/navigation_composite.h"

static void refuse(truing_nav_result_t *out, truing_reason_t reason, const truing_nav_target_t *target)
{
    memset(out, 0, sizeof(*out));
    out->outcome = TRUING_NAV_REFUSED;
    out->reason = reason;
    if (target != NULL) {
        out->target = *target;
    }
}

static truing_navigation_if_t *child_of(truing_navigation_composite_ctx_t *c, truing_nav_composite_child_t which)
{
    return which == TRUING_NAV_COMPOSITE_PHYSICAL ? c->physical : (which == TRUING_NAV_COMPOSITE_SIMULATED ? c->simulated : NULL);
}

/* Rank of how far from a real wheel a source is; the composite takes the worst of its two. */
static int unreality(truing_source_impl_t s)
{
    switch (s) {
    case TRUING_SOURCE_REAL:         return 0;
    case TRUING_SOURCE_HOST_SERVICE: return 1;
    case TRUING_SOURCE_RECORDED:     return 2;
    default:                         return 3;   /* SYNTHETIC, and anything unset, is treated as the least real */
    }
}

static void comp_request(truing_navigation_if_t *self, const truing_nav_target_t *target, truing_nav_result_t *out)
{
    truing_navigation_composite_ctx_t *c = (truing_navigation_composite_ctx_t *)self->ctx;
    if (c == NULL || c->physical == NULL || c->simulated == NULL || target == NULL) {
        refuse(out, TRUING_REASON_NOT_IMPLEMENTED, target);
        return;
    }
    if ((unsigned)target->station >= (unsigned)TRUING_STATION__COUNT) {
        refuse(out, TRUING_REASON_VALUE_OUT_OF_RANGE, target);
        return;
    }
    const truing_nav_composite_child_t which =
        c->physical_station[target->station] ? TRUING_NAV_COMPOSITE_PHYSICAL : TRUING_NAV_COMPOSITE_SIMULATED;
    if (which == TRUING_NAV_COMPOSITE_SIMULATED) {
        /* The stand-in may only act once the real wheel has a reference: its believed position would
         * otherwise be a story with nothing to anchor it, and the physical authority would say "no
         * reference" while this said "done". */
        truing_wheel_position_t p;
        truing_navigation_query(c->physical, &p);
        if (!p.reference_established) {
            refuse(out, TRUING_REASON_WHEEL_REFERENCE_LOST, target);
            return;
        }
    }
    /* A new request replaces whatever was active. If that was a pending operator request on the other
     * child, drop it, so a stale confirmation cannot be routed back to it later. */
    if (c->active != TRUING_NAV_COMPOSITE_NO_CHILD && c->active != which) {
        truing_navigation_stop(child_of(c, c->active));
    }
    c->active = which;
    if (which == TRUING_NAV_COMPOSITE_PHYSICAL) {
        c->physical_requests++;
    } else {
        c->simulated_requests++;
    }
    truing_navigation_request(child_of(c, which), target, out);
    if (out->outcome == TRUING_NAV_REFUSED) {
        c->active = TRUING_NAV_COMPOSITE_NO_CHILD;
    }
}

static void comp_poll(truing_navigation_if_t *self, truing_nav_result_t *out)
{
    truing_navigation_composite_ctx_t *c = (truing_navigation_composite_ctx_t *)self->ctx;
    if (c == NULL) {
        refuse(out, TRUING_REASON_NOT_IMPLEMENTED, NULL);
        return;
    }
    if (c->active == TRUING_NAV_COMPOSITE_NO_CHILD) {
        memset(out, 0, sizeof(*out));   /* IDLE: no request active */
        return;
    }
    truing_navigation_poll(child_of(c, c->active), out);
}

static void comp_confirm(truing_navigation_if_t *self, truing_nav_result_t *out)
{
    truing_navigation_composite_ctx_t *c = (truing_navigation_composite_ctx_t *)self->ctx;
    if (c == NULL) {
        refuse(out, TRUING_REASON_NOT_IMPLEMENTED, NULL);
        return;
    }
    /* Only a physical request has an operator step. A confirmation that arrives with nothing pending,
     * or while the stand-in holds the active request, answers no request and is stale. */
    if (c->active != TRUING_NAV_COMPOSITE_PHYSICAL) {
        refuse(out, TRUING_REASON_STALE_INTENT, NULL);
        return;
    }
    truing_navigation_confirm(c->physical, out);
}

static void comp_establish_reference(truing_navigation_if_t *self, truing_nav_result_t *out)
{
    truing_navigation_composite_ctx_t *c = (truing_navigation_composite_ctx_t *)self->ctx;
    if (c == NULL || c->physical == NULL || c->simulated == NULL) {
        refuse(out, TRUING_REASON_NOT_IMPLEMENTED, NULL);
        return;
    }
    /* The stand-in "homes" instantly -- it is a fiction and costs nothing -- but the reference that
     * counts is the physical one: the operator confirms spoke 0 at the reference station. The
     * stand-in is not consulted about positions until that has happened (comp_request). */
    truing_nav_result_t sim;
    truing_navigation_establish_reference(c->simulated, &sim);
    if (sim.outcome != TRUING_NAV_DONE) {
        *out = sim;   /* it cannot home (no reference station in the machine profile): the demo cannot run */
        c->active = TRUING_NAV_COMPOSITE_NO_CHILD;
        return;
    }
    if (c->active == TRUING_NAV_COMPOSITE_SIMULATED) {
        truing_navigation_stop(c->simulated);
    }
    c->active = TRUING_NAV_COMPOSITE_PHYSICAL;
    c->physical_requests++;
    truing_navigation_establish_reference(c->physical, out);
    if (out->outcome == TRUING_NAV_REFUSED) {
        c->active = TRUING_NAV_COMPOSITE_NO_CHILD;
    }
}

static void comp_query(truing_navigation_if_t *self, truing_wheel_position_t *out)
{
    truing_navigation_composite_ctx_t *c = (truing_navigation_composite_ctx_t *)self->ctx;
    if (c == NULL || c->physical == NULL) {
        memset(out, 0, sizeof(*out));
        out->status = TRUING_STATUS_UNAVAILABLE;
        out->reason = TRUING_REASON_NOT_IMPLEMENTED;
        return;
    }
    truing_navigation_query(c->physical, out);   /* the single authority: the physical wheel only */
}

static void comp_stop(truing_navigation_if_t *self)
{
    truing_navigation_composite_ctx_t *c = (truing_navigation_composite_ctx_t *)self->ctx;
    if (c == NULL) {
        return;
    }
    truing_navigation_stop(c->physical);
    truing_navigation_stop(c->simulated);
    c->active = TRUING_NAV_COMPOSITE_NO_CHILD;
}

bool truing_navigation_composite_init(truing_navigation_if_t *self, truing_navigation_composite_ctx_t *ctx,
                                      truing_navigation_if_t *physical, truing_navigation_if_t *simulated,
                                      const truing_station_id_t *physical_stations, unsigned n_physical_stations)
{
    if (self == NULL || ctx == NULL) {
        return false;
    }
    memset(self, 0, sizeof(*self));   /* every entry point NULL: the null-safe wrappers refuse */
    memset(ctx, 0, sizeof(*ctx));
    if (physical == NULL || simulated == NULL || physical == simulated || physical_stations == NULL ||
        n_physical_stations == 0u) {
        return false;
    }
    for (unsigned i = 0u; i < n_physical_stations; ++i) {
        if ((unsigned)physical_stations[i] >= (unsigned)TRUING_STATION__COUNT || physical_stations[i] == TRUING_STATION_UNSET) {
            return false;
        }
        ctx->physical_station[physical_stations[i]] = true;
    }
    ctx->physical = physical;
    ctx->simulated = simulated;
    (void)snprintf(ctx->name, sizeof(ctx->name), "navigation_composite(physical=%s,simulated=%s)",
                   physical->impl_name != NULL ? physical->impl_name : "?",
                   simulated->impl_name != NULL ? simulated->impl_name : "?");
    self->impl_name = ctx->name;
    self->source_impl = unreality(physical->source_impl) >= unreality(simulated->source_impl) ? physical->source_impl
                                                                                               : simulated->source_impl;
    self->request = comp_request;
    self->poll = comp_poll;
    self->note_operator_confirmation = comp_confirm;
    self->establish_reference = comp_establish_reference;
    self->query = comp_query;
    self->stop = comp_stop;
    self->ctx = ctx;
    return true;
}
