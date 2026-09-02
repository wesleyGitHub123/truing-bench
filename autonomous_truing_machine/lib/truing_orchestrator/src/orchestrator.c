#include "truing_orch/orchestrator.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#include "truing_hal/records.h"

static const char *const k_step_str[] = { "ADVANCED", "IDLE", "WAITING_OPERATOR", "DELAY", "POLL_NAVIGATION", "TERMINAL" };

const char *truing_orch_step_str(truing_orch_step_t s)
{
    return (unsigned)s < sizeof(k_step_str) / sizeof(k_step_str[0]) ? k_step_str[s] : "?";
}

/* ---- telemetry (best-effort; never affects control flow) ---------------------------- */
static uint32_t now_ms(const truing_orchestrator_t *o)
{
    return truing_clock_now_ms(&o->deps.clock);
}

static void emit(truing_orchestrator_t *o, truing_telemetry_event_t *ev)
{
    ev->timestamp_ms = now_ms(o);
    ev->cycle_index = o->wheel_state.cycle_index;
    (void)truing_telemetry_emit(o->deps.telemetry, ev);
}

static void set_state(truing_orchestrator_t *o, truing_state_t next)
{
    if (o->state == next) {
        return;
    }
    truing_telemetry_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = TRUING_EVT_STATE_TRANSITION;
    ev.u.transition.from = o->state;
    ev.u.transition.to = next;
    o->state = next;
    o->transitions++;
    emit(o, &ev);
}

static void emit_measurement(truing_orchestrator_t *o, truing_event_channel_t ch, uint8_t index, const truing_record_meta_t *m,
                             float a, float b)
{
    truing_telemetry_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = TRUING_EVT_MEASUREMENT_RESULT;
    ev.u.measurement.channel = ch;
    ev.u.measurement.index = index;
    ev.u.measurement.status = m->status;
    ev.u.measurement.reason = m->reason_code;
    ev.u.measurement.value_a = a;
    ev.u.measurement.value_b = b;
    emit(o, &ev);
}

static void emit_navigation(truing_orchestrator_t *o, const truing_nav_result_t *r)
{
    truing_telemetry_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = TRUING_EVT_NAVIGATION;
    ev.u.navigation.target_kind = (uint8_t)r->target.kind;
    ev.u.navigation.index = r->target.index;
    ev.u.navigation.angle_rad = r->target.rim_angle_rad;
    ev.u.navigation.station = (uint8_t)r->target.station;
    ev.u.navigation.outcome = (uint8_t)r->outcome;
    truing_wheel_position_t p;
    truing_navigation_query(o->deps.navigation, &p);
    ev.u.navigation.rotation_rad = p.status == TRUING_STATUS_VALID ? p.rotation_rad : NAN;
    emit(o, &ev);
}

static truing_orch_step_t terminal(truing_orchestrator_t *o, truing_terminal_result_t result, truing_reason_t reason)
{
    o->terminal = result;
    o->terminal_reason = reason;
    o->session_active = false;
    truing_wait_clear(&o->waits);
    o->wait_answered = false;
    o->nav_polling = false;
    truing_telemetry_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = TRUING_EVT_TERMINAL_RESULT;
    ev.u.terminal = result;
    emit(o, &ev);
    set_state(o, TRUING_STATE_TERMINAL);
    return TRUING_ORCH_TERMINAL;
}

/* ---- waits -------------------------------------------------------------------------- */
static truing_orch_step_t issue_wait(truing_orchestrator_t *o, const truing_wait_prompt_t *request,
                                     truing_orch_continuation_t cont)
{
    const uint32_t id = truing_wait_issue(&o->waits, request);
    if (id == 0u) {
        /* A prompt that cannot be issued is an internal error; do not spin. */
        return terminal(o, TRUING_TERMINAL_ABORT_OPERATOR, TRUING_REASON_VALUE_OUT_OF_RANGE);
    }
    o->continuation = cont;
    o->wait_answered = false;
    truing_telemetry_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = TRUING_EVT_WAIT_ISSUED;
    ev.u.wait = o->waits.prompt;
    emit(o, &ev);
    set_state(o, TRUING_STATE_WAIT_FOR_OPERATOR);
    return TRUING_ORCH_WAITING_OPERATOR;
}

/* ---- measurement bookkeeping ----------------------------------------------------------- */
static void store_unavailable_spoke(truing_orchestrator_t *o, uint8_t spoke, truing_reason_t reason)
{
    truing_tension_estimate_t e;
    truing_hal_fill_unavailable_estimate(&e, reason, o->wheel_state.cycle_index, now_ms(o),
                                         o->deps.acoustic != NULL ? o->deps.acoustic->source_impl : TRUING_SOURCE_SYNTHETIC);
    (void)truing_wheel_state_set_spoke(&o->wheel_state, spoke, &e, NULL);
    o->remeasure_spoke[spoke] = false;
    emit_measurement(o, TRUING_EVT_CHANNEL_TENSION, spoke, &e.meta, NAN, NAN);
}

static void store_unavailable_rim(truing_orchestrator_t *o, uint8_t rim, truing_reason_t reason)
{
    truing_runout_measurement_t m;
    truing_hal_fill_unavailable_runout(&m, reason, truing_wheel_state_rim_angle(&o->wheel_state, rim), o->wheel_state.cycle_index,
                                       now_ms(o), o->deps.runout != NULL ? o->deps.runout->source_impl : TRUING_SOURCE_SYNTHETIC);
    (void)truing_wheel_state_set_runout(&o->wheel_state, rim, &m, NULL);
    o->remeasure_rim[rim] = false;
    emit_measurement(o, TRUING_EVT_CHANNEL_RUNOUT, rim, &m.meta, NAN, NAN);
}

static bool next_measurement_target(truing_orchestrator_t *o, truing_nav_target_t *t)
{
    memset(t, 0, sizeof(*t));
    for (uint8_t i = 0; i < o->wheel_state.n_spokes; ++i) {
        const bool wanted = o->remeasure_mode ? o->remeasure_spoke[i] : !truing_wheel_state_spoke_measured(&o->wheel_state, i);
        if (wanted) {
            t->kind = TRUING_NAV_TARGET_SPOKE;
            t->index = i;
            t->station = TRUING_STATION_ACOUSTIC;
            return true;
        }
    }
    for (uint8_t k = 0; k < o->wheel_state.n_rim_angles; ++k) {
        const bool wanted = o->remeasure_mode ? o->remeasure_rim[k] : !truing_wheel_state_runout_measured(&o->wheel_state, k);
        if (wanted) {
            t->kind = TRUING_NAV_TARGET_RIM_INDEX;
            t->index = k;
            t->station = TRUING_STATION_RUNOUT;
            return true;
        }
    }
    return false;
}

/* Begins the next measurement or reports that the pass is complete. */
static truing_orch_step_t dispatch_measurement(truing_orchestrator_t *o, truing_state_t return_state, bool *complete)
{
    o->measure_return_state = return_state;
    *complete = false;
    truing_nav_target_t t;
    if (!next_measurement_target(o, &t)) {
        o->remeasure_mode = false;
        *complete = true;
        return TRUING_ORCH_ADVANCED;
    }
    o->current_target = t;
    o->retries_used = 0u;
    o->runout_entry_pending = false;
    set_state(o, TRUING_STATE_POSITION);
    return TRUING_ORCH_ADVANCED;
}

/* ---- navigation by outcome (SPEC §10A.2) ------------------------------------------- */
static truing_state_t state_after_continuation(truing_orch_continuation_t c)
{
    switch (c) {
    case TRUING_ORCH_CONT_REFERENCE:      return TRUING_STATE_READY;
    case TRUING_ORCH_CONT_POSITION_SPOKE: return TRUING_STATE_MEASURE_SPOKE_TENSION;
    case TRUING_ORCH_CONT_POSITION_RIM:   return TRUING_STATE_READ_RUNOUT;
    case TRUING_ORCH_CONT_RUNOUT_ENTRY:   return TRUING_STATE_READ_RUNOUT;
    case TRUING_ORCH_CONT_POSITION_APPLY: return TRUING_STATE_APPLY_ADJUSTMENT;
    case TRUING_ORCH_CONT_ADJUSTMENT:     return TRUING_STATE_APPLY_ADJUSTMENT;
    default:                              return TRUING_STATE_TERMINAL;
    }
}

static truing_orch_step_t navigation_failed(truing_orchestrator_t *o, const truing_nav_result_t *r, truing_orch_continuation_t cont)
{
    const truing_reason_t reason = r->reason != TRUING_REASON_NONE ? r->reason : TRUING_REASON_WHEEL_REFERENCE_LOST;
    switch (cont) {
    case TRUING_ORCH_CONT_REFERENCE:
        o->init_error = reason;
        set_state(o, TRUING_STATE_INITIALIZE);
        return TRUING_ORCH_IDLE;
    case TRUING_ORCH_CONT_POSITION_SPOKE:
        /* Collection continues (SPEC §13.1); solvability is judged later (SPEC §8.11). */
        store_unavailable_spoke(o, o->current_target.index, reason);
        set_state(o, o->measure_return_state);
        return TRUING_ORCH_ADVANCED;
    case TRUING_ORCH_CONT_POSITION_RIM:
        store_unavailable_rim(o, o->current_target.index, reason);
        set_state(o, o->measure_return_state);
        return TRUING_ORCH_ADVANCED;
    default:
        /* Cannot position a spoke for adjustment: the run cannot continue safely. Terminal
         * handling of an unrecoverable navigation fault is OPEN (SPEC §10A.10); the operator
         * must re-establish the reference and start again. */
        truing_navigation_stop(o->deps.navigation);
        return terminal(o, TRUING_TERMINAL_ABORT_OPERATOR, reason);
    }
}

static truing_orch_step_t do_position(truing_orchestrator_t *o, const truing_nav_target_t *t, truing_orch_continuation_t cont)
{
    truing_nav_result_t r;
    truing_navigation_request(o->deps.navigation, t, &r);
    emit_navigation(o, &r);
    switch (r.outcome) {
    case TRUING_NAV_DONE:
        set_state(o, state_after_continuation(cont));
        return TRUING_ORCH_ADVANCED;
    case TRUING_NAV_PENDING_OPERATOR: {
        truing_wait_prompt_t p;
        if (!truing_nav_result_to_prompt(&r, &p)) {
            return navigation_failed(o, &r, cont);
        }
        return issue_wait(o, &p, cont);
    }
    case TRUING_NAV_IN_MOTION:
        o->nav_polling = true;
        o->continuation = cont;
        return TRUING_ORCH_POLL_NAVIGATION;
    default:
        return navigation_failed(o, &r, cont);
    }
}

static truing_orch_step_t poll_navigation(truing_orchestrator_t *o)
{
    truing_nav_result_t r;
    truing_navigation_poll(o->deps.navigation, &r);
    if (r.outcome == TRUING_NAV_IN_MOTION) {
        return TRUING_ORCH_POLL_NAVIGATION;
    }
    o->nav_polling = false;
    emit_navigation(o, &r);
    if (r.outcome == TRUING_NAV_DONE) {
        set_state(o, state_after_continuation(o->continuation));
        return TRUING_ORCH_ADVANCED;
    }
    return navigation_failed(o, &r, o->continuation);
}

/* ---- admission with bounded re-measurement (SPEC §7.3.1, §8.11) ---------------------- */
typedef enum { ADMIT_OK, ADMIT_REMEASURING, ADMIT_ABORTED } admit_outcome_t;

static admit_outcome_t admit_or_remeasure(truing_orchestrator_t *o, truing_orch_step_t *step_out)
{
    truing_reason_t policy_reason = TRUING_REASON_NONE;
    truing_layout_id_t requested = TRUING_LAYOUT_FULL;
    if (o->deps.calc != NULL && o->deps.calc->select_layout != NULL) {
        requested = o->deps.calc->select_layout(o->deps.calc, o->deps.geometry_only_policy, &policy_reason);
    }
    if (requested != TRUING_LAYOUT_TENSION_ABSENT) {
        requested = TRUING_LAYOUT_FULL;
    }
    truing_admission_evaluate(&o->wheel_state, &o->dims, requested, policy_reason, &o->admission);
    if (o->admission.admissible) {
        truing_reason_t reason = TRUING_REASON_NONE;
        /* R3: conditioning against the artifact's recorded values (SPEC §8.11). */
        if (o->deps.calc == NULL || o->deps.calc->conditioning_ok == NULL ||
            !o->deps.calc->conditioning_ok(o->deps.calc, o->admission.layout, &o->solver, &reason)) {
            *step_out = terminal(o, TRUING_TERMINAL_ABORT_NO_MODEL,
                                 reason != TRUING_REASON_NONE ? reason : TRUING_REASON_ARTIFACT_INVALID);
            return ADMIT_ABORTED;
        }
        return ADMIT_OK;
    }
    if (o->remeasure_attempts_used >= o->solver.partial_state_remeasure_attempts) {
        *step_out = terminal(o, TRUING_TERMINAL_ABORT_PARTIAL_STATE, TRUING_REASON_PARTIAL_WHEEL_STATE);
        return ADMIT_ABORTED;
    }
    o->remeasure_attempts_used++;
    memset(o->remeasure_spoke, 0, sizeof(o->remeasure_spoke));
    memset(o->remeasure_rim, 0, sizeof(o->remeasure_rim));
    for (uint8_t i = 0; i < o->dims.n_spokes; ++i) {
        o->remeasure_spoke[i] = truing_admission_missing_spoke(&o->admission, &o->dims, i);
    }
    for (uint8_t k = 0; k < o->dims.n_rim_angles; ++k) {
        o->remeasure_rim[k] = truing_admission_missing_rim_index(&o->admission, &o->dims, k);
    }
    o->remeasure_mode = true;
    *step_out = TRUING_ORCH_ADVANCED;
    return ADMIT_REMEASURING;
}

/* ---- session ----------------------------------------------------------------------- */
static truing_reason_t session_admission(truing_orchestrator_t *o, uint32_t *artifact_id, truing_fingerprint_t *fp)
{
    const char *field = NULL;
    if (truing_solver_config_check(&o->solver, &field) != TRUING_CFG_OK ||
        truing_config_check_pair(o->deps.wheel, &o->solver, &field) != TRUING_CFG_OK) {
        return TRUING_REASON_VALUE_OUT_OF_RANGE;
    }
    /* SPEC §11.3.1: the selected profile must be loaded, complete and compatible. */
    uint32_t missing = 0u;
    if (o->deps.tension_model == NULL || o->deps.tension_model->profile_id != o->solver.tension_model_profile_id ||
        truing_tension_model_profile_check(o->deps.tension_model, &missing, &field) != TRUING_CFG_OK ||
        truing_tension_model_profile_compatible(o->deps.tension_model, o->deps.wheel, &field) != TRUING_CFG_OK) {
        return TRUING_REASON_CALIBRATION_MISSING;
    }
    truing_reason_t reason = TRUING_REASON_NONE;
    if (o->deps.calc == NULL || o->deps.calc->model_available == NULL ||
        !o->deps.calc->model_available(o->deps.calc, o->deps.wheel, &o->solver, &reason, artifact_id, fp)) {
        return reason != TRUING_REASON_NONE ? reason : TRUING_REASON_ARTIFACT_INVALID;
    }
    return TRUING_REASON_NONE;
}

static void begin_session(truing_orchestrator_t *o, uint32_t artifact_id, const truing_fingerprint_t *fp)
{
    const truing_impl_descriptor_t impls[] = {
        { o->deps.acoustic != NULL ? o->deps.acoustic->impl_name : "none",
          o->deps.acoustic != NULL ? o->deps.acoustic->source_impl : TRUING_SOURCE_UNSET },
        { o->deps.runout != NULL ? o->deps.runout->impl_name : "none",
          o->deps.runout != NULL ? o->deps.runout->source_impl : TRUING_SOURCE_UNSET },
        { o->deps.navigation != NULL ? o->deps.navigation->impl_name : "none",
          o->deps.navigation != NULL ? o->deps.navigation->source_impl : TRUING_SOURCE_UNSET },
        { o->deps.calc != NULL ? o->deps.calc->impl_name : "none",
          o->deps.calc != NULL ? o->deps.calc->source_impl : TRUING_SOURCE_UNSET },
    };
    truing_session_header_init(&o->session, o->next_session_id++, o->deps.firmware_version, o->deps.wheel, &o->solver,
                               o->deps.chain != NULL ? o->deps.chain->chain_id : 0u,
                               o->deps.machine != NULL ? o->deps.machine->profile_id : 0u, artifact_id, fp, impls,
                               sizeof(impls) / sizeof(impls[0]));
    o->session_active = true;
    o->terminal = TRUING_TERMINAL_NONE;
    o->terminal_reason = TRUING_REASON_NONE;
    truing_wheel_state_begin_cycle(&o->wheel_state, 1u);
    memset(&o->admission, 0, sizeof(o->admission));
    memset(&o->plan, 0, sizeof(o->plan));
    memset(&o->verification, 0, sizeof(o->verification));
    o->targets_available = false;
    o->remeasure_mode = false;
    o->remeasure_attempts_used = 0u;
    o->cycles_run = 0u;
    o->have_last_j = false;
    o->non_improving = 0u;
    o->layout_changes = 0u;
    o->adjustments_applied = false;
    o->adjustment_confirmed = false;
    o->verify_measure_started = false;
    o->abort_requested = false;
    set_state(o, TRUING_STATE_MEASURE_WHEEL_STATE);
}

/* ---- the step function ------------------------------------------------------------------ */
bool truing_orch_init(truing_orchestrator_t *o, const truing_orch_deps_t *deps)
{
    if (o == NULL || deps == NULL || deps->wheel == NULL || deps->solver == NULL || deps->machine == NULL) {
        return false;
    }
    memset(o, 0, sizeof(*o));
    o->deps = *deps;
    o->solver = *deps->solver;
    o->state = TRUING_STATE_BOOT;
    truing_wait_correlator_init(&o->waits);
    o->next_session_id = 1u;
    return true;
}

truing_orch_step_t truing_orch_step(truing_orchestrator_t *o)
{
    if (o == NULL) {
        return TRUING_ORCH_IDLE;
    }
    o->requested_delay_ms = 0u;

    /* ABORT: accepted earlier, effected here at the next safe point (SPEC §12.3). */
    if (o->abort_requested && truing_state_is_operational(o->state)) {
        o->abort_requested = false;
        truing_acoustic_request_cancel(o->deps.acoustic);
        truing_navigation_stop(o->deps.navigation);
        return terminal(o, TRUING_TERMINAL_ABORT_OPERATOR, TRUING_REASON_NONE);
    }
    if (o->nav_polling) {
        return poll_navigation(o);
    }

    switch (o->state) {
    case TRUING_STATE_BOOT:
        set_state(o, TRUING_STATE_INITIALIZE);
        return TRUING_ORCH_ADVANCED;

    case TRUING_STATE_INITIALIZE: {
        if (o->init_error != TRUING_REASON_NONE) {
            return TRUING_ORCH_IDLE;
        }
        if (!o->init_validated) {
            const char *field = NULL;
            if (truing_wheel_class_config_check(o->deps.wheel, &field) != TRUING_CFG_OK ||
                truing_solver_config_check(&o->solver, &field) != TRUING_CFG_OK ||
                truing_config_check_pair(o->deps.wheel, &o->solver, &field) != TRUING_CFG_OK ||
                (o->deps.chain != NULL && truing_chain_profile_check(o->deps.chain, &field) != TRUING_CFG_OK) ||
                truing_machine_profile_check(o->deps.machine, &field) != TRUING_CFG_OK ||
                !truing_row_dims_init(&o->dims, o->deps.wheel->n_spokes, o->solver.n_rim_angles) ||
                truing_wheel_state_init(&o->wheel_state, o->deps.wheel->n_spokes, o->solver.n_rim_angles, 0u) != TRUING_WS_OK) {
                o->init_error = TRUING_REASON_VALUE_OUT_OF_RANGE;
                return TRUING_ORCH_IDLE;
            }
            o->init_validated = true;
        }
        /* SPEC §6.5 setup verification through the navigation capability. */
        truing_nav_result_t r;
        truing_navigation_establish_reference(o->deps.navigation, &r);
        emit_navigation(o, &r);
        switch (r.outcome) {
        case TRUING_NAV_DONE:
            set_state(o, TRUING_STATE_READY);
            return TRUING_ORCH_ADVANCED;
        case TRUING_NAV_PENDING_OPERATOR: {
            truing_wait_prompt_t p;
            if (!truing_nav_result_to_prompt(&r, &p)) {
                o->init_error = TRUING_REASON_VALUE_OUT_OF_RANGE;
                return TRUING_ORCH_IDLE;
            }
            return issue_wait(o, &p, TRUING_ORCH_CONT_REFERENCE);
        }
        case TRUING_NAV_IN_MOTION:
            o->nav_polling = true;
            o->continuation = TRUING_ORCH_CONT_REFERENCE;
            return TRUING_ORCH_POLL_NAVIGATION;
        default:
            o->init_error = r.reason != TRUING_REASON_NONE ? r.reason : TRUING_REASON_WHEEL_REFERENCE_LOST;
            return TRUING_ORCH_IDLE;
        }
    }

    case TRUING_STATE_READY:
        return TRUING_ORCH_IDLE;

    case TRUING_STATE_WAIT_FOR_OPERATOR: {
        if (!o->wait_answered) {
            return TRUING_ORCH_WAITING_OPERATOR;
        }
        o->wait_answered = false;
        truing_wait_clear(&o->waits);
        const truing_orch_continuation_t cont = o->continuation;
        o->continuation = TRUING_ORCH_CONT_NONE;
        switch (cont) {
        case TRUING_ORCH_CONT_REFERENCE:
        case TRUING_ORCH_CONT_POSITION_SPOKE:
        case TRUING_ORCH_CONT_POSITION_RIM:
        case TRUING_ORCH_CONT_POSITION_APPLY: {
            truing_nav_result_t r;
            truing_navigation_confirm(o->deps.navigation, &r);
            emit_navigation(o, &r);
            if (r.outcome != TRUING_NAV_DONE) {
                return navigation_failed(o, &r, cont);
            }
            set_state(o, state_after_continuation(cont));
            return TRUING_ORCH_ADVANCED;
        }
        case TRUING_ORCH_CONT_RUNOUT_ENTRY:
            o->runout_entry_pending = true;
            set_state(o, TRUING_STATE_READ_RUNOUT);
            return TRUING_ORCH_ADVANCED;
        case TRUING_ORCH_CONT_ADJUSTMENT:
            o->adjustment_confirmed = true;
            set_state(o, TRUING_STATE_APPLY_ADJUSTMENT);
            return TRUING_ORCH_ADVANCED;
        default:
            return terminal(o, TRUING_TERMINAL_ABORT_OPERATOR, TRUING_REASON_VALUE_OUT_OF_RANGE);
        }
    }

    case TRUING_STATE_MEASURE_WHEEL_STATE: {
        bool complete = false;
        const truing_orch_step_t s = dispatch_measurement(o, TRUING_STATE_MEASURE_WHEEL_STATE, &complete);
        if (complete) {
            set_state(o, TRUING_STATE_CHECK_SOLVER_ADMISSION);
        }
        return s;
    }

    case TRUING_STATE_POSITION:
        return do_position(o, &o->current_target,
                           o->current_target.kind == TRUING_NAV_TARGET_SPOKE ? TRUING_ORCH_CONT_POSITION_SPOKE
                                                                             : TRUING_ORCH_CONT_POSITION_RIM);

    case TRUING_STATE_MEASURE_SPOKE_TENSION: {
        /* ONE coarse call (SPEC §9.1); external retry placement re-invokes it after a settle (SPEC §7.4). */
        const uint8_t spoke = o->current_target.index;
        truing_tension_estimate_t e;
        truing_acoustic_measure(o->deps.acoustic, spoke, o->deps.wheel, o->wheel_state.cycle_index, &e);
        emit_measurement(o, TRUING_EVT_CHANNEL_TENSION, spoke, &e.meta, e.tension_n, e.frequency.selected_frequency_hz);
        if (!truing_status_is_solver_admissible(e.meta.status) && e.meta.reason_code != TRUING_REASON_CANCELLED &&
            o->retries_used < o->solver.measurement_retry_count) {
            o->retries_used++;
            o->requested_delay_ms = o->solver.excitation_settle_ms;
            return TRUING_ORCH_DELAY;   /* state unchanged: the next step re-excites */
        }
        if (truing_wheel_state_set_spoke(&o->wheel_state, spoke, &e, NULL) != TRUING_WS_OK) {
            store_unavailable_spoke(o, spoke, TRUING_REASON_VALUE_OUT_OF_RANGE);
        } else {
            o->remeasure_spoke[spoke] = false;
        }
        set_state(o, o->measure_return_state);
        return TRUING_ORCH_ADVANCED;
    }

    case TRUING_STATE_READ_RUNOUT: {
        const uint8_t rim = o->current_target.index;
        const float angle = truing_wheel_state_rim_angle(&o->wheel_state, rim);
        const bool manual = (truing_runout_capabilities(o->deps.runout) & TRUING_RUNOUT_CAP_MANUAL_ENTRY) != 0u;
        if (manual && !o->runout_entry_pending) {
            truing_wait_prompt_t p;
            memset(&p, 0, sizeof(p));
            p.kind = TRUING_WAIT_ENTER_RUNOUT;
            p.station = TRUING_STATION_RUNOUT;
            p.target_index = rim;
            p.target_angle_rad = angle;
            return issue_wait(o, &p, TRUING_ORCH_CONT_RUNOUT_ENTRY);
        }
        o->runout_entry_pending = false;
        truing_runout_measurement_t m;
        truing_runout_read_snapshot(o->deps.runout, rim, angle, o->wheel_state.cycle_index, &m);
        emit_measurement(o, TRUING_EVT_CHANNEL_RUNOUT, rim, &m.meta, m.lateral_mm, m.radial_mm);
        if (truing_wheel_state_set_runout(&o->wheel_state, rim, &m, NULL) != TRUING_WS_OK) {
            store_unavailable_rim(o, rim, TRUING_REASON_VALUE_OUT_OF_RANGE);
        } else {
            o->remeasure_rim[rim] = false;
        }
        set_state(o, o->measure_return_state);
        return TRUING_ORCH_ADVANCED;
    }

    case TRUING_STATE_CHECK_SOLVER_ADMISSION: {
        truing_orch_step_t s = TRUING_ORCH_ADVANCED;
        switch (admit_or_remeasure(o, &s)) {
        case ADMIT_OK:
            set_state(o, TRUING_STATE_COMPUTE_ADJUSTMENTS);
            return TRUING_ORCH_ADVANCED;
        case ADMIT_REMEASURING:
            set_state(o, TRUING_STATE_REMEASURE_GAPS);
            return TRUING_ORCH_ADVANCED;
        default:
            return s;
        }
    }

    case TRUING_STATE_REMEASURE_GAPS:
        /* SPEC §7.3.1: re-measure only the rows the active layout lacks, then re-run admission. */
        set_state(o, TRUING_STATE_MEASURE_WHEEL_STATE);
        return TRUING_ORCH_ADVANCED;

    case TRUING_STATE_COMPUTE_ADJUSTMENTS: {
        truing_reason_t reason = TRUING_REASON_NONE;
        if (o->deps.calc == NULL || o->deps.calc->solve == NULL ||
            !o->deps.calc->solve(o->deps.calc, &o->wheel_state, &o->admission, &o->solver, &o->plan, &reason) || !o->plan.valid) {
            return terminal(o, TRUING_TERMINAL_ABORT_NO_MODEL, reason != TRUING_REASON_NONE ? reason : TRUING_REASON_ARTIFACT_INVALID);
        }
        o->plan.cycle_index = o->wheel_state.cycle_index;
        uint8_t bad = 0u;
        if (truing_plan_is_unsafe(&o->plan, &o->solver, &bad)) {
            return terminal(o, TRUING_TERMINAL_ABORT_UNSAFE_ADJUSTMENT, TRUING_REASON_VALUE_OUT_OF_RANGE);
        }
        (void)truing_plan_apply_deadband(&o->plan, &o->solver, &o->wheel_state);
        if (!o->have_last_j) {
            /* Progress baseline: the cost of the state this plan corrects (SPEC §7.5.1). */
            o->last_j = o->plan.cost_j;
            o->last_layout = o->plan.active_layout;
            o->have_last_j = true;
        }
        set_state(o, TRUING_STATE_COMPUTE_TARGETS);
        return TRUING_ORCH_ADVANCED;
    }

    case TRUING_STATE_COMPUTE_TARGETS: {
        o->targets_reason = TRUING_REASON_NOT_IMPLEMENTED;
        o->targets_available = o->deps.calc != NULL && o->deps.calc->predict_targets != NULL &&
                               o->deps.calc->predict_targets(o->deps.calc, &o->wheel_state, &o->plan, o->predicted_lateral_mm,
                                                             &o->targets_reason);
        o->adjustments_applied = false;
        o->adjustment_confirmed = false;
        o->apply_cursor = 0u;
        while (o->apply_cursor < o->plan.n_spokes && (o->plan.skipped[o->apply_cursor] || o->plan.turns_rev[o->apply_cursor] == 0.0f)) {
            o->apply_cursor++;
        }
        set_state(o, o->apply_cursor < o->plan.n_spokes ? TRUING_STATE_POSITION_TO_SPOKE : TRUING_STATE_VERIFY);
        return TRUING_ORCH_ADVANCED;
    }

    case TRUING_STATE_POSITION_TO_SPOKE: {
        truing_nav_target_t t;
        memset(&t, 0, sizeof(t));
        t.kind = TRUING_NAV_TARGET_SPOKE;
        t.index = o->apply_cursor;
        t.station = TRUING_STATION_ADJUSTMENT;
        o->current_target = t;
        return do_position(o, &t, TRUING_ORCH_CONT_POSITION_APPLY);
    }

    case TRUING_STATE_APPLY_ADJUSTMENT: {
        if (!o->adjustment_confirmed) {
            /* Stage 1: display the turns and wait for the operator (SPEC §7.1, §7.3). */
            truing_wait_prompt_t p;
            memset(&p, 0, sizeof(p));
            p.kind = TRUING_WAIT_APPLY_ADJUSTMENT;
            p.station = TRUING_STATION_ADJUSTMENT;
            p.target_index = o->apply_cursor;
            p.display_turns_rev = o->plan.turns_rev[o->apply_cursor];
            return issue_wait(o, &p, TRUING_ORCH_CONT_ADJUSTMENT);
        }
        o->adjustment_confirmed = false;
        o->adjustments_applied = true;
        do {
            o->apply_cursor++;
        } while (o->apply_cursor < o->plan.n_spokes && (o->plan.skipped[o->apply_cursor] || o->plan.turns_rev[o->apply_cursor] == 0.0f));
        set_state(o, o->apply_cursor < o->plan.n_spokes ? TRUING_STATE_POSITION_TO_SPOKE : TRUING_STATE_VERIFY);
        return TRUING_ORCH_ADVANCED;
    }

    case TRUING_STATE_VERIFY: {
        /* Re-measure through the SAME interfaces the measurement phase uses (SPEC §5.1). The
         * post-adjustment measurement is stamped as the next cycle's state; the next
         * MEASURE_WHEEL_STATE then finds it complete and passes through, so nothing is
         * measured twice and no record is re-stamped. */
        if (o->adjustments_applied && !o->verify_measure_started) {
            o->verify_measure_started = true;
            truing_wheel_state_begin_cycle(&o->wheel_state, (uint8_t)(o->wheel_state.cycle_index + 1u));
            o->remeasure_attempts_used = 0u;
        }
        bool complete = false;
        const truing_orch_step_t s = dispatch_measurement(o, TRUING_STATE_VERIFY, &complete);
        if (!complete) {
            return s;
        }
        truing_orch_step_t step = TRUING_ORCH_ADVANCED;
        switch (admit_or_remeasure(o, &step)) {
        case ADMIT_OK:
            break;
        case ADMIT_REMEASURING:
            return TRUING_ORCH_ADVANCED;   /* stays in VERIFY; the next step picks the gap targets */
        default:
            return step;
        }
        truing_reason_t reason = TRUING_REASON_NONE;
        if (o->deps.calc == NULL || o->deps.calc->verify == NULL ||
            !o->deps.calc->verify(o->deps.calc, &o->wheel_state, &o->admission, &o->solver, &o->verification, &reason)) {
            return terminal(o, TRUING_TERMINAL_ABORT_NO_MODEL, reason != TRUING_REASON_NONE ? reason : TRUING_REASON_ARTIFACT_INVALID);
        }
        set_state(o, TRUING_STATE_EVALUATE_CONVERGENCE);
        return TRUING_ORCH_ADVANCED;
    }

    case TRUING_STATE_EVALUATE_CONVERGENCE: {
        o->verify_measure_started = false;
        o->adjustments_applied = false;
        o->cycles_run++;   /* an outer cycle ends here whatever the verdict */
        const truing_verification_result_t *v = &o->verification;
        truing_wheel_state_summary_t sum;
        truing_wheel_state_summarize(&o->wheel_state, &sum);
        /* SPEC §8.6.3: compliance needs VERIFICATION-GRADE tension evidence — every contributing
         * tension measurement valid. Solver-admissible is not enough. */
        const bool grade_ok = v->tension_evaluated && sum.spokes_verification_grade == o->wheel_state.n_spokes;
        const bool fully = v->geometric_converged && grade_ok && v->tension_compliant;
        if (fully) {
            return terminal(o, TRUING_TERMINAL_CONVERGED, TRUING_REASON_NONE);
        }
        if (v->geometric_converged && !grade_ok) {
            truing_reason_t why;
            if (!v->tension_evaluated) {
                why = o->admission.policy_reason;
                if (why == TRUING_REASON_NONE) {
                    why = TRUING_REASON_NOT_IMPLEMENTED;
                    for (uint8_t i = 0; i < o->wheel_state.n_spokes; ++i) {
                        const truing_status_t st = o->wheel_state.spokes[i].tension.meta.status;
                        if (st != TRUING_STATUS_UNSET && !truing_status_is_solver_admissible(st)) {
                            why = o->wheel_state.spokes[i].tension.meta.reason_code;
                            break;
                        }
                    }
                }
            } else {
                why = TRUING_REASON_TENSION_NOT_VERIFICATION_GRADE;
            }
            return terminal(o, TRUING_TERMINAL_CONVERGED_GEOMETRIC_ONLY, why);
        }
        /* Not converged (or converged geometrically with verification-grade tension still out of
         * spec): stall detection within one layout (SPEC §7.5.1, §8.6.1). */
        if (o->have_last_j && v->layout == o->last_layout) {
            const bool improved = v->cost_j < o->last_j * (1.0f - o->solver.min_relative_improvement);
            o->non_improving = improved ? 0u : (uint8_t)(o->non_improving + 1u);
            if (o->non_improving >= o->solver.consecutive_non_improving_cycles) {
                return terminal(o, TRUING_TERMINAL_ABORT_NO_PROGRESS, TRUING_REASON_NONE);
            }
        } else if (o->have_last_j) {
            o->layout_changes++;   /* objective changed: baseline resets, improved(k) undefined */
            o->non_improving = 0u;
        }
        o->last_j = v->cost_j;
        o->last_layout = v->layout;
        o->have_last_j = true;
        if (o->cycles_run >= o->solver.max_cycles) {
            return terminal(o, TRUING_TERMINAL_ABORT_MAX_CYCLES, TRUING_REASON_NONE);
        }
        set_state(o, TRUING_STATE_MEASURE_WHEEL_STATE);
        return TRUING_ORCH_ADVANCED;
    }

    case TRUING_STATE_TERMINAL:
        return TRUING_ORCH_TERMINAL;

    default:
        return terminal(o, TRUING_TERMINAL_ABORT_OPERATOR, TRUING_REASON_VALUE_OUT_OF_RANGE);
    }
}

/* ---- intents ---------------------------------------------------------------------------- */
truing_intent_verdict_t truing_orch_submit_intent(truing_orchestrator_t *o, const truing_intent_t *intent,
                                                  truing_reason_t *reason_out)
{
    if (reason_out != NULL) {
        *reason_out = TRUING_REASON_NONE;
    }
    if (o == NULL || intent == NULL) {
        return TRUING_INTENT_REJECT_UNKNOWN;
    }
    truing_intent_verdict_t v = truing_intent_admissible(o->state, &o->waits, intent, o->deps.debug_channel_enabled);
    if (v == TRUING_INTENT_ADMIT_ACCEPT) {
        switch (intent->type) {
        case TRUING_INTENT_START_TRUING: {
            uint32_t artifact_id = 0u;
            truing_fingerprint_t fp;
            memset(&fp, 0, sizeof(fp));
            const truing_reason_t why = session_admission(o, &artifact_id, &fp);
            if (why != TRUING_REASON_NONE) {
                o->start_refusal = why;
                v = TRUING_INTENT_REJECT_SESSION_ADMISSION;
                if (reason_out != NULL) {
                    *reason_out = why;
                }
                break;
            }
            o->start_refusal = TRUING_REASON_NONE;
            begin_session(o, artifact_id, &fp);
            break;
        }
        case TRUING_INTENT_SET_PARAMETER: {
            truing_solver_config_t copy = o->solver;
            const char *field = NULL;
            if (!truing_param_apply(&copy, intent->payload.set_parameter.id, intent->payload.set_parameter.value) ||
                truing_solver_config_check(&copy, &field) != TRUING_CFG_OK ||
                truing_config_check_pair(o->deps.wheel, &copy, &field) != TRUING_CFG_OK) {
                v = TRUING_INTENT_REJECT_PARAMETER;
                if (reason_out != NULL) {
                    *reason_out = TRUING_REASON_VALUE_OUT_OF_RANGE;
                }
                break;
            }
            o->solver = copy;
            break;
        }
        case TRUING_INTENT_CONFIRM_POSITIONED:
        case TRUING_INTENT_CONFIRM_ADJUSTMENT_DONE:
            o->answer = *intent;
            o->wait_answered = true;
            break;
        case TRUING_INTENT_SUBMIT_RUNOUT:
            o->answer = *intent;
            /* Hand the entry to the manual runout implementation; read_snapshot() happens in step(). */
            (void)truing_runout_manual_submit(o->deps.runout, o->waits.prompt.target_index, intent->payload.runout.lateral_mm,
                                              intent->payload.runout.radial_mm);
            o->wait_answered = true;
            break;
        case TRUING_INTENT_ABORT:
            o->abort_requested = true;
            break;
        default:
            break;
        }
    }
    if (v != TRUING_INTENT_ADMIT_ACCEPT) {
        o->intents_rejected++;
        if (reason_out != NULL && *reason_out == TRUING_REASON_NONE) {
            *reason_out = truing_intent_verdict_reason(v);
        }
        truing_telemetry_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.kind = TRUING_EVT_INTENT_REJECTED;
        ev.u.rejected.intent = intent->type;
        ev.u.rejected.verdict = v;
        ev.u.rejected.wait_id = intent->wait_id;
        emit(o, &ev);
    }
    return v;
}

bool truing_orch_reset_to_ready(truing_orchestrator_t *o)
{
    if (o == NULL || o->state != TRUING_STATE_TERMINAL || !o->init_validated) {
        return false;
    }
    truing_wheel_position_t p;
    truing_navigation_query(o->deps.navigation, &p);
    if (!p.reference_established) {
        /* Reference lost (e.g. after an aborted move): go back through INITIALIZE. */
        o->init_error = TRUING_REASON_NONE;
        set_state(o, TRUING_STATE_INITIALIZE);
        return true;
    }
    set_state(o, TRUING_STATE_READY);
    return true;
}

/* ---- queries ----------------------------------------------------------------------------- */
void truing_orch_snapshot(const truing_orchestrator_t *o, truing_orch_snapshot_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (o == NULL) {
        return;
    }
    out->state = o->state;
    out->waiting = o->state == TRUING_STATE_WAIT_FOR_OPERATOR && o->waits.active;
    if (out->waiting) {
        out->active_wait = o->waits.prompt;
    }
    out->cycle_index = o->wheel_state.cycle_index;
    out->cycles_run = o->cycles_run;
    out->session_active = o->session_active;
    out->last_result = o->terminal;
    out->last_reason = o->terminal_reason;
    out->init_error = o->init_error;
    out->start_refusal = o->start_refusal;
}

void truing_orch_provenance(truing_orchestrator_t *o, truing_cycle_provenance_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (o == NULL) {
        return;
    }
    out->session_id = o->session.session_id;
    out->artifact_id = o->session.influence_matrix_artifact_id;
    out->generating_fingerprint = o->session.influence_matrix_fingerprint;
    out->tension_model_profile_id = o->session.tension_model_profile_id;
    out->chain_profile_id = o->session.chain_profile_id;
    out->machine_profile_id = o->session.machine_profile_id;
    out->active_layout = o->admission.layout;
    out->active_row_set = o->admission.active_row_set;
    out->solver_config = o->solver;
    truing_wheel_state_summarize(&o->wheel_state, &out->wheel_summary);
    for (uint8_t i = 0; i < o->wheel_state.n_spokes && i < TRUING_MAX_SPOKES; ++i) {
        out->spoke_status[i] = o->wheel_state.spokes[i].tension.meta.status;
        out->spoke_reason[i] = o->wheel_state.spokes[i].tension.meta.reason_code;
    }
    for (uint8_t k = 0; k < o->wheel_state.n_rim_angles && k < TRUING_MAX_RIM_ANGLES; ++k) {
        out->runout_status[k] = o->wheel_state.runout[k].meta.status;
        out->runout_reason[k] = o->wheel_state.runout[k].meta.reason_code;
    }
    out->plan = o->plan;
    out->verification = o->verification;
    truing_navigation_query(o->deps.navigation, &out->wheel_position);
    out->contains_non_real_implementations = o->session.contains_non_real_implementations;
}
