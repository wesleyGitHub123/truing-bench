/* Wheel Navigation -- LASER-GUIDED roller (tension-survey demonstration image, TRUING_DEMO_LASER_NAV).
 *
 * A port of the revolution sequencer the alignment campaign ran from the PC (tools/align_revolution.py, the
 * workbench's `align trial` and reconcile_belief): see navigation_laser.h. What is deliberately NOT ported:
 * the backward re-locating scan (a likely one-blocked-width anchor error in the script, and it is the
 * script's last resort anyway) and the reboot / Wi-Fi recovery (a reboot ends the session). Anything that
 * would need them hands the spoke to the operator instead. */
#include <stdlib.h>
#include <string.h>

#include "truing_hal/navigation_laser.h"

/* ---- configuration --------------------------------------------------------------------------- */
void truing_laser_nav_config_bench_2026_10_03(truing_laser_nav_config_t *out)
{
    memset(out, 0, sizeof(*out));
    /* align_revolution.py RECIPE: (name, steps/s, steps/s^2, offset, strategy, stop distance after the trigger) */
    out->class_recipe[0] = (truing_laser_recipe_t){ "NDS-leading", 1000u, 1600u, 0, TRUING_ALIGN_STRAT_REACQUIRE, 314 };
    out->class_recipe[1] = (truing_laser_recipe_t){ "DS-leading", 800u, 1600u, -20, TRUING_ALIGN_STRAT_REACQUIRE_CLEAR, 201 };
    out->class_recipe[2] = (truing_laser_recipe_t){ "NDS-trailing", 500u, 500u, 0, TRUING_ALIGN_STRAT_STOP, 251 };
    out->class_recipe[3] = (truing_laser_recipe_t){ "DS-trailing", 500u, 600u, 0, TRUING_ALIGN_STRAT_STOP, 209 };
    /* GAP: enter-to-enter after a spoke of class c, measured on the bench wheel 2026-10-03 */
    out->gap_steps[0] = 1980;
    out->gap_steps[1] = 700;
    out->gap_steps[2] = 1965;
    out->gap_steps[3] = 3290;
    out->id_tol_steps = 250;          /* half the 700 gap is 350 */
    out->width_min = 55;              /* blocked width measured 64-68 */
    out->width_max = 80;
    out->stop_tol_steps = 20;
    out->chatter_edges = 60;          /* a trial makes ~4-12 edges; the chatter episodes made thousands */
    out->max_tries = 3;
    out->dir = 1;
    out->max_steps = 12000u;          /* session align1 */
    out->settle_ms = 300u;
    out->slow_hz = 200u;
    out->slow_accel = 1000u;
    out->reacq_max_steps = 7908u;     /* the workbench default with no rmax: 4 x the measured median pitch */
    out->backup_hz = 800u;            /* align_revolution move_before */
    out->backup_accel = 1600u;
}

/* ---- spacing ----------------------------------------------------------------------------------- */
static int32_t cls_of(int32_t spoke)
{
    const int32_t m = spoke % 4;
    return m < 0 ? m + 4 : m;
}

int32_t truing_laser_nav_offset(const truing_laser_nav_config_t *cfg, int32_t a, int32_t b)
{
    int32_t s = 0;
    if (b >= a) {
        for (int32_t i = a; i < b; ++i) {
            s += cfg->gap_steps[cls_of(i)];
        }
        return s;
    }
    for (int32_t i = b; i < a; ++i) {
        s += cfg->gap_steps[cls_of(i)];
    }
    return -s;
}

bool truing_laser_nav_identify(const truing_laser_nav_config_t *cfg, int32_t anchor_pos, int32_t anchor_spoke,
                               int32_t trigger, int32_t *spoke_out, int32_t *err_out)
{
    int32_t best = anchor_spoke - 2;
    int32_t best_err = 0;
    bool first = true;
    for (int32_t j = anchor_spoke - 2; j <= anchor_spoke + 6; ++j) {
        const int32_t err = (trigger - anchor_pos) * cfg->dir - truing_laser_nav_offset(cfg, anchor_spoke, j);
        if (first || abs(err) < abs(best_err)) {
            best = j;
            best_err = err;
            first = false;
        }
    }
    *spoke_out = best;
    *err_out = best_err;
    return abs(best_err) <= cfg->id_tol_steps;
}

/* Python's zone // 2: floor, so zone -1 (just before the spoke) is one spoke back. */
static int32_t floor_half(int32_t z)
{
    return z >= 0 ? z / 2 : -((1 - z) / 2);
}

/* ---- the sequencer ------------------------------------------------------------------------------ */
static void result_for(truing_navigation_laser_ctx_t *c, truing_nav_outcome_t o, truing_reason_t reason,
                       truing_nav_result_t *out)
{
    memset(out, 0, sizeof(*out));
    out->outcome = o;
    out->reason = reason;
    out->target = c->target;
}

static void forget_position(truing_navigation_laser_ctx_t *c)
{
    c->belief_valid = false;
    c->belief_level_known = false;
    c->anchor_valid = false;
    c->at_station = -1;
}

/* Hand spoke k to the operator: the manual implementation issues the positioning wait. */
static void hand_over(truing_navigation_laser_ctx_t *c, const char *why, truing_nav_result_t *out)
{
    c->last_why = why;
    c->handovers++;
    c->phase = TRUING_LASER_NAV_MANUAL;
    forget_position(c);
    if (c->port.release != NULL) {
        c->port.release(c->port.user);   /* the operator turns the wheel by hand: the roller lets go */
    }
    c->manual.request(&c->manual, &c->target, out);
    if (out->outcome != TRUING_NAV_PENDING_OPERATOR) {
        c->phase = TRUING_LASER_NAV_IDLE;
    }
}

static void succeed(truing_navigation_laser_ctx_t *c, bool sensor, truing_nav_result_t *out)
{
    c->phase = TRUING_LASER_NAV_DONE;
    c->tries = 0u;
    c->at_station = (int16_t)c->target.index;
    /* The manual implementation keeps the logical rotation: a confirmation anchors it. */
    truing_nav_result_t tmp;
    c->manual.request(&c->manual, &c->target, &tmp);
    c->manual.note_operator_confirmation(&c->manual, &tmp);
    c->mctx.position.operator_confirmed = !sensor;
    c->mctx.position.sensor_confirmed = sensor;
    result_for(c, TRUING_NAV_DONE, TRUING_REASON_NONE, out);
}

static void back_up_or_hand_over(truing_navigation_laser_ctx_t *c, const char *why, truing_nav_result_t *out);

/* One controller run toward spoke k from the current belief. */
static void start_trial(truing_navigation_laser_ctx_t *c, truing_nav_result_t *out)
{
    bool rescaled = false;
    if (!c->port.enable(c->port.user, &rescaled)) {
        hand_over(c, "the roller driver would not energise", out);
        return;
    }
    if (rescaled) {
        /* Every step position held here is in the old scale (a driver reset, docs/DEMO_BUILD.md). The spoke count
         * (belief) is not a step count and stands; the spacing anchor is re-taken from the next trigger. */
        c->anchor_valid = false;
        c->rescales++;
    }
    /* reconcile_belief: a spoke that entered the beam since the last stop (a strike nudged it) */
    const bool now_blocked = c->port.beam_blocked(c->port.user);
    if (c->belief_level_known && now_blocked != c->belief_blocked && now_blocked) {
        c->belief += 1;
    }
    c->belief_level_known = false;
    const int32_t j = c->k - c->belief;
    if (j < 1) {
        /* spoke k itself was pushed into the beam: not where its recipe stops it, so back up and run again */
        back_up_or_hand_over(c, "the spoke was nudged into the beam before its run", out);
        return;
    }
    if (j > 4) {
        hand_over(c, "the spoke is more than 4 spokes ahead of the one at the beam", out);
        return;
    }
    const truing_laser_recipe_t *r = &c->cfg.class_recipe[cls_of(c->k)];
    truing_align_config_t ac;
    memset(&ac, 0, sizeof(ac));
    ac.mode = TRUING_ALIGN_MODE_ALIGN;
    ac.strategy = r->strategy;
    ac.dir = c->cfg.dir;
    ac.target_break = (uint16_t)j;
    ac.max_steps = c->cfg.max_steps;
    ac.settle_ms = c->cfg.settle_ms;
    ac.offset_steps = r->offset_steps;
    ac.center = false;
    ac.slow_hz = c->cfg.slow_hz;
    ac.slow_accel = c->cfg.slow_accel;
    ac.reacq_max_steps = c->cfg.reacq_max_steps;
    c->edges0 = c->port.beam_edges(c->port.user);
    if (!c->port.start_trial(c->port.user, &ac, r->hz, r->accel)) {
        hand_over(c, "the alignment controller refused the run", out);
        return;
    }
    c->trials++;
    c->phase = TRUING_LASER_NAV_TRIAL;
    result_for(c, TRUING_NAV_IN_MOTION, TRUING_REASON_NONE, out);
}

/* align_revolution move_before: to the middle of the gap before spoke k, then declare spoke k-1 behind. */
static void back_up_or_hand_over(truing_navigation_laser_ctx_t *c, const char *why, truing_nav_result_t *out)
{
    c->last_why = why;
    c->tries++;
    if (c->tries >= c->cfg.max_tries || !c->anchor_valid) {
        hand_over(c, why, out);
        return;
    }
    const int32_t target = c->anchor_pos +
        c->cfg.dir * (truing_laser_nav_offset(&c->cfg, c->anchor_spoke, c->k) - c->cfg.gap_steps[cls_of(c->k - 1)] / 2);
    const int32_t here = c->port.position(c->port.user);
    if (!c->port.move(c->port.user, target - here, c->cfg.backup_hz, c->cfg.backup_accel)) {
        hand_over(c, "the roller refused the back-up move", out);
        return;
    }
    c->backups++;
    c->phase = TRUING_LASER_NAV_BACKUP;
    result_for(c, TRUING_NAV_IN_MOTION, TRUING_REASON_NONE, out);
}

static void judge_trial(truing_navigation_laser_ctx_t *c, const truing_laser_trial_t *t, truing_nav_result_t *out)
{
    const uint32_t edges = c->port.beam_edges(c->port.user) - c->edges0;
    if (edges > c->cfg.chatter_edges) {
        c->chatter++;
        back_up_or_hand_over(c, "beam chatter during the run", out);
        return;
    }
    if (!t->fired || t->result == TRUING_ALIGN_RESULT_ABORTED || t->result == TRUING_ALIGN_RESULT_FAULT ||
        t->result == TRUING_ALIGN_RESULT_LASER_MISSED || t->result == TRUING_ALIGN_RESULT_LASER_AMBIGUOUS) {
        back_up_or_hand_over(c, "the run did not complete", out);
        return;
    }
    /* name the trigger by the spacing from the last trusted spoke BEFORE anything strikes */
    const bool named = c->anchor_valid;
    if (c->anchor_valid) {
        int32_t who = 0, err = 0;
        if (!truing_laser_nav_identify(&c->cfg, c->anchor_pos, c->anchor_spoke, t->trigger_pos, &who, &err)) {
            hand_over(c, "the trigger fits no spoke of the wheel's spacing", out);
            return;
        }
        c->anchor_pos = t->trigger_pos;
        c->anchor_spoke = who;
        if (who != c->k) {
            c->misnamed++;
            back_up_or_hand_over(c, "it stopped on another spoke (a nudged spoke)", out);
            return;
        }
    } else {
        /* the first trigger after a placement is trusted to be spoke k: nothing yet to name it against */
        c->anchor_valid = true;
        c->anchor_pos = t->trigger_pos;
        c->anchor_spoke = c->k;
    }
    if (t->width_used < c->cfg.width_min || t->width_used > c->cfg.width_max) {
        /* the step scale or the beam is off: a back-up computed in that scale cannot be trusted (the PC script
         * re-located here, which this port does not do), so the operator positions this spoke */
        hand_over(c, "blocked width off its value: the step scale or the beam is off", out);
        return;
    }
    const truing_laser_recipe_t *r = &c->cfg.class_recipe[cls_of(c->k)];
    if ((r->strategy == TRUING_ALIGN_STRAT_REACQUIRE || r->strategy == TRUING_ALIGN_STRAT_REACQUIRE_CLEAR) &&
        t->result != TRUING_ALIGN_RESULT_ALIGNED) {
        back_up_or_hand_over(c, "the reacquiring recipe did not end aligned", out);
        return;
    }
    const int32_t over = (t->stop_pos - t->trigger_pos) * c->cfg.dir;
    if (abs(over - r->stop_steps) > c->cfg.stop_tol_steps) {
        back_up_or_hand_over(c, "the stop distance is off its class value", out);
        return;
    }
    c->belief_valid = true;
    c->belief = c->k + floor_half(t->final_zone);
    c->belief_level_known = true;
    c->belief_blocked = t->final_blocked;
    c->aligned++;
    c->last_why = NULL;
    /* sensor_confirmed only when the spacing actually named the spoke; the first trigger after a placement
     * or a rescale is trusted, not named */
    succeed(c, named, out);
}

/* ---- navigation contract ---------------------------------------------------------------------- */
static void laser_request(truing_navigation_if_t *self, const truing_nav_target_t *target, truing_nav_result_t *out)
{
    truing_navigation_laser_ctx_t *c = (truing_navigation_laser_ctx_t *)self->ctx;
    memset(out, 0, sizeof(*out));
    if (c == NULL || target == NULL) {
        out->outcome = TRUING_NAV_REFUSED;
        out->reason = TRUING_REASON_NOT_IMPLEMENTED;
        return;
    }
    if (c->phase == TRUING_LASER_NAV_TRIAL || c->phase == TRUING_LASER_NAV_BACKUP) {
        out->outcome = TRUING_NAV_REFUSED;   /* one request at a time; the orchestrator never overlaps them */
        out->reason = TRUING_REASON_VALUE_OUT_OF_RANGE;
        out->target = *target;
        return;
    }
    c->target = *target;
    c->tries = 0u;
    if (target->kind != TRUING_NAV_TARGET_SPOKE) {
        hand_over(c, "only spokes are positioned by the laser", out);
        return;
    }
    if (c->at_station == (int16_t)target->index) {
        succeed(c, c->mctx.position.sensor_confirmed, out);   /* already there (a re-request of the same spoke) */
        return;
    }
    if (!c->belief_valid) {
        hand_over(c, "no spoke is known to be at the beam", out);
        return;
    }
    /* the unwrapped spoke: the next one at or after the belief that has this index */
    const int32_t n = (int32_t)c->n_spokes;
    int32_t d = ((int32_t)target->index - c->belief) % n;
    if (d <= 0) {
        d += n;
    }
    c->k = c->belief + d;
    c->at_station = -1;
    start_trial(c, out);
}

static void laser_poll(truing_navigation_if_t *self, truing_nav_result_t *out)
{
    truing_navigation_laser_ctx_t *c = (truing_navigation_laser_ctx_t *)self->ctx;
    if (c == NULL) {
        memset(out, 0, sizeof(*out));
        out->outcome = TRUING_NAV_REFUSED;
        return;
    }
    switch (c->phase) {
    case TRUING_LASER_NAV_TRIAL: {
        truing_laser_trial_t t;
        memset(&t, 0, sizeof(t));
        if (!c->port.trial(c->port.user, &t)) {
            hand_over(c, "the alignment controller is not available", out);
            return;
        }
        if (t.active || c->port.moving(c->port.user)) {
            result_for(c, TRUING_NAV_IN_MOTION, TRUING_REASON_NONE, out);
            return;
        }
        judge_trial(c, &t, out);
        return;
    }
    case TRUING_LASER_NAV_BACKUP:
        if (c->port.moving(c->port.user)) {
            result_for(c, TRUING_NAV_IN_MOTION, TRUING_REASON_NONE, out);
            return;
        }
        if (c->port.beam_blocked(c->port.user)) {
            hand_over(c, "the beam is blocked where the gap before the spoke should be", out);
            return;
        }
        c->belief_valid = true;
        c->belief = c->k - 1;
        c->belief_level_known = false;   /* declared, as `align spoke k-1` */
        start_trial(c, out);
        return;
    case TRUING_LASER_NAV_MANUAL:
        c->manual.poll(&c->manual, out);
        return;
    case TRUING_LASER_NAV_DONE:
        result_for(c, TRUING_NAV_DONE, TRUING_REASON_NONE, out);
        return;
    default:
        result_for(c, TRUING_NAV_IDLE, TRUING_REASON_NONE, out);
        return;
    }
}

static void laser_confirm(truing_navigation_if_t *self, truing_nav_result_t *out)
{
    truing_navigation_laser_ctx_t *c = (truing_navigation_laser_ctx_t *)self->ctx;
    if (c == NULL) {
        memset(out, 0, sizeof(*out));
        out->outcome = TRUING_NAV_REFUSED;
        return;
    }
    c->manual.note_operator_confirmation(&c->manual, out);
    if (out->outcome != TRUING_NAV_DONE) {
        return;
    }
    /* The operator placed this spoke at the strike point. The next run starts from it; its first trigger
     * re-anchors the spacing (nothing to name it against until then). Spoke 0 for the reference is placed
     * just BEFORE the beam, so it is the spoke the first run looks for: the belief is the one behind it. */
    const bool reference = out->target.kind == TRUING_NAV_TARGET_SPOKE && c->phase != TRUING_LASER_NAV_MANUAL &&
                           out->target.index == 0u && !c->belief_valid;
    forget_position(c);
    c->belief_valid = true;
    /* The level each placement is supposed to leave, so the next run's reconcile checks it: spoke 0 for the
     * reference goes just BEFORE the beam (clear); a handed-over spoke goes IN it (blocked). A reference
     * spoke found in the beam reconciles to "spoke 0 already there", which is not where its recipe stops it,
     * and with no anchor to back up from it goes back to the operator rather than being struck as spoke 1. */
    c->belief_level_known = true;
    if (reference) {
        c->belief = -1;
        c->belief_blocked = false;
        c->at_station = -1;
    } else {
        c->belief = (int32_t)out->target.index;
        c->belief_blocked = true;
        c->at_station = (int16_t)out->target.index;
    }
    c->phase = reference ? TRUING_LASER_NAV_IDLE : TRUING_LASER_NAV_DONE;
    c->tries = 0u;
}

static void laser_establish_reference(truing_navigation_if_t *self, truing_nav_result_t *out)
{
    truing_navigation_laser_ctx_t *c = (truing_navigation_laser_ctx_t *)self->ctx;
    if (c == NULL) {
        memset(out, 0, sizeof(*out));
        out->outcome = TRUING_NAV_REFUSED;
        return;
    }
    forget_position(c);
    c->phase = TRUING_LASER_NAV_IDLE;
    c->manual.establish_reference(&c->manual, out);
}

static void laser_query(truing_navigation_if_t *self, truing_wheel_position_t *out)
{
    truing_navigation_laser_ctx_t *c = (truing_navigation_laser_ctx_t *)self->ctx;
    if (c == NULL) {
        memset(out, 0, sizeof(*out));
        out->status = TRUING_STATUS_UNAVAILABLE;
        out->reason = TRUING_REASON_NOT_IMPLEMENTED;
        return;
    }
    c->manual.query(&c->manual, out);
}

static void laser_stop(truing_navigation_if_t *self)
{
    truing_navigation_laser_ctx_t *c = (truing_navigation_laser_ctx_t *)self->ctx;
    if (c == NULL) {
        return;
    }
    if (c->phase == TRUING_LASER_NAV_TRIAL || c->phase == TRUING_LASER_NAV_BACKUP) {
        forget_position(c);   /* a stop mid-move leaves the wheel somewhere nobody vouched for */
    }
    c->port.stop(c->port.user);
    c->manual.stop(&c->manual);
    c->phase = TRUING_LASER_NAV_IDLE;
}

void truing_navigation_laser_init(truing_navigation_if_t *self, truing_navigation_laser_ctx_t *ctx,
                                  truing_clock_if_t clock, uint8_t n_spokes, const truing_machine_profile_t *machine,
                                  const truing_laser_nav_config_t *cfg, const truing_laser_port_t *port,
                                  truing_source_impl_t source_impl)
{
    if (self == NULL || ctx == NULL || cfg == NULL || port == NULL) {
        return;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->cfg = *cfg;
    ctx->port = *port;
    ctx->n_spokes = n_spokes;
    ctx->at_station = -1;
    truing_navigation_manual_init(&ctx->manual, &ctx->mctx, clock, n_spokes, n_spokes, machine);
    self->impl_name = "navigation_laser";
    self->source_impl = source_impl;
    self->request = laser_request;
    self->poll = laser_poll;
    self->note_operator_confirmation = laser_confirm;
    self->establish_reference = laser_establish_reference;
    self->query = laser_query;
    self->stop = laser_stop;
    self->ctx = ctx;
}
