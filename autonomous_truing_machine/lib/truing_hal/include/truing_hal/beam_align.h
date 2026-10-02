/**
 * @file beam_align.h
 * Laser-guided spoke ALIGNMENT controller -- the real-time half of the roller / laser / acoustic campaign
 * (docs/ALIGN_CAMPAIGN.md). It answers one question per run, on the board, in real time:
 *
 *     the laser saw THIS spoke break the beam  ->  what did the roller do  ->  is THAT SAME spoke in the
 *     beam when everything has stopped?
 *
 * A run: move along `dir` at the drive's current ramp, count beam breaks, and on the Nth break request the
 * drive's own controlled stop (from the beam interrupt). After a settle, apply the configured correction
 * strategy, settle again, and classify. Strategies are the campaign's competing candidates:
 *
 *     STOP            A: trigger -> controlled stop -> evaluate
 *     REVERSE_FIXED   B: ... -> stop -> reverse a fixed step count at the slow ramp -> evaluate
 *     RETURN_TO_EDGE  B': ... -> stop -> step-based return to the trigger edge (+ half the measured blocked
 *                     width if `center`, + `offset_steps`) using the counter and the MEASURED overshoot
 *     REACQUIRE       C: ... -> stop -> slow reverse until the laser re-finds the same spoke (edge accounting,
 *                     below), controlled stop, optional centering by half the measured width -> evaluate
 *     REACQUIRE_CLEAR D: ... -> stop -> slow reverse, re-find the same spoke, KEEP REVERSING through it until the
 *                     beam reads clear again on its far side (zone -1), controlled stop there, then `offset_steps`
 *                     more along dir (negative = further back from the spoke; 0 = none) -> evaluate. The success
 *                     state is "the target spoke has just been cleared": zone -1, beam clear. No centering.
 *
 * WHICH SPOKE IS IN THE BEAM: zone accounting. At the trigger the target spoke is in the beam: zone 0. Every
 * beam edge while the drive moves shifts the zone by +1 when the motion is along the approach direction and
 * -1 against it. Even zone = a spoke is in the beam (zone/2 spokes ahead of the target), odd = a gap. So
 * "the same spoke is back in the beam" is exactly "zone 0 and the beam blocked"; the beam level's parity
 * must agree with the zone or the run is AMBIGUOUS. Edges while the drive is still (settle spring-back, a
 * pluck) have no known direction: they are counted, and accepted only if the parity still agrees.
 *
 * Alignment means "the target spoke blocks the beam". That is alignment WITH THE STATION only when the beam
 * crosses the strike point; the campaign records that as an explicit assumption, it is not established here.
 *
 * Pure and framework-free (host-tested in test/test_hal_drive). The target glue (src/laser_bench.c) calls
 * on_edge() from the beam interrupt -- its only output there is "request the controlled stop now" -- and
 * tick() from a 2 ms task. Drive COMMANDS (ramp change, move) are returned in truing_align_cmd_t and executed
 * by the caller outside any lock; the controller never starts motion on its own outside a run it was asked to
 * start, never re-times the drive's stop, and treats any motion it did not command as an abort. Step counts
 * are pulses sent, never proof the wheel followed: the beam is the physical observation.
 */
#ifndef TRUING_HAL_BEAM_ALIGN_H
#define TRUING_HAL_BEAM_ALIGN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TRUING_ALIGN_MODE_COUNT = 1,   /* stage 0: stop on the Nth break, no correction: laser count reliability */
    TRUING_ALIGN_MODE_ALIGN = 2,   /* stages 1-3: stop on the Nth break, then the strategy */
} truing_align_mode_t;

typedef enum {
    TRUING_ALIGN_STRAT_STOP = 0,
    TRUING_ALIGN_STRAT_REVERSE_FIXED,
    TRUING_ALIGN_STRAT_RETURN_TO_EDGE,
    TRUING_ALIGN_STRAT_REACQUIRE,
    TRUING_ALIGN_STRAT_REACQUIRE_CLEAR,
    TRUING_ALIGN_STRAT__COUNT
} truing_align_strategy_t;

/* Every value here is a CAMPAIGN PARAMETER chosen by the operator for one run -- none is a calibration. */
typedef struct {
    truing_align_mode_t     mode;
    truing_align_strategy_t strategy;
    int8_t   dir;              /* +1 / -1: approach direction in drive steps */
    uint16_t target_break;     /* request the stop on this beam break of the run (1-based) */
    uint32_t max_steps;        /* approach travel bound: no target break within it = LASER_MISSED */
    uint32_t settle_ms;        /* wait after each stop before reading the beam */
    uint32_t reverse_steps;    /* REVERSE_FIXED: magnitude, applied against dir */
    int32_t  offset_steps;     /* RETURN_TO_EDGE / REACQUIRE: extra signed steps along dir at the end */
    bool     center;           /* RETURN_TO_EDGE / REACQUIRE: aim for the middle of the measured blocked span */
    uint32_t slow_hz;          /* correction moves' ramp */
    uint32_t slow_accel;
    uint32_t reacq_max_steps;  /* REACQUIRE: how far the reverse search may go */
} truing_align_config_t;

typedef enum {
    TRUING_ALIGN_PHASE_IDLE = 0,
    TRUING_ALIGN_PHASE_APPROACH,      /* moving, counting breaks */
    TRUING_ALIGN_PHASE_STOPPING,      /* trigger seen, the drive's controlled stop decelerating */
    TRUING_ALIGN_PHASE_SETTLE,        /* stopped; waiting settle_ms before reading the beam */
    TRUING_ALIGN_PHASE_CORRECT,       /* the strategy's correction move */
    TRUING_ALIGN_PHASE_TRIM,          /* REACQUIRE's centering / offset move */
    TRUING_ALIGN_PHASE_FINAL_SETTLE,
    TRUING_ALIGN_PHASE_DONE,
} truing_align_phase_t;

typedef enum {
    TRUING_ALIGN_RESULT_NONE = 0,
    TRUING_ALIGN_RESULT_COUNT_DONE,             /* stage 0: the target break was reached and the drive stopped */
    TRUING_ALIGN_RESULT_ALIGNED,                /* zone 0, beam blocked: the triggering spoke is in the beam */
    TRUING_ALIGN_RESULT_OVERSHOOT,              /* ended past the target (zone > 0) */
    TRUING_ALIGN_RESULT_UNDERSHOOT,             /* ended short of the target (zone < 0) */
    TRUING_ALIGN_RESULT_LASER_MISSED,           /* the travel bound ran out before the target break */
    TRUING_ALIGN_RESULT_LASER_AMBIGUOUS,        /* beam level contradicts the zone, or the same-spoke check failed */
    TRUING_ALIGN_RESULT_REVERSE_FAILED,         /* a step-based correction ended off the target */
    TRUING_ALIGN_RESULT_REACQUISITION_FAILED,   /* the reverse search ran out without re-finding the spoke */
    TRUING_ALIGN_RESULT_ABORTED,                /* operator abort, a manual stop, or motion nobody here commanded */
    TRUING_ALIGN_RESULT_FAULT,                  /* coils released or the driver faulted */
} truing_align_result_t;

typedef enum {
    TRUING_ALIGN_DETAIL_NONE = 0,
    TRUING_ALIGN_DETAIL_OPERATOR_ABORT,
    TRUING_ALIGN_DETAIL_STOPPED_EARLY,          /* the approach ended short of max_steps and not by the trigger */
    TRUING_ALIGN_DETAIL_UNEXPECTED_MOTION,      /* the drive moved while the controller was waiting */
    TRUING_ALIGN_DETAIL_DRIVE_REFUSED,          /* a commanded ramp change or move was refused */
    TRUING_ALIGN_DETAIL_COILS_OFF,
    TRUING_ALIGN_DETAIL_DRIVER_FAULT,
    TRUING_ALIGN_DETAIL_PARITY,                 /* beam level and zone disagree */
    TRUING_ALIGN_DETAIL_SAME_SPOKE,             /* reacquired more than half a measured pitch from where it left */
} truing_align_detail_t;

/* A drive command for the caller to execute (outside any lock), in this order. */
typedef struct {
    bool     set_motion;
    uint32_t hz, accel;
    bool     move;
    int32_t  steps;
} truing_align_cmd_t;

typedef struct {
    uint32_t t_us;
    int32_t  pos;
    int8_t   sign;     /* +1 along the approach, -1 against, 0 the drive was still */
    bool     enter;    /* true: the beam became blocked */
    uint8_t  phase;    /* truing_align_phase_t when it happened */
    int8_t   zone;     /* zone after this edge (meaningful once the trigger has set it) */
} truing_align_edge_t;

typedef struct {
    uint8_t  phase;
    uint32_t t_us;
    int32_t  pos;
    bool     blocked;
} truing_align_event_t;

#define TRUING_ALIGN_EDGE_LOG  160u
#define TRUING_ALIGN_EVENT_LOG 24u

typedef struct {
    truing_align_config_t  cfg;
    truing_align_phase_t   phase;
    truing_align_result_t  result;
    truing_align_detail_t  detail;
    uint32_t run;
    bool     blocked;              /* the beam level as the controller last saw it */
    int8_t   move_sign;            /* sign of the move in progress relative to cfg.dir */
    bool     cmd_outstanding;      /* a move was handed to the caller and not yet seen running or done */
    int32_t  cmd_pos;              /* where the drive was when that move was handed out */
    uint32_t cmd_t_us;
    uint32_t phase_t_us;
    bool     profile_switched;     /* the slow ramp was applied; restore the approach ramp when done */
    bool     restore_pending;
    /* start */
    int32_t  start_pos;
    bool     start_blocked;
    uint32_t start_t_us;
    uint32_t approach_hz, approach_accel;
    /* approach and trigger */
    uint16_t breaks;
    bool     fired;
    uint32_t trigger_t_us;
    int32_t  trigger_pos;
    bool     zone_valid;
    int8_t   zone;
    bool     parity_bad;
    uint16_t edges_still;
    /* first stop */
    int32_t  stop_pos;
    uint32_t stop_t_us;
    bool     blocked_after_stop;
    int8_t   zone_after_stop;
    uint16_t edges_in_stop;        /* edges between the trigger and the end of the deceleration */
    bool     target_left;
    int32_t  target_leave_pos;
    int32_t  pitch_med, width_med; /* measured on this run's approach (-1 = not enough edges) */
    int32_t  pitch_min;            /* the shortest enter-to-enter gap on this run's approach (-1 = none): the wheel's spacing is not uniform */
    int32_t  width_used;           /* the blocked width used for centering (-1 = none available) */
    /* correction */
    int32_t  corr_cmd;
    int32_t  corr_start_pos;
    uint16_t corr_edges;
    bool     reacquired;
    uint16_t reacquisitions;
    int32_t  reacq_pos;
    uint32_t reacq_t_us;
    bool     cleared;              /* REACQUIRE_CLEAR: the beam read clear again past the spoke (zone -1) */
    int32_t  clear_pos;
    bool     same_spoke_checked, same_spoke_ok;
    int32_t  trim_cmd;
    /* end */
    int32_t  final_pos;
    bool     final_blocked;
    int8_t   final_zone;
    uint32_t end_t_us;
    /* logs */
    truing_align_edge_t  edges[TRUING_ALIGN_EDGE_LOG];
    uint16_t n_edges, edges_dropped;
    truing_align_event_t events[TRUING_ALIGN_EVENT_LOG];
    uint16_t n_events;
} truing_beam_align_t;

void truing_beam_align_init(truing_beam_align_t *a, bool blocked_now);

/* Validate `cfg` and open a run from a still drive at `pos`. On success `cmd` holds the approach move. NULL
 * `why` is fine; it receives a short reason on refusal. The caller executes `cmd` and reports a refused move
 * with truing_beam_align_command_failed(). */
bool truing_beam_align_start(truing_beam_align_t *a, const truing_align_config_t *cfg, int32_t pos, bool moving,
                             bool blocked_now, uint32_t t_us, uint32_t hz, uint32_t accel, truing_align_cmd_t *cmd,
                             const char **why);

/* Beam edge interrupt (ISR-safe: integer only, no calls out). True: request the drive's controlled stop now. */
bool truing_beam_align_on_edge(truing_beam_align_t *a, bool blocked_now, uint32_t t_us, int32_t pos, bool moving);

/* Periodic, task context. Fills `cmd` when the controller needs the drive to do something; true when the run
 * finished at this call. */
bool truing_beam_align_tick(truing_beam_align_t *a, uint32_t t_us, int32_t pos, bool moving, bool coils_on,
                            bool faulted, truing_align_cmd_t *cmd);

/* The caller could not execute the last command (a refused ramp change or move): the run ends FAULT. */
void truing_beam_align_command_failed(truing_beam_align_t *a, uint32_t t_us, int32_t pos);

/* Operator abort. True when a run was active (the caller then requests the drive's controlled stop). */
bool truing_beam_align_abort(truing_beam_align_t *a, uint32_t t_us, int32_t pos);

bool truing_beam_align_active(const truing_beam_align_t *a);

/* Is a re-found edge `offset` steps from where the target left the beam still the target's? Any other spoke's edge is at
 * least the shortest gap away, so more than half of that is not this spoke. `min_pitch` <= 0: not known, no verdict (true). */
bool truing_beam_align_same_spoke(int32_t offset, int32_t min_pitch);

const char *truing_align_phase_str(truing_align_phase_t p);
const char *truing_align_result_str(truing_align_result_t r);
const char *truing_align_detail_str(truing_align_detail_t d);
const char *truing_align_strategy_str(truing_align_strategy_t s);
/* Parses a strategy name as printed by truing_align_strategy_str(); false if unknown. */
bool truing_align_strategy_parse(const char *s, truing_align_strategy_t *out);

#ifdef __cplusplus
}
#endif
#endif /* TRUING_HAL_BEAM_ALIGN_H */
