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

/* Requests the path for the next session. False if this image has no automatic path, or a
 * session is currently active. `detail` (optional) receives a short reason. */
bool truing_demo_request_acquisition(bool automatic, const char **detail);

#endif /* TRUING_ORCH_DEMO_H */
