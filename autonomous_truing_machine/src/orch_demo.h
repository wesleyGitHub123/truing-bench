/**
 * @file orch_demo.h
 * On-device self-play of the Stage 1 workflow (Phase 1d bring-up): the orchestrator
 * runs on core 0 against the manual navigation and manual runout implementations, with
 * the auto-operator playing the human, synthetic acoustic estimates and the synthetic
 * calculation double. Demonstrates the state machine, operator waits, wait-instance
 * correlation, telemetry and provenance on the target. Not a demonstration of truing.
 */
#ifndef TRUING_ORCH_DEMO_H
#define TRUING_ORCH_DEMO_H

void truing_orch_demo_start(void);

#endif /* TRUING_ORCH_DEMO_H */
