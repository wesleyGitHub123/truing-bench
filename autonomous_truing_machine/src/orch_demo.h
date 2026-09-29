/**
 * @file orch_demo.h
 * On-device run of the Stage 1 workflow: the orchestrator runs on core 0 against real
 * HAL implementations, with synthetic acoustic estimates and the artifact-backed
 * calculation. Demonstrates the state machine, operator waits, wait-instance correlation,
 * telemetry and provenance on the target. Not a demonstration of truing.
 */
#ifndef TRUING_ORCH_DEMO_H
#define TRUING_ORCH_DEMO_H

#include <stdbool.h>

#include "truing/config.h"
#include "truing_hal/acoustic_real.h"

void truing_orch_demo_start(void);

/* ---- capture evidence (SPEC §12.5 debug channel) ---------------------------------------
 *
 * The samples behind the last acoustic measurement, so a pluck that went wrong on the bench
 * can leave the board as a file and be replayed on a host (test/test_acoustic_replay). The
 * composition root owns the acoustic subsystem, so it is the only place that can hand them
 * out; the orchestrator's contract is untouched and nothing in the control path is aware
 * this exists.
 *
 * False before anything has been measured. Whether the words came from a microphone or from
 * the synthetic source is not the caller's guess to make - `view.words` is accompanied by the
 * subsystem's source_impl through truing_demo_acoustic_source(), and the dump records it. */
bool truing_demo_last_capture(truing_acoustic_capture_view_t *out);
uint32_t truing_demo_capture_seq(void);
/* True iff a capture exists whose outcome is not yet settled - an attempt is mid-flight and
 * truing_demo_last_capture()'s status/reason may still describe the PREVIOUS attempt even
 * though the words are the new one's. The debug HTTP handlers check this before reading
 * anything (SPEC §12.5/§13.3): a 409 here, not a half-finished 200. */
bool truing_demo_capture_pending(void);
/* The other microphone's record of the last capture on a dual-mic build: same frames, never
 * analysed. Guard it with truing_demo_capture_seq() exactly like the capture. False otherwise. */
bool truing_demo_far_capture(const int32_t **words, uint32_t *n_words, uint8_t *input);
/* Live listening for GET /debug/listen.pcm: start from now, then poll for the station's new samples.
 * Follows the ring read-only, so it cannot disturb a measurement. False / 0 without a real front end. */
bool truing_demo_listen_start(uint32_t *pos);
/* One excitation pulse on a station's actuator (0 LEFT, 1 RIGHT), blocking until it is done: the bench
 * burst that GET /debug/listen.pcm schedules between reads. False when that station has no configured
 * actuator (its BOARD_PLUCK_ACTUATOR_*_PRESENT is 0) or the pulse is refused. */
bool truing_demo_fire(uint32_t station, float pulse_ms);
bool truing_demo_fire_ready(uint32_t station);   /* that station has a configured actuator */
uint32_t truing_demo_listen_read(uint32_t station, uint32_t *pos, int32_t *out, uint32_t max, uint32_t *lost);

/* The lab image's bench drive (TRUING_BENCH_DRIVE), for GET /debug/drive.json. */
typedef struct {
    bool     enabled;          /* coils energised */
    bool     moving;
    bool     faulted;          /* latched */
    bool     diag_high;        /* the DIAG pin right now, unfiltered */
    bool     uart_ok;
    uint32_t fault_detail;
    int32_t  position_steps;   /* pulses actually emitted since the last zero: commanded, not evidence the wheel moved */
    uint32_t max_step_hz;
    uint32_t accel_steps_s2;
} truing_demo_drive_state_t;
/* False when this image has no bench drive (or it failed to initialise); `out` is then untouched. */
bool truing_demo_bench_drive_state(truing_demo_drive_state_t *out);
truing_source_impl_t truing_demo_acoustic_source(void);

/* The acoustic chain configuration the measurement ran under. Its digest is what lets a
 * stored capture refuse replay under different DSP constants. NULL before start-up. */
const truing_chain_profile_t *truing_demo_chain_profile(void);
/* How the acoustic stations' actuators excite a spoke; its own digest, separate from the chain's. */
const truing_excitation_profile_t *truing_demo_excitation_profile(void);

/* ---- acquisition path (FAST DEMO image only) ------------------------------------------
 * Which implementations satisfy navigation and runout for the NEXT session:
 *
 *   manual     navigation_manual + runout_manual, both TRUING_SOURCE_REAL. The machine
 *              stops and asks a person to position the wheel and read the dial gauges -
 *              64 operator answers per cycle. This is the physical path.
 *   automatic  navigation_synthetic + runout_synthetic, both TRUING_SOURCE_SYNTHETIC.
 *              The implementations answer for themselves, so the acquisition runs
 *              unattended. Not a physical result.
 *              EXCEPT on the acoustic demonstration (a REAL front end, not the campaign bench
 *              image): there navigation is navigation_composite -- the operator places and confirms
 *              each struck spoke at its acoustic station, and only runout/adjustment positioning is
 *              simulated. It is still labelled SYNTHETIC (the weakest of its parts) and runout is
 *              still synthetic, so it is still not a physical wheel result; but the run is no longer
 *              unattended. The campaign bench image keeps synthetic navigation: it runs MEASURE_ONCE
 *              outside a session, with the physical spoke declared per shot.
 *
 * The choice is a property of the SESSION, not of a running one: it can only be applied
 * while the machine is idle, and applying it re-initialises the orchestrator so the whole
 * BOOT -> INITIALIZE -> READY sequence runs again against the newly selected
 * implementations. Session admission then records what actually ran (SPEC §6.2), which is
 * why this cannot make a session lie about itself: no session is in progress when it
 * changes, and the next one is admitted with the truth.
 *
 * In every other image the request is refused and the manual path is the only path, so
 * the ordinary firmware has no way to reach the synthetic implementations at all. */
bool truing_demo_acquisition_is_automatic(void);
/* True when navigation is the composite (the operator places each struck spoke, the rest is simulated).
 * Reported by /id as a fact so the page never infers it from real_front_end + acquisition. */
bool truing_demo_navigation_is_composite(void);

/* Requests the path for the next session. False if this image has no automatic path, or a
 * session is currently active. `detail` (optional) receives a short reason. */
bool truing_demo_request_acquisition(bool automatic, const char **detail);

#endif /* TRUING_ORCH_DEMO_H */
