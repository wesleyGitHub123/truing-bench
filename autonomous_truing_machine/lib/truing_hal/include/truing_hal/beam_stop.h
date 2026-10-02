/**
 * @file beam_stop.h
 * BENCH laser-stop: a one-shot beam-break trigger that asks for the wheel drive's own controlled stop,
 * and the record of what followed. A characterization tool for the lab image (TRUING_BENCH_DRIVE), NOT
 * navigation: it knows nothing about spokes, stations or wheel angle, and nothing here becomes wheel
 * state. Each trial answers one question -- "the receiver saw the beam break at step T; the last step
 * pulse went out at step F; was the beam still blocked then?" -- so the speed and deceleration that stop
 * a spoke inside the beam can be found from data before any indexing algorithm is designed
 * (docs/LASER_BENCH.md).
 *
 * Pure and framework-free so the decision logic is host-testable. The target glue (src/laser_bench.c)
 * feeds it from a GPIO edge interrupt and a short poll, under one spinlock, and is the one that calls the
 * drive's stop(): truing_beam_stop_on_edge() only says whether to. stop() is the existing controlled
 * deceleration (wheel_drive_if.h, step_ramp.h "any context"); this module never starts, extends or
 * re-times motion, so a manual stop, a coil release and a driver fault keep the authority they had.
 *
 * Vocabulary is the spoke sensor seam's (spoke_sensor_if.h): ENTER = the beam became blocked, LEAVE = it
 * is clear again. That seam stays unimplemented (TRUING_SPOKE_SENSOR is still reserved): this is the bench
 * step that gathers the trigger/overshoot data a real implementation would be designed from.
 *
 * Every step count here is the drive's pulse counter -- commanded motion, never evidence the wheel
 * followed. The beam level at the end of the pulse train is the one physical observation a trial makes.
 */
#ifndef TRUING_HAL_BEAM_STOP_H
#define TRUING_HAL_BEAM_STOP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "truing_hal/spoke_sensor_if.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TRUING_BEAM_RESULT_NONE = 0,          /* no trial has finished */
    TRUING_BEAM_RESULT_STOPPED_IN_BEAM,   /* triggered; the beam stayed blocked from the break to the last pulse */
    TRUING_BEAM_RESULT_PASSED_THROUGH,    /* triggered; the beam was clear again by the last pulse */
    TRUING_BEAM_RESULT_BLOCKED_AGAIN,     /* triggered; the beam cleared after the break and was blocked again at the
                                           * last pulse: chatter at the spoke's edge, or a LATER spoke -- the clear
                                           * position and edges_after tell which; this module cannot */
    TRUING_BEAM_RESULT_NO_TRIGGER,        /* armed, a move ran and ended without a beam break */
    TRUING_BEAM_RESULT_INTERRUPTED,       /* triggered, but the coils were released or the drive faulted first */
} truing_beam_result_t;

typedef struct {
    uint32_t trial;            /* 1, 2, ... one per arm */
    bool     blocked_at_arm;   /* armed with the beam already blocked: only the NEXT break triggers */
    bool     saw_motion;       /* the drive was seen moving while this trial was open */
    uint32_t max_step_hz;      /* the ramp the move ran at (recorded once motion is seen; it cannot change mid-move) */
    uint32_t accel_steps_s2;
    bool     fired;
    uint32_t trigger_t_us;
    int32_t  trigger_pos;
    bool     left_beam;        /* the beam cleared after the trigger and before the last pulse */
    int32_t  leave_pos;
    uint32_t edges_after;      /* beam edges between the trigger and the end: the spoke leaving, or chatter */
    bool     ended;
    uint32_t end_t_us;         /* when the poll first saw the pulse train over: poll resolution, not the last pulse */
    int32_t  final_pos;
    bool     blocked_at_end;
    bool     coils_at_end;
    bool     fault_at_end;
    truing_beam_result_t result;
} truing_beam_trial_t;

/* One beam edge, kept for `laser watch`: where the counter was when the beam changed. */
typedef struct {
    uint32_t            seq;   /* 1-based, monotonic across the ring */
    truing_spoke_edge_t edge;
    uint32_t            t_us;
    int32_t             pos;
    bool                moving;
} truing_beam_edge_rec_t;

#define TRUING_BEAM_LOG_LEN 32u

typedef struct {
    /* the input */
    bool     blocked;          /* the level the last edge left it at */
    uint32_t edges, enters, leaves;
    uint32_t glitches;         /* an interrupt that found the level unchanged: a pulse shorter than its latency */
    uint32_t resyncs;          /* edges the poll found that no interrupt reported */
    uint32_t last_edge_t_us;
    /* edge log, newest at log_seq */
    truing_beam_edge_rec_t log[TRUING_BEAM_LOG_LEN];
    uint32_t log_seq;
    /* the laser-stop trial */
    bool     armed;
    uint32_t trials;
    uint32_t stops_requested;
    truing_beam_trial_t cur;   /* the open (or most recently opened) trial */
    truing_beam_trial_t last;  /* the most recent FINISHED trial; result NONE until one finishes */
} truing_beam_stop_t;

void truing_beam_stop_init(truing_beam_stop_t *b, bool blocked_now);

/* Open a trial (one-shot) at drive position `pos`. Re-arming an open, unfired trial replaces it. A trial
 * already triggered and still decelerating is not disturbed: false, and nothing changes. */
bool truing_beam_stop_arm(truing_beam_stop_t *b, int32_t pos);
/* Close an unfired trial without a record. A triggered one runs to its end regardless. */
void truing_beam_stop_disarm(truing_beam_stop_t *b);

/* A beam edge interrupt (ISR-safe: integer only, no calls out). `blocked_now` is the level read in the
 * interrupt; an unchanged level is counted as a glitch. True means: request the drive's controlled stop NOW
 * -- an ENTER while armed and the drive is moving. At most once per arm. */
bool truing_beam_stop_on_edge(truing_beam_stop_t *b, bool blocked_now, uint32_t t_us, int32_t pos, bool moving);
/* The same for an edge the poll found that no interrupt reported (counted in `resyncs`). */
bool truing_beam_stop_resync(truing_beam_stop_t *b, bool blocked_now, uint32_t t_us, int32_t pos, bool moving);

/* Periodic, task context: closes a trial once its move is over. True when a trial finished at this call
 * (it is then in `last`). `coils_on` / `faulted` are the drive's own plain state. */
bool truing_beam_stop_poll(truing_beam_stop_t *b, bool moving, int32_t pos, uint32_t t_us, bool coils_on,
                           bool faulted, uint32_t max_step_hz, uint32_t accel_steps_s2);

/* Edges newer than `since_seq`, oldest first, at most `max`. `lost` = newer edges already overwritten. */
size_t truing_beam_stop_edges_since(const truing_beam_stop_t *b, uint32_t since_seq, truing_beam_edge_rec_t *out,
                                    size_t max, uint32_t *lost);

const char *truing_beam_result_str(truing_beam_result_t r);

#ifdef __cplusplus
}
#endif
#endif /* TRUING_HAL_BEAM_STOP_H */
