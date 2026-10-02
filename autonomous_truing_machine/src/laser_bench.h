/* Bench laser-stop on the lab image (TRUING_BENCH_DRIVE): the laser break-beam receiver on
 * BOARD_SPOKE_BEAM_GPIO as an observable bench subsystem, and an optional one-shot trigger that asks the
 * bench drive for its own controlled stop on a beam break (truing_hal/beam_stop.h). A characterization
 * tool, not navigation: nothing here knows a spoke or a wheel angle, and nothing becomes wheel state.
 *
 * Served as GET /debug/laser.json (net_transport.c); driven by `laser` in tools/workbench.py. Arming it
 * needs no admission: it can only ever request a stop that the drive's own stop() would honour from
 * anyone -- never start, extend or re-time motion -- so `roll stop`, `roll off` and a driver fault keep
 * the authority they had. Wiring and the meter checks before power: docs/LASER_BENCH.md. */
#ifndef TRUING_LASER_BENCH_H
#define TRUING_LASER_BENCH_H

#include <stdbool.h>
#include <stdint.h>

#include "truing_hal/beam_align.h"
#include "truing_hal/beam_stop.h"
#include "truing_hal/wheel_drive_if.h"
#include "wheel_drive_tmc2209.h"

typedef enum {
    TRUING_LASER_INPUT_UNPROBED = 0,
    TRUING_LASER_INPUT_DRIVEN,     /* the pin read the same with the internal pull-up and pull-down: something drives it */
    TRUING_LASER_INPUT_FLOATING,   /* it followed the internal pull: nothing (or nothing strong enough) is connected */
} truing_laser_input_t;

typedef struct {
    bool                ready;     /* configured: the lab image and the GPIO/interrupt set up */
    int                 gpio;
    int                 level;     /* the raw pin level now */
    truing_laser_input_t input;
    uint32_t            probed_age_ms;
    uint32_t            now_us;    /* the board's microsecond clock when this was taken, for ages */
} truing_laser_bench_io_t;

/* Configure the receiver input and start the poll. `drive` is the bench drive the trigger stops; `dctx` its
 * own state, read plainly for coils/fault/ramp. False when the pin or interrupt could not be set up. */
bool truing_laser_bench_init(truing_wheel_drive_if_t *drive, const truing_wheel_drive_tmc2209_ctx_t *dctx);

/* A consistent copy of the trigger state (taken under its lock) and of the input. False: not configured. */
bool truing_laser_bench_snapshot(truing_beam_stop_t *out, truing_laser_bench_io_t *io);

/* Arm (one-shot) or disarm the laser-stop. Arming while a triggered stop is still decelerating is refused. */
bool truing_laser_bench_arm(bool on, const char **detail);

/* Re-check whether the pin is driven, by reading it with each internal pull. Refused (false) while armed or
 * while the drive moves: a pull toggle on a floating pin would look like a beam edge. */
bool truing_laser_bench_probe(const char **detail);

/* The alignment campaign's real-time controller (truing_hal/beam_align.h), fed by the same beam interrupt and
 * poll as the laser-stop; at most one of the two owns the stop decision at a time. GET /debug/align.json.
 * Start: refused with the coils off, a latched fault, the laser-stop armed, a run in progress or the drive
 * moving. Abort: ends the run ABORTED and requests the drive's controlled stop. */
bool truing_laser_align_start(const truing_align_config_t *cfg, const char **detail);
bool truing_laser_align_abort(const char **detail);
bool truing_laser_align_snapshot(truing_beam_align_t *out);

#endif /* TRUING_LASER_BENCH_H */
