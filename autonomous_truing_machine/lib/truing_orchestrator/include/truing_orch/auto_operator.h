/**
 * @file auto_operator.h
 * Auto-operator: a test driver that plays the human. Given the orchestrator's
 * current-state snapshot (SPEC §12.2), it produces the operator intent that
 * answers the active wait — echoing its wait_id (SPEC §7.3) — so the complete
 * Stage 1 workflow can run with zero hardware (SPEC §14.5) on the host and,
 * as a self-play demonstration, on the target.
 *
 * It also carries a deliberately SYNTHETIC wheel model: when it "applies" an
 * adjustment of `turns` on spoke i, the lateral runout at rim index i changes by
 * `+mm_per_turn * turns` (the sense the synthetic calculation double corrects with
 * turns = -k * lateral). This is test physics, not a model of any wheel; the real
 * sign conventions of SPEC §6.4 are the artifact generator's business (Phase 1b).
 */
#ifndef TRUING_ORCH_AUTO_OPERATOR_H
#define TRUING_ORCH_AUTO_OPERATOR_H

#include <stdbool.h>
#include <stdint.h>

#include "truing/limits.h"
#include "truing/operator_intent.h"
#include "truing_orch/orchestrator.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool  enabled;
    float lateral_mm[TRUING_MAX_RIM_ANGLES];
    float radial_mm[TRUING_MAX_RIM_ANGLES];
    float mm_per_turn;            /* synthetic response of lateral runout to one nipple turn */
} truing_auto_wheel_model_t;

/* Optional override: supply runout values per (rim index, cycle) instead of the wheel model. */
typedef bool (*truing_auto_runout_fn)(void *user, uint8_t rim_index, uint8_t cycle_index, float *lateral_mm, float *radial_mm);

typedef struct {
    truing_auto_wheel_model_t wheel;
    truing_auto_runout_fn     runout_fn;
    void                     *user;
    uint32_t                  answers;
    uint32_t                  positions_confirmed;
    uint32_t                  runouts_entered;
    uint32_t                  adjustments_applied;
    float                     turns_applied[TRUING_MAX_SPOKES];   /* cumulative per spoke */
} truing_auto_operator_t;

void truing_auto_operator_init(truing_auto_operator_t *a);
void truing_auto_operator_set_wheel(truing_auto_operator_t *a, uint8_t n_rim_angles, const float *lateral_mm,
                                    const float *radial_mm, float mm_per_turn);

/* Builds the intent answering the snapshot's active wait. False when nothing is waiting. */
bool truing_auto_operator_answer(truing_auto_operator_t *a, const truing_orch_snapshot_t *snap, truing_intent_t *out);

/* Drives the orchestrator until it is idle, terminal, or `max_steps` steps have run.
 * Waits are answered by the auto-operator; delays and navigation polls are simply
 * stepped through. Returns the last step result. */
truing_orch_step_t truing_orch_run_auto(truing_orchestrator_t *o, truing_auto_operator_t *a, uint32_t max_steps,
                                        uint32_t *steps_used);

#ifdef __cplusplus
}
#endif
#endif /* TRUING_ORCH_AUTO_OPERATOR_H */
