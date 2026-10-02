/* Bench laser-stop trigger and trial record (see beam_stop.h). on_edge() runs in an interrupt on the
 * target: integer only, no calls out, no allocation. */
#include "truing_hal/beam_stop.h"

#include <string.h>

void truing_beam_stop_init(truing_beam_stop_t *b, bool blocked_now)
{
    if (b == NULL) {
        return;
    }
    memset(b, 0, sizeof(*b));
    b->blocked = blocked_now;
}

bool truing_beam_stop_arm(truing_beam_stop_t *b, int32_t pos)
{
    if (b == NULL || (b->cur.fired && !b->cur.ended)) {
        return false;   /* a triggered trial is still decelerating: let it finish and be recorded */
    }
    memset(&b->cur, 0, sizeof(b->cur));
    b->cur.trial = ++b->trials;
    b->cur.blocked_at_arm = b->blocked;
    b->cur.final_pos = pos;
    b->cur.trigger_pos = pos;
    b->armed = true;
    return true;
}

void truing_beam_stop_disarm(truing_beam_stop_t *b)
{
    if (b != NULL) {
        b->armed = false;   /* an unfired trial is dropped; a fired one is no longer armed anyway */
    }
}

static void log_edge(truing_beam_stop_t *b, truing_spoke_edge_t edge, uint32_t t_us, int32_t pos, bool moving)
{
    b->log_seq++;
    b->log[(b->log_seq - 1u) % TRUING_BEAM_LOG_LEN] =
        (truing_beam_edge_rec_t){ .seq = b->log_seq, .edge = edge, .t_us = t_us, .pos = pos, .moving = moving };
}

bool truing_beam_stop_on_edge(truing_beam_stop_t *b, bool blocked_now, uint32_t t_us, int32_t pos, bool moving)
{
    if (b == NULL) {
        return false;
    }
    if (blocked_now == b->blocked) {
        b->glitches++;   /* the level went and came back before the interrupt read it */
        return false;
    }
    b->blocked = blocked_now;
    const truing_spoke_edge_t edge = blocked_now ? TRUING_SPOKE_EDGE_ENTER : TRUING_SPOKE_EDGE_LEAVE;
    b->edges++;
    if (blocked_now) {
        b->enters++;
    } else {
        b->leaves++;
    }
    b->last_edge_t_us = t_us;
    log_edge(b, edge, t_us, pos, moving);
    if (moving) {
        b->cur.saw_motion = b->cur.saw_motion || b->armed;
    }

    truing_beam_trial_t *c = &b->cur;
    if (c->fired && !c->ended) {
        c->edges_after++;
        if (!blocked_now && !c->left_beam) {
            c->left_beam = true;
            c->leave_pos = pos;
        }
        return false;
    }
    if (b->armed && blocked_now && moving) {
        /* One shot: disarmed the moment it fires, so the next break needs a fresh arm. */
        b->armed = false;
        c->fired = true;
        c->trigger_t_us = t_us;
        c->trigger_pos = pos;
        c->final_pos = pos;
        b->stops_requested++;
        return true;
    }
    return false;
}

bool truing_beam_stop_resync(truing_beam_stop_t *b, bool blocked_now, uint32_t t_us, int32_t pos, bool moving)
{
    if (b == NULL || blocked_now == b->blocked) {
        return false;
    }
    b->resyncs++;
    return truing_beam_stop_on_edge(b, blocked_now, t_us, pos, moving);
}

static void finish(truing_beam_stop_t *b, truing_beam_result_t result, int32_t pos, uint32_t t_us, bool coils_on,
                   bool faulted)
{
    truing_beam_trial_t *c = &b->cur;
    c->ended = true;
    c->end_t_us = t_us;
    c->final_pos = pos;
    c->blocked_at_end = b->blocked;
    c->coils_at_end = coils_on;
    c->fault_at_end = faulted;
    c->result = result;
    b->armed = false;
    b->last = *c;
}

bool truing_beam_stop_poll(truing_beam_stop_t *b, bool moving, int32_t pos, uint32_t t_us, bool coils_on,
                           bool faulted, uint32_t max_step_hz, uint32_t accel_steps_s2)
{
    if (b == NULL) {
        return false;
    }
    truing_beam_trial_t *c = &b->cur;
    const bool open = b->armed || (c->fired && !c->ended);
    if (!open) {
        return false;
    }
    if (moving && !c->saw_motion) {
        c->saw_motion = true;
    }
    /* The ramp cannot change while the drive moves (set_motion is refused then), so the first look at a
     * moving drive -- or at a triggered one -- records the ramp this move ran at. */
    if ((moving || c->fired) && c->max_step_hz == 0u) {
        c->max_step_hz = max_step_hz;
        c->accel_steps_s2 = accel_steps_s2;
    }
    if (moving) {
        return false;
    }
    if (c->fired) {
        truing_beam_result_t r = TRUING_BEAM_RESULT_PASSED_THROUGH;
        if (!coils_on || faulted) {
            r = TRUING_BEAM_RESULT_INTERRUPTED;   /* the pulse count ended, but not by the controlled stop */
        } else if (b->blocked) {
            /* Blocked at the end is only "the spoke that broke it" if the beam never cleared in between. */
            r = c->left_beam ? TRUING_BEAM_RESULT_BLOCKED_AGAIN : TRUING_BEAM_RESULT_STOPPED_IN_BEAM;
        }
        finish(b, r, pos, t_us, coils_on, faulted);
        return true;
    }
    if (c->saw_motion) {
        /* Armed through a whole move that never broke the beam (or was stopped by someone else first). */
        finish(b, TRUING_BEAM_RESULT_NO_TRIGGER, pos, t_us, coils_on, faulted);
        return true;
    }
    return false;
}

size_t truing_beam_stop_edges_since(const truing_beam_stop_t *b, uint32_t since_seq, truing_beam_edge_rec_t *out,
                                    size_t max, uint32_t *lost)
{
    if (lost != NULL) {
        *lost = 0u;
    }
    if (b == NULL || out == NULL || since_seq >= b->log_seq) {
        return 0u;
    }
    uint32_t first = since_seq + 1u;
    const uint32_t oldest = b->log_seq > TRUING_BEAM_LOG_LEN ? b->log_seq - TRUING_BEAM_LOG_LEN + 1u : 1u;
    if (first < oldest) {
        if (lost != NULL) {
            *lost = oldest - first;
        }
        first = oldest;
    }
    size_t n = 0u;
    for (uint32_t s = first; s <= b->log_seq && n < max; ++s) {
        out[n++] = b->log[(s - 1u) % TRUING_BEAM_LOG_LEN];
    }
    return n;
}

const char *truing_beam_result_str(truing_beam_result_t r)
{
    switch (r) {
    case TRUING_BEAM_RESULT_STOPPED_IN_BEAM: return "STOPPED_IN_BEAM";
    case TRUING_BEAM_RESULT_PASSED_THROUGH:  return "PASSED_THROUGH";
    case TRUING_BEAM_RESULT_BLOCKED_AGAIN:   return "BLOCKED_AGAIN";
    case TRUING_BEAM_RESULT_NO_TRIGGER:      return "NO_TRIGGER";
    case TRUING_BEAM_RESULT_INTERRUPTED:     return "INTERRUPTED";
    default:                                 return "NONE";
    }
}
