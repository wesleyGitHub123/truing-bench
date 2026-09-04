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

void truing_orch_demo_start(void);

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
