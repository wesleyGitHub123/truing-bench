/**
 * @file orchestrator.h
 * The Orchestrator (SPEC §5.1, §7): owns the state machine, the outer cycle loop and
 * the inner per-spoke apply loop, and sequences everything else. It holds no
 * measurement logic and no drive mechanics: every subsystem is reached through its
 * contract, and Wheel Navigation is consumed by OUTCOME alone (SPEC §10A).
 *
 * Execution model — cooperative steps. truing_orch_step() performs one bounded piece
 * of autonomous work and reports what it needs next: nothing (idle / waiting for an
 * operator intent), a delay (excitation settle), a navigation poll, or that the
 * session reached a terminal result. A FreeRTOS task on core 0 (SPEC §4.5) drives it;
 * host tests drive it directly. Framework-free (P3).
 *
 * Operator intents arrive through truing_orch_submit_intent(), which applies the
 * SPEC §12.3 per-intent admissibility and SPEC §7.3 wait-instance correlation and
 * records the answer; state transitions happen only inside truing_orch_step().
 */
#ifndef TRUING_ORCH_ORCHESTRATOR_H
#define TRUING_ORCH_ORCHESTRATOR_H

#include <stdbool.h>
#include <stdint.h>

#include "truing/admission.h"
#include "truing/config.h"
#include "truing/operator_intent.h"
#include "truing/plan.h"
#include "truing/row_layout.h"
#include "truing/session.h"
#include "truing/state_ids.h"
#include "truing/wheel_state.h"
#include "truing_calc/calc_if.h"
#include "truing_hal/acoustic_if.h"
#include "truing_hal/clock_if.h"
#include "truing_hal/navigation_if.h"
#include "truing_hal/runout_if.h"
#include "truing_hal/telemetry_if.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TRUING_ORCH_ADVANCED = 0,       /* did work; call again */
    TRUING_ORCH_IDLE,               /* READY, or INITIALIZE failed: nothing to do until an intent arrives */
    TRUING_ORCH_WAITING_OPERATOR,   /* WAIT_FOR_OPERATOR: nothing to do until the matching intent arrives */
    TRUING_ORCH_DELAY,              /* autonomous delay (requested_delay_ms) before calling again */
    TRUING_ORCH_POLL_NAVIGATION,    /* automated navigation in motion: call again after a short interval */
    TRUING_ORCH_TERMINAL,           /* session finished; result in the snapshot */
} truing_orch_step_t;

typedef struct {
    const truing_wheel_class_config_t    *wheel;
    const truing_solver_config_t         *solver;         /* copied at init; session-mutable/fixed values apply to the copy */
    const truing_chain_profile_t         *chain;
    const truing_tension_model_profile_t *tension_model;  /* the loaded profile; must match solver.tension_model_profile_id */
    const truing_machine_profile_t       *machine;
    truing_acoustic_if_t   *acoustic;
    truing_runout_if_t     *runout;
    truing_navigation_if_t *navigation;
    truing_calc_if_t       *calc;
    truing_telemetry_if_t  *telemetry;          /* optional; never affects control flow */
    truing_clock_if_t       clock;
    const char             *firmware_version;
    bool                    geometry_only_policy;   /* SPEC §8.11 sanctioned policy exclusion of the tension channel */
    bool                    debug_channel_enabled;  /* SPEC §12.5: development only */
    /* 0 = collect every spoke, which is what the machine does and what every interactive image
     * sets. A demonstration image may bound the acoustic pass, and the orchestrator honours the
     * bound ONLY while the selected layout is TENSION_ABSENT, i.e. while those rows are not in
     * the solve at all (SPEC §8.11). Under any layout that uses tension this is ignored. */
    uint8_t                 tension_sample_limit;
} truing_orch_deps_t;

/* What the machine resumes with once the active wait is answered. */
typedef enum {
    TRUING_ORCH_CONT_NONE = 0,
    TRUING_ORCH_CONT_REFERENCE,        /* spoke 0 confirmed at the reference station -> READY */
    TRUING_ORCH_CONT_POSITION_SPOKE,   /* spoke at acoustic station -> MEASURE_SPOKE_TENSION */
    TRUING_ORCH_CONT_POSITION_RIM,     /* rim index at runout station -> READ_RUNOUT */
    TRUING_ORCH_CONT_RUNOUT_ENTRY,     /* SUBMIT_RUNOUT received -> READ_RUNOUT */
    TRUING_ORCH_CONT_POSITION_APPLY,   /* spoke at adjustment station -> APPLY_ADJUSTMENT */
    TRUING_ORCH_CONT_ADJUSTMENT,       /* CONFIRM_ADJUSTMENT_DONE -> next spoke / VERIFY */
} truing_orch_continuation_t;

typedef struct {
    truing_orch_deps_t         deps;
    truing_solver_config_t     solver;              /* working copy (SPEC §12.3.1 mutability applied here) */
    truing_row_dims_t          dims;
    truing_state_t             state;
    truing_wait_correlator_t   waits;
    truing_orch_continuation_t continuation;
    bool                       wait_answered;
    truing_intent_t            answer;              /* the accepted intent answering the active wait */
    bool                       nav_polling;         /* automated navigation in motion */
    truing_wheel_state_t       wheel_state;
    truing_session_header_t    session;
    bool                       session_active;
    uint32_t                   next_session_id;
    truing_admission_t         admission;
    truing_adjustment_plan_t   plan;
    truing_verification_result_t verification;
    float                      predicted_lateral_mm[TRUING_MAX_SPOKES];
    bool                       targets_available;
    truing_reason_t            targets_reason;
    truing_terminal_result_t   terminal;
    truing_reason_t            terminal_reason;
    truing_reason_t            init_error;
    bool                       init_validated;
    truing_reason_t            start_refusal;       /* last START_TRUING refusal (SPEC §11.3.1) */
    /* measurement bookkeeping */
    truing_nav_target_t        current_target;
    truing_state_t             measure_return_state;
    bool                       remeasure_mode;
    bool                       remeasure_spoke[TRUING_MAX_SPOKES];
    bool                       remeasure_rim[TRUING_MAX_RIM_ANGLES];
    uint8_t                    retries_used;
    uint8_t                    remeasure_attempts_used;
    bool                       runout_entry_pending;
    /* apply loop */
    uint8_t                    apply_cursor;
    bool                       adjustments_applied;
    bool                       adjustment_confirmed;
    bool                       verify_measure_started;
    /* progress (SPEC §7.5.1, §8.6.1) */
    uint8_t                    cycles_run;
    bool                       have_last_j;
    float                      last_j;
    truing_layout_id_t         last_layout;
    uint8_t                    non_improving;
    uint8_t                    layout_changes;
    bool                       abort_requested;
    uint32_t                   requested_delay_ms;
    uint32_t                   transitions;
    uint32_t                   intents_rejected;
} truing_orchestrator_t;

/* SPEC §12.2 current-state query: present state only, never history. */
typedef struct {
    truing_state_t           state;
    bool                     waiting;
    truing_wait_prompt_t     active_wait;
    uint8_t                  cycle_index;
    uint8_t                  cycles_run;
    bool                     session_active;
    truing_terminal_result_t last_result;
    truing_reason_t          last_reason;
    truing_reason_t          init_error;
    truing_reason_t          start_refusal;
} truing_orch_snapshot_t;

/* SPEC §12.2 GET_CURRENT_CYCLE_PROVENANCE / P6: the in-RAM authoritative record. */
typedef struct {
    uint32_t                     session_id;
    uint32_t                     artifact_id;
    truing_fingerprint_t         generating_fingerprint;
    uint32_t                     tension_model_profile_id;
    uint32_t                     chain_profile_id;
    uint32_t                     machine_profile_id;
    truing_layout_id_t           active_layout;
    truing_row_mask_t            active_row_set;
    truing_solver_config_t       solver_config;      /* weights and tolerances applied */
    truing_wheel_state_summary_t wheel_summary;
    truing_status_t              spoke_status[TRUING_MAX_SPOKES];
    truing_reason_t              spoke_reason[TRUING_MAX_SPOKES];
    truing_status_t              runout_status[TRUING_MAX_RIM_ANGLES];
    truing_reason_t              runout_reason[TRUING_MAX_RIM_ANGLES];
    truing_adjustment_plan_t     plan;
    truing_verification_result_t verification;
    truing_wheel_position_t      wheel_position;     /* SPEC §10A position authority snapshot */
    bool                         contains_non_real_implementations;
    /* Bounded acoustic sampling (demonstration images only; limit 0 everywhere else). Reported
     * so that a record showing three tension measurements on a 32-spoke wheel says WHY it holds
     * three, and cannot be read as a complete physical measurement of the wheel. */
    uint8_t                      tension_sample_limit;      /* 0 = every spoke was to be collected */
    uint8_t                      tension_sampled;           /* spokes with a tension record this cycle */
    bool                         tension_omitted_by_layout; /* the rest were left uncollected: not in the layout */
} truing_cycle_provenance_t;

bool truing_orch_init(truing_orchestrator_t *o, const truing_orch_deps_t *deps);
truing_orch_step_t truing_orch_step(truing_orchestrator_t *o);
/* Applies SPEC §12.3 admissibility and §7.3 correlation; records the answer. Never transitions. */
truing_intent_verdict_t truing_orch_submit_intent(truing_orchestrator_t *o, const truing_intent_t *intent,
                                                  truing_reason_t *reason_out);
/* After a terminal result: return to READY for a new session (reference still established). */
bool truing_orch_reset_to_ready(truing_orchestrator_t *o);
void truing_orch_snapshot(const truing_orchestrator_t *o, truing_orch_snapshot_t *out);
void truing_orch_provenance(truing_orchestrator_t *o, truing_cycle_provenance_t *out);

const char *truing_orch_step_str(truing_orch_step_t s);

#ifdef __cplusplus
}
#endif
#endif /* TRUING_ORCH_ORCHESTRATOR_H */
