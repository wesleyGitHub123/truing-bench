/**
 * @file navigation_laser.h
 * LASER-GUIDED roller navigation for the tension-survey demonstration image (TRUING_DEMO_LASER_NAV).
 *
 * The firmware form of the alignment campaign's revolution sequencer (tools/align_revolution.py and the
 * workbench's `align trial`, docs/ALIGN_CAMPAIGN.md): the roller turns the wheel
 * forward until spoke k breaks the beam at the strike point, the per-class recipe stops it there, and the
 * spoke is NAMED from the wheel's measured 4-spoke spacing before anything strikes -- except the first
 * trigger after a placement or a rescale, which is trusted (nothing to name it against; the PC script
 * re-located first instead, which is not ported). A failed or misnamed stop
 * is backed up and retried a bounded number of times; then the spoke is handed to the operator as a MANUAL
 * positioning wait (the page's roller jog), and the next spoke resumes automatically.
 *
 * Pure: every hardware access goes through truing_laser_port_t, so the sequencing is host-tested against a
 * simulated wheel. The real controller (truing_hal/beam_align.h) runs behind the port on the board. The
 * bench's 32/32 revolution was the PC sequencer's; this port has not yet run on the bench.
 *
 * Spoke numbers inside are UNWRAPPED (they keep counting past 31) so spacing sums stay simple; a request
 * names spoke index % n_spokes. Navigation moves FORWARD only (the session goes 0, 1, 2, ...): a request
 * that is not 1..4 spokes ahead of the belief is handed to the operator.
 */
#ifndef TRUING_HAL_NAVIGATION_LASER_H
#define TRUING_HAL_NAVIGATION_LASER_H

#include <stdbool.h>
#include <stdint.h>

#include "truing_hal/beam_align.h"
#include "truing_hal/navigation_manual.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One spoke class's alignment recipe (spoke mod 4). Bench-tuned on the demo wheel, 2026-10-03, by the
 * operator judging every hit by eye (docs/ALIGN_CAMPAIGN.md); the stop distance is v^2/(2a) of the ramp. */
typedef struct {
    const char             *name;
    uint32_t                hz, accel;       /* approach ramp, steps/s and steps/s^2 */
    int32_t                 offset_steps;    /* REACQUIRE*: extra steps along dir at the end */
    truing_align_strategy_t strategy;
    int32_t                 stop_steps;      /* expected distance from the trigger to the first stop */
} truing_laser_recipe_t;

typedef struct {
    truing_laser_recipe_t class_recipe[4];
    int32_t  gap_steps[4];       /* enter-to-enter gap AFTER a spoke of class c (the wheel's spacing repeats every 4) */
    int32_t  id_tol_steps;       /* a trigger this close to a spoke's expected edge is that spoke */
    int32_t  width_min, width_max;   /* a blocked width outside this means the step scale or the beam is off */
    int32_t  stop_tol_steps;     /* the first stop may differ from its class value by this much */
    uint32_t chatter_edges;      /* more beam edges than this in one trial: chatter, discard it */
    uint8_t  max_tries;          /* automatic attempts per spoke before the operator is asked */
    int8_t   dir;                /* drive direction that brings the next spoke (index_dir +1 on the bench) */
    uint32_t max_steps;          /* approach travel bound */
    uint32_t settle_ms;
    uint32_t slow_hz, slow_accel;    /* correction ramp */
    uint32_t reacq_max_steps;
    uint32_t backup_hz, backup_accel;
} truing_laser_nav_config_t;

/* The demo wheel's measured values (align_revolution.py RECIPE/GAP and the `align1` session). */
void truing_laser_nav_config_bench_2026_10_03(truing_laser_nav_config_t *out);

/* What the port reports about the current / last controller run. */
typedef struct {
    bool                  active;
    truing_align_result_t result;
    bool                  fired;
    int32_t               trigger_pos;
    int32_t               stop_pos;
    int8_t                final_zone;
    bool                  final_blocked;
    int32_t               width_used;    /* -1: not measured */
} truing_laser_trial_t;

/* The hardware behind the sequencer. Every call is from task context. */
typedef struct {
    /* Energise the coils (the driver rewrites and verifies its microstep setting). `*rescaled` is set when the
     * setting had to be repaired: every step position before this call is in another scale. */
    bool (*enable)(void *user, bool *rescaled);
    /* Set the ramp and start one controller run from a still drive. */
    bool (*start_trial)(void *user, const truing_align_config_t *cfg, uint32_t hz, uint32_t accel);
    bool (*trial)(void *user, truing_laser_trial_t *out);
    /* Set the ramp and start a plain relative move. */
    bool (*move)(void *user, int32_t steps, uint32_t hz, uint32_t accel);
    bool (*moving)(void *user);
    int32_t (*position)(void *user);
    bool (*beam_blocked)(void *user);
    uint32_t (*beam_edges)(void *user);   /* a running count of every beam edge */
    void (*stop)(void *user);             /* end any run and request the drive's controlled stop */
    void (*release)(void *user);          /* de-energise the coils: the operator turns the wheel by hand */
    void *user;
} truing_laser_port_t;

typedef enum {
    TRUING_LASER_NAV_IDLE = 0,
    TRUING_LASER_NAV_TRIAL,      /* a controller run is moving the wheel to spoke k */
    TRUING_LASER_NAV_BACKUP,     /* backing up to the gap before spoke k for another try */
    TRUING_LASER_NAV_MANUAL,     /* spoke k was handed to the operator */
    TRUING_LASER_NAV_DONE,
} truing_laser_nav_phase_t;

typedef struct {
    truing_laser_nav_config_t cfg;
    truing_laser_port_t       port;
    truing_navigation_if_t    manual;        /* position bookkeeping and the operator fallback */
    truing_navigation_manual_ctx_t mctx;
    uint8_t                   n_spokes;
    truing_laser_nav_phase_t  phase;
    truing_nav_target_t       target;
    int32_t                   k;             /* the requested spoke, unwrapped */
    uint8_t                   tries;
    uint32_t                  edges0;
    /* belief: the last spoke at or just behind the beam (unwrapped), and the beam level the last stop left */
    bool                      belief_valid;
    int32_t                   belief;
    bool                      belief_level_known;
    bool                      belief_blocked;
    /* anchor: a trusted (entering-edge position, spoke) pair that names later triggers by spacing */
    bool                      anchor_valid;
    int32_t                   anchor_pos;
    int32_t                   anchor_spoke;
    int16_t                   at_station;    /* spoke index the last DONE left at the station, -1 none */
    /* counters, for the log and the tests */
    uint32_t                  trials, aligned, backups, handovers, misnamed, chatter, rescales;
    const char               *last_why;      /* why the last attempt was not accepted (static string) */
} truing_navigation_laser_ctx_t;

void truing_navigation_laser_init(truing_navigation_if_t *self, truing_navigation_laser_ctx_t *ctx,
                                  truing_clock_if_t clock, uint8_t n_spokes, const truing_machine_profile_t *machine,
                                  const truing_laser_nav_config_t *cfg, const truing_laser_port_t *port,
                                  truing_source_impl_t source_impl);

/* Steps from spoke a's entering edge to spoke b's under the configured spacing (negative when b is behind a). */
int32_t truing_laser_nav_offset(const truing_laser_nav_config_t *cfg, int32_t a, int32_t b);
/* Which spoke's entering edge is `trigger`, given the trusted anchor; false when none is within id_tol. */
bool truing_laser_nav_identify(const truing_laser_nav_config_t *cfg, int32_t anchor_pos, int32_t anchor_spoke,
                               int32_t trigger, int32_t *spoke_out, int32_t *err_out);

#ifdef __cplusplus
}
#endif
#endif /* TRUING_HAL_NAVIGATION_LASER_H */
