/* Orchestrator: the complete Stage 1 workflow with zero hardware (SPEC §7, §12.2, §12.3,
 * §14.5) — manual navigation and manual runout answered by the auto-operator, synthetic
 * acoustic estimates, and the synthetic calculation double. */
#include <math.h>
#include <string.h>
#include <unity.h>

#include "../support/test_records.h"
#include "truing_calc/calc_if.h"
#include "truing_fixtures/fixtures.h"
#include "truing_hal/acoustic_if.h"
#include "truing_hal/clock_if.h"
#include "truing_hal/navigation_manual.h"
#include "truing_hal/runout_if.h"
#include "truing_hal/telemetry_if.h"
#include "truing_orch/auto_operator.h"
#include "truing_orch/orchestrator.h"

/* ---- a counting telemetry sink: which states were entered, how many events by kind ---- */
typedef struct {
    uint32_t state_entries[TRUING_STATE__COUNT];
    uint32_t by_kind[TRUING_EVT__COUNT];
    uint32_t waits_by_kind[TRUING_WAIT_KIND__COUNT];
    truing_terminal_result_t last_terminal;
} sink_ctx_t;

static bool sink_emit(truing_telemetry_if_t *self, const truing_telemetry_event_t *ev)
{
    sink_ctx_t *c = (sink_ctx_t *)self->ctx;
    if ((unsigned)ev->kind < TRUING_EVT__COUNT) {
        c->by_kind[ev->kind]++;
    }
    if (ev->kind == TRUING_EVT_STATE_TRANSITION && (unsigned)ev->u.transition.to < TRUING_STATE__COUNT) {
        c->state_entries[ev->u.transition.to]++;
    }
    if (ev->kind == TRUING_EVT_WAIT_ISSUED && (unsigned)ev->u.wait.kind < TRUING_WAIT_KIND__COUNT) {
        c->waits_by_kind[ev->u.wait.kind]++;
    }
    if (ev->kind == TRUING_EVT_TERMINAL_RESULT) {
        c->last_terminal = ev->u.terminal;
    }
    return true;
}

/* ---- the rig ------------------------------------------------------------------------ */
typedef struct {
    truing_wheel_class_config_t wheel;
    truing_solver_config_t solver;
    truing_chain_profile_t chain;
    truing_tension_model_profile_t tmodel;
    truing_machine_profile_t machine;
    truing_fake_clock_t fc;
    truing_clock_if_t clock;
    truing_acoustic_if_t acoustic;
    truing_acoustic_synthetic_ctx_t actx;
    truing_runout_if_t runout;
    truing_runout_manual_ctx_t rctx;
    truing_navigation_if_t nav;
    truing_navigation_manual_ctx_t nctx;
    truing_calc_if_t calc;
    truing_calc_synthetic_ctx_t cctx;
    truing_telemetry_if_t sink;
    sink_ctx_t sctx;
    truing_auto_operator_t op;
    truing_orchestrator_t orch;
} rig_t;

static rig_t g;   /* large: static */

static void rig_build(rig_t *r, const float *lateral, float mm_per_turn)
{
    memset(r, 0, sizeof(*r));
    truing_fixture_wheel_class_sym32(&r->wheel);
    truing_fixture_solver_config(&r->solver, 32u);
    truing_fixture_chain_profile_inmp441(&r->chain);
    truing_fixture_tension_model_profile_complete(&r->tmodel);
    truing_fixture_machine_profile(&r->machine);
    truing_fake_clock_init(&r->fc, &r->clock, 1000u);
    truing_acoustic_synthetic_init(&r->acoustic, &r->actx, r->clock, 32u, TRUING_TENSION_MODEL_IDEAL_STRING, 1u);
    r->actx.snr_db = 30.0f;
    for (uint8_t i = 0; i < 32u; ++i) {
        truing_acoustic_synthetic_set_spoke(&r->actx, i, 1000.0f, 480.0f);
    }
    truing_runout_manual_init(&r->runout, &r->rctx, r->clock);
    truing_navigation_manual_init(&r->nav, &r->nctx, r->clock, 32u, 32u, &r->machine);
    truing_calc_synthetic_init(&r->calc, &r->cctx, 42u);
    r->sink.impl_name = "test_sink";
    r->sink.source_impl = TRUING_SOURCE_SYNTHETIC;
    r->sink.emit = sink_emit;
    r->sink.ctx = &r->sctx;
    truing_auto_operator_init(&r->op);
    float zeros[TRUING_MAX_RIM_ANGLES];
    memset(zeros, 0, sizeof(zeros));
    truing_auto_operator_set_wheel(&r->op, 32u, lateral != NULL ? lateral : zeros, zeros, mm_per_turn);
    truing_orch_deps_t deps;
    memset(&deps, 0, sizeof(deps));
    deps.wheel = &r->wheel;
    deps.solver = &r->solver;
    deps.chain = &r->chain;
    deps.tension_model = &r->tmodel;
    deps.machine = &r->machine;
    deps.acoustic = &r->acoustic;
    deps.runout = &r->runout;
    deps.navigation = &r->nav;
    deps.calc = &r->calc;
    deps.telemetry = &r->sink;
    deps.clock = r->clock;
    deps.firmware_version = "test";
    TEST_ASSERT_TRUE(truing_orch_init(&r->orch, &deps));
}

static void bring_to_ready(rig_t *r)
{
    uint32_t steps = 0u;
    const truing_orch_step_t s = truing_orch_run_auto(&r->orch, &r->op, 50u, &steps);
    TEST_ASSERT_EQUAL_INT(TRUING_ORCH_IDLE, s);
    TEST_ASSERT_EQUAL_INT(TRUING_STATE_READY, r->orch.state);
}

static truing_intent_verdict_t start(rig_t *r, truing_reason_t *reason)
{
    truing_intent_t i;
    memset(&i, 0, sizeof(i));
    i.type = TRUING_INTENT_START_TRUING;
    return truing_orch_submit_intent(&r->orch, &i, reason);
}

static truing_orch_step_t run(rig_t *r, uint32_t max_steps)
{
    uint32_t used = 0u;
    return truing_orch_run_auto(&r->orch, &r->op, max_steps, &used);
}

/* Two spokes out of true; the synthetic calc (1 turn/mm) and wheel (1 mm/turn) correct them in one cycle. */
static void two_spoke_lateral(float *lat)
{
    memset(lat, 0, sizeof(float) * TRUING_MAX_RIM_ANGLES);
    lat[3] = 0.4f;
    lat[10] = -0.3f;
}

void setUp(void) {}
void tearDown(void) {}

/* ---- tests ------------------------------------------------------------------------------ */
static void test_initialize_establishes_reference_through_navigation(void)
{
    rig_build(&g, NULL, 1.0f);
    TEST_ASSERT_EQUAL_INT(TRUING_ORCH_ADVANCED, truing_orch_step(&g.orch));   /* BOOT -> INITIALIZE */
    TEST_ASSERT_EQUAL_INT(TRUING_ORCH_WAITING_OPERATOR, truing_orch_step(&g.orch));
    truing_orch_snapshot_t snap;
    truing_orch_snapshot(&g.orch, &snap);
    TEST_ASSERT_EQUAL_INT(TRUING_STATE_WAIT_FOR_OPERATOR, snap.state);
    TEST_ASSERT_TRUE(snap.waiting);
    TEST_ASSERT_EQUAL_INT(TRUING_WAIT_CONFIRM_SPOKE0_AT_STATION, snap.active_wait.kind);
    TEST_ASSERT_EQUAL_INT(TRUING_STATION_ACOUSTIC, snap.active_wait.station);   /* the profile's reference station */
    TEST_ASSERT_EQUAL_UINT32(1u, snap.active_wait.wait_id);
    /* START_TRUING is not admissible here (SPEC §12.3). */
    truing_reason_t reason;
    TEST_ASSERT_EQUAL_INT(TRUING_INTENT_REJECT_STATE, start(&g, &reason));
    /* A confirmation with the wrong wait_id is stale. */
    truing_intent_t bad;
    memset(&bad, 0, sizeof(bad));
    bad.type = TRUING_INTENT_CONFIRM_POSITIONED;
    bad.wait_id = 77u;
    TEST_ASSERT_EQUAL_INT(TRUING_INTENT_REJECT_STALE_INTENT, truing_orch_submit_intent(&g.orch, &bad, &reason));
    TEST_ASSERT_EQUAL_INT(TRUING_REASON_STALE_INTENT, reason);
    /* Both rejections (the premature START and the stale confirmation) were reported. */
    TEST_ASSERT_EQUAL_UINT32(2u, g.sctx.by_kind[TRUING_EVT_INTENT_REJECTED]);
    TEST_ASSERT_EQUAL_UINT32(2u, g.orch.intents_rejected);
    /* The genuine confirmation brings the machine to READY with the reference anchored. */
    truing_intent_t ok;
    TEST_ASSERT_TRUE(truing_auto_operator_answer(&g.op, &snap, &ok));
    TEST_ASSERT_EQUAL_INT(TRUING_INTENT_ADMIT_ACCEPT, truing_orch_submit_intent(&g.orch, &ok, &reason));
    TEST_ASSERT_EQUAL_INT(TRUING_ORCH_ADVANCED, truing_orch_step(&g.orch));   /* WAIT -> READY */
    TEST_ASSERT_EQUAL_INT(TRUING_ORCH_IDLE, truing_orch_step(&g.orch));
    truing_wheel_position_t pos;
    truing_navigation_query(&g.nav, &pos);
    TEST_ASSERT_TRUE(pos.reference_established);
    TEST_ASSERT_TRUE(pos.operator_confirmed);
}

static void test_start_is_refused_without_a_usable_model_or_profile(void)
{
    rig_build(&g, NULL, 1.0f);
    bring_to_ready(&g);
    truing_reason_t reason;
    /* No artifact: refused, not aborted (nothing started). */
    g.cctx.model_available = false;
    TEST_ASSERT_EQUAL_INT(TRUING_INTENT_REJECT_SESSION_ADMISSION, start(&g, &reason));
    TEST_ASSERT_EQUAL_INT(TRUING_REASON_ARTIFACT_INVALID, reason);
    TEST_ASSERT_EQUAL_INT(TRUING_STATE_READY, g.orch.state);
    g.cctx.model_available = true;
    /* Incomplete tension-model profile: CALIBRATION_MISSING (SPEC §11.3.1). */
    truing_fixture_tension_model_profile_incomplete(&g.tmodel);
    g.tmodel.profile_id = 1u;
    TEST_ASSERT_EQUAL_INT(TRUING_INTENT_REJECT_SESSION_ADMISSION, start(&g, &reason));
    TEST_ASSERT_EQUAL_INT(TRUING_REASON_CALIBRATION_MISSING, reason);
    truing_orch_snapshot_t snap;
    truing_orch_snapshot(&g.orch, &snap);
    TEST_ASSERT_EQUAL_INT(TRUING_REASON_CALIBRATION_MISSING, snap.start_refusal);
    /* Selected profile id not loaded: also refused. */
    truing_fixture_tension_model_profile_complete(&g.tmodel);
    truing_intent_t set;
    memset(&set, 0, sizeof(set));
    set.type = TRUING_INTENT_SET_PARAMETER;
    set.payload.set_parameter.id = TRUING_PARAM_TENSION_MODEL_PROFILE_ID;
    set.payload.set_parameter.value = 2.0f;
    TEST_ASSERT_EQUAL_INT(TRUING_INTENT_ADMIT_ACCEPT, truing_orch_submit_intent(&g.orch, &set, &reason));   /* session-fixed, READY */
    TEST_ASSERT_EQUAL_INT(TRUING_INTENT_REJECT_SESSION_ADMISSION, start(&g, &reason));
    set.payload.set_parameter.value = 1.0f;
    TEST_ASSERT_EQUAL_INT(TRUING_INTENT_ADMIT_ACCEPT, truing_orch_submit_intent(&g.orch, &set, &reason));
    TEST_ASSERT_EQUAL_INT(TRUING_INTENT_ADMIT_ACCEPT, start(&g, &reason));
    TEST_ASSERT_EQUAL_INT(TRUING_STATE_MEASURE_WHEEL_STATE, g.orch.state);
    TEST_ASSERT_TRUE(g.orch.session_active);
}

static void test_full_workflow_reaches_converged_geometric_only(void)
{
    float lat[TRUING_MAX_RIM_ANGLES];
    two_spoke_lateral(lat);
    rig_build(&g, lat, 1.0f);
    bring_to_ready(&g);
    truing_reason_t reason;
    TEST_ASSERT_EQUAL_INT(TRUING_INTENT_ADMIT_ACCEPT, start(&g, &reason));
    TEST_ASSERT_EQUAL_INT(TRUING_ORCH_TERMINAL, run(&g, 2000u));

    truing_orch_snapshot_t snap;
    truing_orch_snapshot(&g.orch, &snap);
    /* SPEC §8.6.3 / §4.4.1: provisional tension estimates can never support CONVERGED. */
    TEST_ASSERT_EQUAL_INT(TRUING_TERMINAL_CONVERGED_GEOMETRIC_ONLY, snap.last_result);
    TEST_ASSERT_EQUAL_INT(TRUING_REASON_TENSION_NOT_VERIFICATION_GRADE, snap.last_reason);
    TEST_ASSERT_EQUAL_UINT8(1u, snap.cycles_run);
    TEST_ASSERT_EQUAL_UINT8(2u, snap.cycle_index);   /* the verify measurement is cycle 2's state */
    TEST_ASSERT_FALSE(snap.session_active);

    /* The state machine of SPEC §7.1 was traversed. */
    TEST_ASSERT_TRUE(g.sctx.state_entries[TRUING_STATE_MEASURE_WHEEL_STATE] >= 1u);
    TEST_ASSERT_TRUE(g.sctx.state_entries[TRUING_STATE_CHECK_SOLVER_ADMISSION] >= 1u);
    TEST_ASSERT_EQUAL_UINT32(1u, g.sctx.state_entries[TRUING_STATE_COMPUTE_ADJUSTMENTS]);
    TEST_ASSERT_EQUAL_UINT32(1u, g.sctx.state_entries[TRUING_STATE_COMPUTE_TARGETS]);
    TEST_ASSERT_EQUAL_UINT32(2u, g.sctx.state_entries[TRUING_STATE_POSITION_TO_SPOKE]);
    /* APPLY_ADJUSTMENT is entered after positioning and re-entered when the operator's
     * confirmation resumes it: twice per adjusted spoke. */
    TEST_ASSERT_EQUAL_UINT32(4u, g.sctx.state_entries[TRUING_STATE_APPLY_ADJUSTMENT]);
    TEST_ASSERT_TRUE(g.sctx.state_entries[TRUING_STATE_VERIFY] >= 1u);
    TEST_ASSERT_EQUAL_UINT32(1u, g.sctx.state_entries[TRUING_STATE_EVALUATE_CONVERGENCE]);
    TEST_ASSERT_EQUAL_UINT32(0u, g.sctx.state_entries[TRUING_STATE_REMEASURE_GAPS]);
    TEST_ASSERT_EQUAL_INT(TRUING_TERMINAL_CONVERGED_GEOMETRIC_ONLY, g.sctx.last_terminal);

    /* Every positioning went through the navigation capability by prompt: 1 reference + 64 (measure)
     * + 2 (apply) + 64 (verify re-measure) = 131 confirmations; 64 runout entries; 2 adjustments. */
    TEST_ASSERT_EQUAL_UINT32(131u, g.op.positions_confirmed);
    TEST_ASSERT_EQUAL_UINT32(64u, g.op.runouts_entered);
    TEST_ASSERT_EQUAL_UINT32(2u, g.op.adjustments_applied);
    TEST_ASSERT_EQUAL_UINT32(64u, g.actx.calls);
    TEST_ASSERT_EQUAL_UINT32(2u, g.sctx.waits_by_kind[TRUING_WAIT_APPLY_ADJUSTMENT]);
    TEST_ASSERT_EQUAL_UINT32(64u, g.sctx.waits_by_kind[TRUING_WAIT_ENTER_RUNOUT]);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, -0.4f, g.op.turns_applied[3]);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.3f, g.op.turns_applied[10]);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, g.op.wheel.lateral_mm[3]);

    /* SPEC §12.2 / P6 provenance is queryable and complete. */
    truing_cycle_provenance_t prov;
    truing_orch_provenance(&g.orch, &prov);
    TEST_ASSERT_EQUAL_UINT32(1u, prov.session_id);
    TEST_ASSERT_EQUAL_UINT32(42u, prov.artifact_id);
    TEST_ASSERT_TRUE(prov.generating_fingerprint.set);
    TEST_ASSERT_EQUAL_UINT32(1u, prov.tension_model_profile_id);
    TEST_ASSERT_EQUAL_UINT32(1u, prov.chain_profile_id);
    TEST_ASSERT_EQUAL_UINT32(1u, prov.machine_profile_id);
    TEST_ASSERT_EQUAL_INT(TRUING_LAYOUT_FULL, prov.active_layout);
    TEST_ASSERT_EQUAL_UINT16(96u, truing_row_mask_popcount(&prov.active_row_set));
    TEST_ASSERT_TRUE(prov.plan.valid);
    TEST_ASSERT_EQUAL_UINT8(32u, prov.plan.n_suspect_rows);
    TEST_ASSERT_EQUAL_UINT8(64u, prov.plan.n_valid_rows);
    TEST_ASSERT_TRUE(prov.verification.geometric_converged);
    TEST_ASSERT_TRUE(prov.verification.tension_evaluated);
    TEST_ASSERT_EQUAL_UINT8(32u, prov.wheel_summary.spokes_by_status[TRUING_STATUS_SUSPECT]);
    TEST_ASSERT_EQUAL_UINT8(0u, prov.wheel_summary.spokes_verification_grade);
    TEST_ASSERT_EQUAL_INT(TRUING_STATUS_VALID, prov.wheel_position.status);
    TEST_ASSERT_TRUE(prov.wheel_position.operator_confirmed);
    TEST_ASSERT_TRUE(prov.contains_non_real_implementations);   /* synthetic acoustic + calc */
    TEST_ASSERT_EQUAL_INT(TRUING_STATUS_VALID, prov.runout_status[3]);
    TEST_ASSERT_EQUAL_INT(TRUING_REASON_PROVISIONAL_MODE_ID, prov.spoke_reason[0]);
}

/* A test-local acoustic implementation producing VALID, confirmed-fundamental estimates: what a
 * future validated layer 3 would yield. It exists only to prove the CONVERGED path is reachable. */
static void valid_acoustic_measure(truing_acoustic_if_t *self, uint8_t spoke_id, const truing_wheel_class_config_t *wheel,
                                   uint8_t cycle_index, truing_tension_estimate_t *out)
{
    (void)self;
    (void)spoke_id;
    (void)wheel;
    test_make_valid_estimate(out, cycle_index, 1000.0f, 480.0f);
}

static void valid_acoustic_cancel(truing_acoustic_if_t *self)
{
    (void)self;
}

static void test_converged_requires_verification_grade_tension(void)
{
    float lat[TRUING_MAX_RIM_ANGLES];
    two_spoke_lateral(lat);
    rig_build(&g, lat, 1.0f);
    g.acoustic.impl_name = "acoustic_valid_test_double";
    g.acoustic.measure_spoke_tension = valid_acoustic_measure;
    g.acoustic.request_cancel = valid_acoustic_cancel;
    g.acoustic.source_impl = TRUING_SOURCE_SYNTHETIC;
    bring_to_ready(&g);
    truing_reason_t reason;
    TEST_ASSERT_EQUAL_INT(TRUING_INTENT_ADMIT_ACCEPT, start(&g, &reason));
    TEST_ASSERT_EQUAL_INT(TRUING_ORCH_TERMINAL, run(&g, 2000u));
    truing_orch_snapshot_t snap;
    truing_orch_snapshot(&g.orch, &snap);
    TEST_ASSERT_EQUAL_INT(TRUING_TERMINAL_CONVERGED, snap.last_result);
    TEST_ASSERT_TRUE(truing_terminal_is_success(snap.last_result));
}

static void test_n_mt_unidentified_runs_geometry_only(void)
{
    float lat[TRUING_MAX_RIM_ANGLES];
    two_spoke_lateral(lat);
    rig_build(&g, lat, 1.0f);
    g.cctx.n_mt_identified = false;   /* SPEC §8.4 Part 1: refuse mean-tension targeting, TENSION_ABSENT */
    bring_to_ready(&g);
    truing_reason_t reason;
    TEST_ASSERT_EQUAL_INT(TRUING_INTENT_ADMIT_ACCEPT, start(&g, &reason));
    TEST_ASSERT_EQUAL_INT(TRUING_ORCH_TERMINAL, run(&g, 2000u));
    truing_orch_snapshot_t snap;
    truing_orch_snapshot(&g.orch, &snap);
    TEST_ASSERT_EQUAL_INT(TRUING_TERMINAL_CONVERGED_GEOMETRIC_ONLY, snap.last_result);
    TEST_ASSERT_EQUAL_INT(TRUING_REASON_MEAN_TENSION_MODEL_UNAVAILABLE, snap.last_reason);
    truing_cycle_provenance_t prov;
    truing_orch_provenance(&g.orch, &prov);
    TEST_ASSERT_EQUAL_INT(TRUING_LAYOUT_TENSION_ABSENT, prov.active_layout);
    TEST_ASSERT_EQUAL_UINT16(64u, truing_row_mask_popcount(&prov.active_row_set));
    TEST_ASSERT_EQUAL_INT(TRUING_REASON_MEAN_TENSION_MODEL_UNAVAILABLE, prov.plan.policy_reason);
    /* The measurements kept their true status: policy excluded them, nothing failed. */
    TEST_ASSERT_EQUAL_UINT8(32u, prov.wheel_summary.spokes_by_status[TRUING_STATUS_SUSPECT]);
    TEST_ASSERT_EQUAL_UINT8(0u, prov.plan.n_suspect_rows);
}

static void test_persistent_spoke_failure_retries_then_remeasures_then_aborts_partial(void)
{
    rig_build(&g, NULL, 1.0f);
    truing_acoustic_synthetic_set_failure(&g.actx, 5u, TRUING_STATUS_REJECTED, TRUING_REASON_LOW_SNR);
    bring_to_ready(&g);
    truing_reason_t reason;
    TEST_ASSERT_EQUAL_INT(TRUING_INTENT_ADMIT_ACCEPT, start(&g, &reason));
    uint32_t delays = 0u;
    truing_orch_step_t s = TRUING_ORCH_ADVANCED;
    for (uint32_t i = 0; i < 4000u && s != TRUING_ORCH_TERMINAL && s != TRUING_ORCH_IDLE; ++i) {
        s = truing_orch_step(&g.orch);
        if (s == TRUING_ORCH_DELAY) {
            TEST_ASSERT_EQUAL_UINT32(500u, g.orch.requested_delay_ms);   /* excitation_settle_ms (fixture) */
            delays++;
        } else if (s == TRUING_ORCH_WAITING_OPERATOR) {
            truing_orch_snapshot_t snap;
            truing_intent_t intent;
            truing_orch_snapshot(&g.orch, &snap);
            TEST_ASSERT_TRUE(truing_auto_operator_answer(&g.op, &snap, &intent));
            TEST_ASSERT_EQUAL_INT(TRUING_INTENT_ADMIT_ACCEPT, truing_orch_submit_intent(&g.orch, &intent, NULL));
        }
    }
    TEST_ASSERT_EQUAL_INT(TRUING_ORCH_TERMINAL, s);
    truing_orch_snapshot_t snap;
    truing_orch_snapshot(&g.orch, &snap);
    TEST_ASSERT_EQUAL_INT(TRUING_TERMINAL_ABORT_PARTIAL_STATE, snap.last_result);
    TEST_ASSERT_EQUAL_INT(TRUING_REASON_PARTIAL_WHEEL_STATE, snap.last_reason);
    /* Spoke 5: 1 + 2 retries per pass, over 1 initial + 2 re-measure passes = 9 excitations, each
     * retry after a settle delay; the other 31 spokes were excited once and never re-measured. */
    TEST_ASSERT_EQUAL_UINT32(31u + 9u, g.actx.calls);
    TEST_ASSERT_EQUAL_UINT32(6u, delays);
    TEST_ASSERT_EQUAL_UINT32(2u, g.sctx.state_entries[TRUING_STATE_REMEASURE_GAPS]);
    TEST_ASSERT_EQUAL_UINT32(0u, g.sctx.state_entries[TRUING_STATE_COMPUTE_ADJUSTMENTS]);
    TEST_ASSERT_EQUAL_UINT32(32u, g.op.runouts_entered);   /* runout was never re-measured */
    truing_cycle_provenance_t prov;
    truing_orch_provenance(&g.orch, &prov);
    TEST_ASSERT_EQUAL_INT(TRUING_STATUS_REJECTED, prov.spoke_status[5]);
    TEST_ASSERT_EQUAL_INT(TRUING_REASON_LOW_SNR, prov.spoke_reason[5]);
}

/* Runout provider that rejects index 2 on its first entry only. */
static uint32_t g_bad_entries;
static bool flaky_runout(void *user, uint8_t rim_index, uint8_t cycle_index, float *lat, float *rad)
{
    (void)user;
    (void)cycle_index;
    if (rim_index == 2u && g_bad_entries == 0u) {
        g_bad_entries++;
        *lat = NAN;   /* a mis-typed dial reading */
        *rad = 0.0f;
        return true;
    }
    return false;   /* fall back to the wheel model */
}

static void test_transient_runout_failure_is_closed_by_remeasure_gaps(void)
{
    float lat[TRUING_MAX_RIM_ANGLES];
    two_spoke_lateral(lat);
    rig_build(&g, lat, 1.0f);
    g_bad_entries = 0u;
    g.op.runout_fn = flaky_runout;
    bring_to_ready(&g);
    truing_reason_t reason;
    TEST_ASSERT_EQUAL_INT(TRUING_INTENT_ADMIT_ACCEPT, start(&g, &reason));
    TEST_ASSERT_EQUAL_INT(TRUING_ORCH_TERMINAL, run(&g, 3000u));
    truing_orch_snapshot_t snap;
    truing_orch_snapshot(&g.orch, &snap);
    TEST_ASSERT_EQUAL_INT(TRUING_TERMINAL_CONVERGED_GEOMETRIC_ONLY, snap.last_result);
    TEST_ASSERT_EQUAL_UINT32(1u, g.sctx.state_entries[TRUING_STATE_REMEASURE_GAPS]);
    TEST_ASSERT_EQUAL_UINT32(65u, g.op.runouts_entered);   /* 32 + 1 gap + 32 verify */
    TEST_ASSERT_EQUAL_UINT32(64u, g.actx.calls);           /* no spoke was re-excited for a runout gap */
}

static void test_unsafe_adjustment_aborts_before_any_apply(void)
{
    float lat[TRUING_MAX_RIM_ANGLES];
    two_spoke_lateral(lat);
    rig_build(&g, lat, 1.0f);
    g.cctx.use_override = true;
    g.cctx.turns_override[7] = 1.5f;   /* > max_adjustment_revolutions (fixture 1.0) */
    bring_to_ready(&g);
    truing_reason_t reason;
    TEST_ASSERT_EQUAL_INT(TRUING_INTENT_ADMIT_ACCEPT, start(&g, &reason));
    TEST_ASSERT_EQUAL_INT(TRUING_ORCH_TERMINAL, run(&g, 2000u));
    truing_orch_snapshot_t snap;
    truing_orch_snapshot(&g.orch, &snap);
    TEST_ASSERT_EQUAL_INT(TRUING_TERMINAL_ABORT_UNSAFE_ADJUSTMENT, snap.last_result);
    TEST_ASSERT_EQUAL_UINT32(0u, g.op.adjustments_applied);
    TEST_ASSERT_EQUAL_UINT32(0u, g.sctx.state_entries[TRUING_STATE_APPLY_ADJUSTMENT]);
}

static void test_no_progress_and_max_cycles_aborts(void)
{
    float lat[TRUING_MAX_RIM_ANGLES];
    two_spoke_lateral(lat);
    /* A wheel that does not respond to adjustments: cost never decreases. */
    rig_build(&g, lat, 0.0f);
    bring_to_ready(&g);
    truing_reason_t reason;
    TEST_ASSERT_EQUAL_INT(TRUING_INTENT_ADMIT_ACCEPT, start(&g, &reason));
    TEST_ASSERT_EQUAL_INT(TRUING_ORCH_TERMINAL, run(&g, 6000u));
    truing_orch_snapshot_t snap;
    truing_orch_snapshot(&g.orch, &snap);
    TEST_ASSERT_EQUAL_INT(TRUING_TERMINAL_ABORT_NO_PROGRESS, snap.last_result);
    TEST_ASSERT_EQUAL_UINT8(2u, snap.cycles_run);   /* consecutive_non_improving_cycles = 2 (fixture) */
    TEST_ASSERT_EQUAL_UINT32(2u, g.sctx.state_entries[TRUING_STATE_EVALUATE_CONVERGENCE]);

    /* A wheel that improves a little each cycle but never enough: max_cycles bounds it. */
    rig_build(&g, lat, 0.3f);
    g.solver.consecutive_non_improving_cycles = 10u;
    g.solver.max_cycles = 3u;
    truing_orch_deps_t deps = g.orch.deps;
    TEST_ASSERT_TRUE(truing_orch_init(&g.orch, &deps));
    bring_to_ready(&g);
    TEST_ASSERT_EQUAL_INT(TRUING_INTENT_ADMIT_ACCEPT, start(&g, &reason));
    TEST_ASSERT_EQUAL_INT(TRUING_ORCH_TERMINAL, run(&g, 8000u));
    truing_orch_snapshot(&g.orch, &snap);
    TEST_ASSERT_EQUAL_INT(TRUING_TERMINAL_ABORT_MAX_CYCLES, snap.last_result);
    TEST_ASSERT_EQUAL_UINT8(3u, snap.cycles_run);
}

static void test_abort_and_parameter_rules_at_a_wait(void)
{
    float lat[TRUING_MAX_RIM_ANGLES];
    two_spoke_lateral(lat);
    rig_build(&g, lat, 1.0f);
    bring_to_ready(&g);
    truing_reason_t reason;
    TEST_ASSERT_EQUAL_INT(TRUING_INTENT_ADMIT_ACCEPT, start(&g, &reason));
    /* Run to the first positioning wait of the measurement phase. */
    truing_orch_step_t s = TRUING_ORCH_ADVANCED;
    while (s == TRUING_ORCH_ADVANCED) {
        s = truing_orch_step(&g.orch);
    }
    TEST_ASSERT_EQUAL_INT(TRUING_ORCH_WAITING_OPERATOR, s);
    truing_orch_snapshot_t snap;
    truing_orch_snapshot(&g.orch, &snap);
    TEST_ASSERT_EQUAL_INT(TRUING_WAIT_POSITION_TO_SPOKE, snap.active_wait.kind);
    TEST_ASSERT_EQUAL_INT(TRUING_STATION_ACOUSTIC, snap.active_wait.station);
    TEST_ASSERT_EQUAL_UINT8(0u, snap.active_wait.target_index);

    /* Session-fixed parameter: rejected while a session is active; session-mutable: applied. */
    truing_intent_t set;
    memset(&set, 0, sizeof(set));
    set.type = TRUING_INTENT_SET_PARAMETER;
    set.payload.set_parameter.id = TRUING_PARAM_TARGET_TENSION;
    set.payload.set_parameter.value = 1200.0f;
    TEST_ASSERT_EQUAL_INT(TRUING_INTENT_REJECT_PARAMETER, truing_orch_submit_intent(&g.orch, &set, &reason));
    set.payload.set_parameter.id = TRUING_PARAM_MAX_CYCLES;
    set.payload.set_parameter.value = 7.0f;
    TEST_ASSERT_EQUAL_INT(TRUING_INTENT_ADMIT_ACCEPT, truing_orch_submit_intent(&g.orch, &set, &reason));
    TEST_ASSERT_EQUAL_UINT8(7u, g.orch.solver.max_cycles);
    set.payload.set_parameter.id = TRUING_PARAM_TOL_LATERAL;   /* artifact-bound: always refused */
    TEST_ASSERT_EQUAL_INT(TRUING_INTENT_REJECT_PARAMETER, truing_orch_submit_intent(&g.orch, &set, &reason));
    /* A value that would make the configuration invalid is refused and not applied. */
    set.payload.set_parameter.id = TRUING_PARAM_MAX_CYCLES;
    set.payload.set_parameter.value = 0.0f;
    TEST_ASSERT_EQUAL_INT(TRUING_INTENT_REJECT_PARAMETER, truing_orch_submit_intent(&g.orch, &set, &reason));
    TEST_ASSERT_EQUAL_INT(TRUING_REASON_VALUE_OUT_OF_RANGE, reason);
    TEST_ASSERT_EQUAL_UINT8(7u, g.orch.solver.max_cycles);

    /* ABORT is accepted at the wait and effected at the next step (SPEC §12.3). */
    truing_intent_t abort;
    memset(&abort, 0, sizeof(abort));
    abort.type = TRUING_INTENT_ABORT;
    TEST_ASSERT_EQUAL_INT(TRUING_INTENT_ADMIT_ACCEPT, truing_orch_submit_intent(&g.orch, &abort, &reason));
    TEST_ASSERT_EQUAL_INT(TRUING_ORCH_TERMINAL, truing_orch_step(&g.orch));
    truing_orch_snapshot(&g.orch, &snap);
    TEST_ASSERT_EQUAL_INT(TRUING_TERMINAL_ABORT_OPERATOR, snap.last_result);
    TEST_ASSERT_FALSE(snap.waiting);
    /* A late confirmation for the abandoned wait is rejected. */
    truing_intent_t late;
    memset(&late, 0, sizeof(late));
    late.type = TRUING_INTENT_CONFIRM_POSITIONED;
    late.wait_id = snap.active_wait.wait_id;
    TEST_ASSERT_NOT_EQUAL(TRUING_INTENT_ADMIT_ACCEPT, truing_orch_submit_intent(&g.orch, &late, &reason));
    /* Reset to READY and run a second session to completion. */
    TEST_ASSERT_TRUE(truing_orch_reset_to_ready(&g.orch));
    TEST_ASSERT_EQUAL_INT(TRUING_STATE_READY, g.orch.state);
    TEST_ASSERT_EQUAL_INT(TRUING_INTENT_ADMIT_ACCEPT, start(&g, &reason));
    TEST_ASSERT_EQUAL_INT(TRUING_ORCH_TERMINAL, run(&g, 2000u));
    truing_orch_snapshot(&g.orch, &snap);
    TEST_ASSERT_EQUAL_INT(TRUING_TERMINAL_CONVERGED_GEOMETRIC_ONLY, snap.last_result);
    truing_cycle_provenance_t prov;
    truing_orch_provenance(&g.orch, &prov);
    TEST_ASSERT_EQUAL_UINT32(2u, prov.session_id);
}

static void test_state_within_tolerance_skips_apply_and_does_not_remeasure(void)
{
    rig_build(&g, NULL, 1.0f);   /* a true wheel */
    bring_to_ready(&g);
    truing_reason_t reason;
    TEST_ASSERT_EQUAL_INT(TRUING_INTENT_ADMIT_ACCEPT, start(&g, &reason));
    TEST_ASSERT_EQUAL_INT(TRUING_ORCH_TERMINAL, run(&g, 2000u));
    truing_orch_snapshot_t snap;
    truing_orch_snapshot(&g.orch, &snap);
    TEST_ASSERT_EQUAL_INT(TRUING_TERMINAL_CONVERGED_GEOMETRIC_ONLY, snap.last_result);
    TEST_ASSERT_EQUAL_UINT32(0u, g.op.adjustments_applied);
    TEST_ASSERT_EQUAL_UINT32(32u, g.op.runouts_entered);   /* nothing applied -> no re-measurement */
    TEST_ASSERT_EQUAL_UINT8(1u, snap.cycle_index);
    TEST_ASSERT_EQUAL_UINT32(0u, g.sctx.state_entries[TRUING_STATE_POSITION_TO_SPOKE]);
    TEST_ASSERT_EQUAL_UINT32(1u, g.sctx.state_entries[TRUING_STATE_VERIFY]);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_initialize_establishes_reference_through_navigation);
    RUN_TEST(test_start_is_refused_without_a_usable_model_or_profile);
    RUN_TEST(test_full_workflow_reaches_converged_geometric_only);
    RUN_TEST(test_converged_requires_verification_grade_tension);
    RUN_TEST(test_n_mt_unidentified_runs_geometry_only);
    RUN_TEST(test_persistent_spoke_failure_retries_then_remeasures_then_aborts_partial);
    RUN_TEST(test_transient_runout_failure_is_closed_by_remeasure_gaps);
    RUN_TEST(test_unsafe_adjustment_aborts_before_any_apply);
    RUN_TEST(test_no_progress_and_max_cycles_aborts);
    RUN_TEST(test_abort_and_parameter_rules_at_a_wait);
    RUN_TEST(test_state_within_tolerance_skips_apply_and_does_not_remeasure);
    return UNITY_END();
}
