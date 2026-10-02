/* Laser-guided spoke alignment controller (see beam_align.h). on_edge() runs in the beam interrupt on the
 * target: integer only, no calls out. Everything that commands the drive goes through truing_align_cmd_t. */
#include "truing_hal/beam_align.h"

#include <stdlib.h>
#include <string.h>

/* A handed-out move that shows neither motion nor a position change for this long was not executed. */
#define CMD_GRACE_US 1000000u

static int32_t abs32(int32_t v)
{
    return v < 0 ? -v : v;
}

static bool zone_even(int8_t z)
{
    return (z & 1) == 0;
}

static void event(truing_beam_align_t *a, truing_align_phase_t p, uint32_t t_us, int32_t pos)
{
    a->phase = p;
    a->phase_t_us = t_us;
    if (a->n_events < TRUING_ALIGN_EVENT_LOG) {
        a->events[a->n_events++] = (truing_align_event_t){ .phase = (uint8_t)p, .t_us = t_us, .pos = pos,
                                                           .blocked = a->blocked };
    }
}

void truing_beam_align_init(truing_beam_align_t *a, bool blocked_now)
{
    if (a == NULL) {
        return;
    }
    memset(a, 0, sizeof(*a));
    a->blocked = blocked_now;
}

bool truing_beam_align_active(const truing_beam_align_t *a)
{
    return a != NULL && a->phase != TRUING_ALIGN_PHASE_IDLE && a->phase != TRUING_ALIGN_PHASE_DONE;
}

static void issue_move(truing_beam_align_t *a, truing_align_cmd_t *cmd, int32_t steps, int32_t pos, uint32_t t_us)
{
    cmd->move = true;
    cmd->steps = steps;
    a->move_sign = (int8_t)(((steps > 0) == (a->cfg.dir > 0)) ? 1 : -1);
    a->cmd_outstanding = true;
    a->cmd_pos = pos;
    a->cmd_t_us = t_us;
}

static void use_slow_ramp(truing_beam_align_t *a, truing_align_cmd_t *cmd)
{
    if (!a->profile_switched) {
        cmd->set_motion = true;
        cmd->hz = a->cfg.slow_hz;
        cmd->accel = a->cfg.slow_accel;
        a->profile_switched = true;
    }
}

static void finish(truing_beam_align_t *a, truing_align_result_t r, truing_align_detail_t d, uint32_t t_us,
                   int32_t pos)
{
    a->result = r;
    a->detail = d;
    a->final_pos = pos;
    a->final_blocked = a->blocked;
    a->final_zone = a->zone;
    a->end_t_us = t_us;
    a->cmd_outstanding = false;
    a->restore_pending = a->profile_switched;
    event(a, TRUING_ALIGN_PHASE_DONE, t_us, pos);
}

bool truing_beam_align_start(truing_beam_align_t *a, const truing_align_config_t *cfg, int32_t pos, bool moving,
                             bool blocked_now, uint32_t t_us, uint32_t hz, uint32_t accel, truing_align_cmd_t *cmd,
                             const char **why)
{
    const char *w = NULL;
    if (a == NULL || cfg == NULL || cmd == NULL) {
        w = "internal: null argument";
    } else if (truing_beam_align_active(a)) {
        w = "a run is already in progress";
    } else if (moving) {
        w = "the roller is moving: stop it first";
    } else if (cfg->dir != 1 && cfg->dir != -1) {
        w = "dir must be +1 or -1";
    } else if (cfg->mode != TRUING_ALIGN_MODE_COUNT && cfg->mode != TRUING_ALIGN_MODE_ALIGN) {
        w = "mode must be count or align";
    } else if (cfg->target_break == 0u || cfg->target_break > 1000u) {
        w = "target break is 1..1000";
    } else if (cfg->max_steps == 0u || cfg->max_steps > 200000u) {
        w = "max_steps is 1..200000";
    } else if (cfg->settle_ms > 10000u) {
        w = "settle is at most 10000 ms";
    } else if (cfg->mode == TRUING_ALIGN_MODE_ALIGN) {
        if ((unsigned)cfg->strategy >= (unsigned)TRUING_ALIGN_STRAT__COUNT) {
            w = "unknown strategy";
        } else if (cfg->strategy != TRUING_ALIGN_STRAT_STOP && (cfg->slow_hz == 0u || cfg->slow_accel == 0u)) {
            w = "a correction strategy needs slow_hz and slow_accel";
        } else if (cfg->strategy == TRUING_ALIGN_STRAT_REVERSE_FIXED && (cfg->reverse_steps == 0u ||
                                                                         cfg->reverse_steps > 200000u)) {
            w = "REVERSE_FIXED needs reverse_steps 1..200000";
        } else if ((cfg->strategy == TRUING_ALIGN_STRAT_REACQUIRE ||
                    cfg->strategy == TRUING_ALIGN_STRAT_REACQUIRE_CLEAR) &&
                   (cfg->reacq_max_steps == 0u || cfg->reacq_max_steps > 200000u)) {
            w = "REACQUIRE / REACQUIRE_CLEAR needs reacq_max_steps 1..200000";
        }
    }
    if (w != NULL) {
        if (why != NULL) {
            *why = w;
        }
        return false;
    }
    const uint32_t run = a->run + 1u;
    memset(a, 0, sizeof(*a));
    a->run = run;
    a->cfg = *cfg;
    a->blocked = blocked_now;
    a->start_pos = pos;
    a->start_blocked = blocked_now;
    a->start_t_us = t_us;
    a->approach_hz = hz;
    a->approach_accel = accel;
    a->pitch_med = -1;
    a->pitch_min = -1;
    a->width_med = -1;
    a->width_used = -1;
    memset(cmd, 0, sizeof(*cmd));
    event(a, TRUING_ALIGN_PHASE_APPROACH, t_us, pos);
    issue_move(a, cmd, (int32_t)cfg->dir * (int32_t)cfg->max_steps, pos, t_us);
    if (why != NULL) {
        *why = "started";
    }
    return true;
}

static void log_edge(truing_beam_align_t *a, bool enter, uint32_t t_us, int32_t pos, int8_t sign)
{
    if (a->n_edges < TRUING_ALIGN_EDGE_LOG) {
        a->edges[a->n_edges++] = (truing_align_edge_t){ .t_us = t_us, .pos = pos, .sign = sign, .enter = enter,
                                                        .phase = (uint8_t)a->phase, .zone = a->zone };
    } else {
        a->edges_dropped++;
    }
}

bool truing_beam_align_on_edge(truing_beam_align_t *a, bool blocked_now, uint32_t t_us, int32_t pos, bool moving)
{
    if (a == NULL || blocked_now == a->blocked) {
        return false;
    }
    a->blocked = blocked_now;
    if (!truing_beam_align_active(a)) {
        return false;
    }
    if (!moving) {
        a->edges_still++;   /* no known direction: settle spring-back, a pluck, a hand */
        log_edge(a, blocked_now, t_us, pos, 0);
        return false;
    }
    bool stop = false;
    if (a->phase == TRUING_ALIGN_PHASE_APPROACH) {
        if (blocked_now) {
            a->breaks++;
            if (a->breaks == a->cfg.target_break) {
                a->fired = true;
                a->trigger_t_us = t_us;
                a->trigger_pos = pos;
                a->zone_valid = true;
                a->zone = 0;
                stop = true;
            }
        }
        log_edge(a, blocked_now, t_us, pos, a->move_sign);
        if (stop) {
            event(a, TRUING_ALIGN_PHASE_STOPPING, t_us, pos);
        }
        return stop;
    }
    if (a->zone_valid) {
        a->zone = (int8_t)(a->zone + a->move_sign);
        if (zone_even(a->zone) != blocked_now) {
            a->parity_bad = true;
        }
    }
    log_edge(a, blocked_now, t_us, pos, a->move_sign);
    switch (a->phase) {
    case TRUING_ALIGN_PHASE_STOPPING:
        a->edges_in_stop++;
        if (!blocked_now && !a->target_left && a->zone == 1) {
            a->target_left = true;
            a->target_leave_pos = pos;
        }
        break;
    case TRUING_ALIGN_PHASE_CORRECT:
        a->corr_edges++;
        if (a->cfg.strategy == TRUING_ALIGN_STRAT_REACQUIRE && !a->reacquired && blocked_now && a->zone == 0) {
            a->reacquired = true;
            a->reacquisitions++;
            a->reacq_pos = pos;
            a->reacq_t_us = t_us;
            stop = true;   /* the same spoke is back: the drive's controlled stop, from here */
        } else if (a->cfg.strategy == TRUING_ALIGN_STRAT_REACQUIRE_CLEAR) {
            if (!a->reacquired && blocked_now && a->zone == 0) {
                a->reacquired = true;   /* the spoke is back in the beam: do NOT stop, keep reversing through it */
                a->reacquisitions++;
                a->reacq_pos = pos;
                a->reacq_t_us = t_us;
            } else if (a->reacquired && !a->cleared && !blocked_now && a->zone == -1) {
                a->cleared = true;      /* the beam is clear again on the spoke's far side */
                a->clear_pos = pos;
                stop = true;
            }
        }
        break;
    default:
        break;
    }
    return stop;
}

/* Medians of this run's approach: break-to-break spacing and blocked width, in steps (-1 = not enough). */
static int cmp_i32(const void *x, const void *y)
{
    const int32_t a = *(const int32_t *)x, b = *(const int32_t *)y;
    return (a > b) - (a < b);
}

static int32_t median(int32_t *v, size_t n)
{
    if (n == 0u) {
        return -1;
    }
    qsort(v, n, sizeof(v[0]), cmp_i32);
    return v[n / 2u];
}

bool truing_beam_align_same_spoke(int32_t offset, int32_t min_pitch)
{
    return min_pitch <= 0 || abs32(offset) * 2 <= min_pitch;
}

static void approach_stats(truing_beam_align_t *a)
{
    static int32_t pitches[TRUING_ALIGN_EDGE_LOG], widths[TRUING_ALIGN_EDGE_LOG];   /* task context only */
    size_t np = 0u, nw = 0u;
    bool have_enter = false;
    int32_t last_enter = 0;
    bool open = false;
    for (uint16_t i = 0; i < a->n_edges; ++i) {
        const truing_align_edge_t *e = &a->edges[i];
        if (e->phase != TRUING_ALIGN_PHASE_APPROACH || e->sign != 1) {
            continue;
        }
        if (e->enter) {
            if (have_enter) {
                pitches[np++] = abs32(e->pos - last_enter);
            }
            have_enter = true;
            last_enter = e->pos;
            open = true;
        } else if (open) {
            widths[nw++] = abs32(e->pos - last_enter);
            open = false;
        }
    }
    a->pitch_med = median(pitches, np);
    a->pitch_min = -1;
    for (size_t i = 0u; i < np; ++i) {
        if (a->pitch_min < 0 || pitches[i] < a->pitch_min) {
            a->pitch_min = pitches[i];
        }
    }
    a->width_med = median(widths, nw);
    a->width_used = a->target_left ? abs32(a->target_leave_pos - a->trigger_pos) : a->width_med;
}

static int32_t centering(const truing_beam_align_t *a)
{
    return (a->cfg.center && a->width_used > 0) ? a->width_used / 2 : 0;
}

static truing_align_result_t evaluate(truing_beam_align_t *a, truing_align_detail_t *d)
{
    *d = TRUING_ALIGN_DETAIL_NONE;
    if (a->cfg.mode == TRUING_ALIGN_MODE_COUNT) {
        return TRUING_ALIGN_RESULT_COUNT_DONE;
    }
    if (!a->zone_valid || a->parity_bad || zone_even(a->zone) != a->blocked) {
        *d = TRUING_ALIGN_DETAIL_PARITY;
        return TRUING_ALIGN_RESULT_LASER_AMBIGUOUS;
    }
    if (a->same_spoke_checked && !a->same_spoke_ok) {
        *d = TRUING_ALIGN_DETAIL_SAME_SPOKE;
        return TRUING_ALIGN_RESULT_LASER_AMBIGUOUS;
    }
    /* REACQUIRE_CLEAR's target state is "just cleared the spoke": zone -1, beam clear. Every other strategy aims
     * at the spoke itself: zone 0, beam blocked. */
    const int8_t want = a->cfg.strategy == TRUING_ALIGN_STRAT_REACQUIRE_CLEAR ? -1 : 0;
    if (a->zone == want) {
        return TRUING_ALIGN_RESULT_ALIGNED;
    }
    if (a->cfg.strategy == TRUING_ALIGN_STRAT_REVERSE_FIXED || a->cfg.strategy == TRUING_ALIGN_STRAT_RETURN_TO_EDGE) {
        return TRUING_ALIGN_RESULT_REVERSE_FAILED;
    }
    return a->zone > want ? TRUING_ALIGN_RESULT_OVERSHOOT : TRUING_ALIGN_RESULT_UNDERSHOOT;
}

static void finish_evaluated(truing_beam_align_t *a, uint32_t t_us, int32_t pos)
{
    truing_align_detail_t d;
    const truing_align_result_t r = evaluate(a, &d);
    finish(a, r, d, t_us, pos);
}

/* Settled after the first stop: choose the correction. */
static void after_first_stop(truing_beam_align_t *a, uint32_t t_us, int32_t pos, truing_align_cmd_t *cmd)
{
    a->blocked_after_stop = a->blocked;
    a->zone_after_stop = a->zone;
    approach_stats(a);
    if (a->cfg.mode == TRUING_ALIGN_MODE_COUNT || a->cfg.strategy == TRUING_ALIGN_STRAT_STOP) {
        finish_evaluated(a, t_us, pos);
        return;
    }
    const int32_t dir = a->cfg.dir;
    int32_t steps = 0;
    switch (a->cfg.strategy) {
    case TRUING_ALIGN_STRAT_REVERSE_FIXED:
        steps = -dir * (int32_t)a->cfg.reverse_steps;
        break;
    case TRUING_ALIGN_STRAT_RETURN_TO_EDGE:
        steps = (a->trigger_pos + dir * (centering(a) + a->cfg.offset_steps)) - pos;
        break;
    case TRUING_ALIGN_STRAT_REACQUIRE_CLEAR:
        if (a->zone == 0 && a->blocked) {
            /* The stop ended inside the spoke: it is already "re-found". Reverse through it to the far side. */
            a->reacquired = true;
            a->reacq_pos = pos;
            a->reacq_t_us = t_us;
        }
        steps = -dir * (int32_t)a->cfg.reacq_max_steps;
        break;
    case TRUING_ALIGN_STRAT_REACQUIRE:
        if (a->zone == 0 && a->blocked) {
            /* Still in the beam: nothing to re-find. Center / offset from the trigger edge if asked. */
            steps = (a->cfg.center || a->cfg.offset_steps != 0)
                        ? (a->trigger_pos + dir * (centering(a) + a->cfg.offset_steps)) - pos
                        : 0;
            a->corr_cmd = steps;
            a->corr_start_pos = pos;
            if (steps != 0) {
                use_slow_ramp(a, cmd);
                issue_move(a, cmd, steps, pos, t_us);
                event(a, TRUING_ALIGN_PHASE_TRIM, t_us, pos);
                a->trim_cmd = steps;
            } else {
                event(a, TRUING_ALIGN_PHASE_FINAL_SETTLE, t_us, pos);
            }
            return;
        }
        steps = -dir * (int32_t)a->cfg.reacq_max_steps;
        break;
    default:
        break;
    }
    a->corr_cmd = steps;
    a->corr_start_pos = pos;
    if (steps == 0) {
        event(a, TRUING_ALIGN_PHASE_FINAL_SETTLE, t_us, pos);
        return;
    }
    use_slow_ramp(a, cmd);
    issue_move(a, cmd, steps, pos, t_us);
    event(a, TRUING_ALIGN_PHASE_CORRECT, t_us, pos);
}

/* The correction move is over. */
static void after_correction(truing_beam_align_t *a, uint32_t t_us, int32_t pos, truing_align_cmd_t *cmd)
{
    const bool clear_strategy = a->cfg.strategy == TRUING_ALIGN_STRAT_REACQUIRE_CLEAR;
    if (a->cfg.strategy != TRUING_ALIGN_STRAT_REACQUIRE && !clear_strategy) {
        event(a, TRUING_ALIGN_PHASE_FINAL_SETTLE, t_us, pos);
        return;
    }
    if (!a->reacquired || (clear_strategy && !a->cleared)) {
        finish(a, TRUING_ALIGN_RESULT_REACQUISITION_FAILED, TRUING_ALIGN_DETAIL_NONE, t_us, pos);
        return;
    }
    /* Same spoke? Coming back against the approach, the target re-enters the beam at the edge where it left.
     * Any other spoke's edge is at least the SHORTEST measured gap away (the spacing repeats 1963/718/1963/3255 on
     * the bench wheel, so a median over-trusts a neighbour 700 steps off), so more than half of that is not this spoke. */
    if (a->target_left && a->pitch_min > 0) {
        a->same_spoke_checked = true;
        a->same_spoke_ok = truing_beam_align_same_spoke(a->reacq_pos - a->target_leave_pos, a->pitch_min);
    }
    /* REACQUIRE_CLEAR: after the clear stop, `offset_steps` more along dir (negative = further back, away from the
     * spoke), relative to where the stop ended. The others aim at the re-found edge. */
    const int32_t steps = clear_strategy ? a->cfg.dir * a->cfg.offset_steps
                          : (a->cfg.center || a->cfg.offset_steps != 0)
                              ? (a->reacq_pos + a->cfg.dir * (a->cfg.offset_steps - centering(a))) - pos
                              : 0;
    a->trim_cmd = steps;
    if (steps != 0) {
        issue_move(a, cmd, steps, pos, t_us);
        event(a, TRUING_ALIGN_PHASE_TRIM, t_us, pos);
    } else {
        event(a, TRUING_ALIGN_PHASE_FINAL_SETTLE, t_us, pos);
    }
}

bool truing_beam_align_tick(truing_beam_align_t *a, uint32_t t_us, int32_t pos, bool moving, bool coils_on,
                            bool faulted, truing_align_cmd_t *cmd)
{
    if (a == NULL || cmd == NULL) {
        return false;
    }
    memset(cmd, 0, sizeof(*cmd));
    bool finished = false;
    if (truing_beam_align_active(a)) {
        if (a->cmd_outstanding && (moving || pos != a->cmd_pos)) {
            a->cmd_outstanding = false;   /* the handed-out move is running (or already ran) */
        }
        if (!coils_on || faulted) {
            finish(a, TRUING_ALIGN_RESULT_FAULT,
                   faulted ? TRUING_ALIGN_DETAIL_DRIVER_FAULT : TRUING_ALIGN_DETAIL_COILS_OFF, t_us, pos);
            finished = true;
        } else if (a->cmd_outstanding) {
            if (t_us - a->cmd_t_us > CMD_GRACE_US) {
                finish(a, TRUING_ALIGN_RESULT_FAULT, TRUING_ALIGN_DETAIL_DRIVE_REFUSED, t_us, pos);
                finished = true;
            }
        } else {
            switch (a->phase) {
            case TRUING_ALIGN_PHASE_APPROACH:
                if (!moving) {
                    const bool ran_out = (uint32_t)abs32(pos - a->start_pos) >= a->cfg.max_steps;
                    finish(a, ran_out ? TRUING_ALIGN_RESULT_LASER_MISSED : TRUING_ALIGN_RESULT_ABORTED,
                           ran_out ? TRUING_ALIGN_DETAIL_NONE : TRUING_ALIGN_DETAIL_STOPPED_EARLY, t_us, pos);
                    finished = true;
                }
                break;
            case TRUING_ALIGN_PHASE_STOPPING:
                if (!moving) {
                    a->stop_pos = pos;
                    a->stop_t_us = t_us;
                    event(a, TRUING_ALIGN_PHASE_SETTLE, t_us, pos);
                }
                break;
            case TRUING_ALIGN_PHASE_SETTLE:
            case TRUING_ALIGN_PHASE_FINAL_SETTLE:
                if (moving) {
                    finish(a, TRUING_ALIGN_RESULT_ABORTED, TRUING_ALIGN_DETAIL_UNEXPECTED_MOTION, t_us, pos);
                    finished = true;
                } else if (t_us - a->phase_t_us >= a->cfg.settle_ms * 1000u) {
                    if (a->phase == TRUING_ALIGN_PHASE_SETTLE) {
                        after_first_stop(a, t_us, pos, cmd);
                    } else {
                        finish_evaluated(a, t_us, pos);
                    }
                    finished = a->phase == TRUING_ALIGN_PHASE_DONE;
                }
                break;
            case TRUING_ALIGN_PHASE_CORRECT:
                if (!moving) {
                    after_correction(a, t_us, pos, cmd);
                    finished = a->phase == TRUING_ALIGN_PHASE_DONE;
                }
                break;
            case TRUING_ALIGN_PHASE_TRIM:
                if (!moving) {
                    event(a, TRUING_ALIGN_PHASE_FINAL_SETTLE, t_us, pos);
                }
                break;
            default:
                break;
            }
        }
    }
    /* Put the operator's ramp back once nothing moves (a run may end mid-motion, e.g. an abort). */
    if (!truing_beam_align_active(a) && a->restore_pending && !moving && !cmd->move) {
        cmd->set_motion = true;
        cmd->hz = a->approach_hz;
        cmd->accel = a->approach_accel;
        a->restore_pending = false;
        a->profile_switched = false;
    }
    return finished;
}

void truing_beam_align_command_failed(truing_beam_align_t *a, uint32_t t_us, int32_t pos)
{
    if (truing_beam_align_active(a)) {
        finish(a, TRUING_ALIGN_RESULT_FAULT, TRUING_ALIGN_DETAIL_DRIVE_REFUSED, t_us, pos);
    }
}

bool truing_beam_align_abort(truing_beam_align_t *a, uint32_t t_us, int32_t pos)
{
    if (!truing_beam_align_active(a)) {
        return false;
    }
    finish(a, TRUING_ALIGN_RESULT_ABORTED, TRUING_ALIGN_DETAIL_OPERATOR_ABORT, t_us, pos);
    return true;
}

const char *truing_align_phase_str(truing_align_phase_t p)
{
    static const char *const k[] = { "IDLE", "APPROACH", "STOPPING", "SETTLE", "CORRECT", "TRIM", "FINAL_SETTLE",
                                     "DONE" };
    return (unsigned)p < sizeof(k) / sizeof(k[0]) ? k[p] : "?";
}

const char *truing_align_result_str(truing_align_result_t r)
{
    static const char *const k[] = { "NONE", "COUNT_DONE", "ALIGNED", "OVERSHOOT", "UNDERSHOOT", "LASER_MISSED",
                                     "LASER_AMBIGUOUS", "REVERSE_FAILED", "REACQUISITION_FAILED", "ABORTED",
                                     "FAULT" };
    return (unsigned)r < sizeof(k) / sizeof(k[0]) ? k[r] : "?";
}

const char *truing_align_detail_str(truing_align_detail_t d)
{
    static const char *const k[] = { "", "OPERATOR_ABORT", "STOPPED_EARLY", "UNEXPECTED_MOTION", "DRIVE_REFUSED",
                                     "COILS_OFF", "DRIVER_FAULT", "PARITY", "SAME_SPOKE" };
    return (unsigned)d < sizeof(k) / sizeof(k[0]) ? k[d] : "?";
}

static const char *const k_strategy[TRUING_ALIGN_STRAT__COUNT] = { "STOP", "REVERSE_FIXED", "RETURN_TO_EDGE",
                                                                   "REACQUIRE", "REACQUIRE_CLEAR" };

const char *truing_align_strategy_str(truing_align_strategy_t s)
{
    return (unsigned)s < (unsigned)TRUING_ALIGN_STRAT__COUNT ? k_strategy[s] : "?";
}

bool truing_align_strategy_parse(const char *s, truing_align_strategy_t *out)
{
    for (unsigned i = 0; s != NULL && i < (unsigned)TRUING_ALIGN_STRAT__COUNT; ++i) {
        if (strcmp(s, k_strategy[i]) == 0) {
            if (out != NULL) {
                *out = (truing_align_strategy_t)i;
            }
            return true;
        }
    }
    return false;
}
