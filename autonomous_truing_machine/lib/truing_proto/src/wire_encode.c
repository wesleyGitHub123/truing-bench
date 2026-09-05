#include "truing_proto/wire.h"

#include <string.h>

#include "truing/limits.h"
#include "truing_hal/navigation_if.h"
#include "truing_proto/json.h"

/* Decimal places per quantity, chosen from what the measurement can actually
 * resolve rather than from what a float can print: runout dials read to 0.01 mm,
 * tension to whole newtons, nipple turns to hundredths of a revolution. */
#define DP_MM      3u
#define DP_HZ      3u
#define DP_N       2u
#define DP_REV     4u
#define DP_RAD     5u
#define DP_RATIO   5u

static const char *channel_str(truing_event_channel_t c)
{
    return c == TRUING_EVT_CHANNEL_RUNOUT ? "RUNOUT" : "TENSION";
}

/* The wait prompt as SPEC §12.2 requires it: a positioning prompt names the wheel
 * feature AND the station that requires it (SPEC §10A), never one without the other. */
static void write_prompt(truing_json_writer_t *w, const char *key, const truing_wait_prompt_t *p)
{
    truing_json_obj_open(w, key);
    truing_json_u32(w, "wait_id", p->wait_id);
    truing_json_str(w, "kind", truing_wait_kind_str(p->kind));
    truing_json_str(w, "expected_intent", truing_intent_type_str(p->expected_intent));
    truing_json_str(w, "station", truing_station_str(p->station));
    truing_json_u32(w, "target_index", p->target_index);
    if (p->kind == TRUING_WAIT_POSITION_TO_RIM_ANGLE) {
        truing_json_f32(w, "target_angle_rad", p->target_angle_rad, DP_RAD);
    }
    if (p->kind == TRUING_WAIT_APPLY_ADJUSTMENT) {
        truing_json_f32(w, "display_turns_rev", p->display_turns_rev, DP_REV);
    }
    truing_json_u32(w, "timeout_ms", p->timeout_ms);
    truing_json_obj_close(w);
}

size_t truing_wire_encode_event(const truing_telemetry_event_t *ev, char *buf, size_t cap)
{
    if (ev == NULL || buf == NULL) {
        return 0u;
    }
    truing_json_writer_t w;
    truing_json_init(&w, buf, cap);
    truing_json_obj_open(&w, NULL);
    truing_json_str(&w, "t", "event");
    truing_json_str(&w, "kind", truing_event_kind_str(ev->kind));
    truing_json_u32(&w, "ts_ms", ev->timestamp_ms);
    truing_json_u32(&w, "cycle_index", ev->cycle_index);

    switch (ev->kind) {
    case TRUING_EVT_STATE_TRANSITION:
        truing_json_str(&w, "from", truing_state_str(ev->u.transition.from));
        truing_json_str(&w, "to", truing_state_str(ev->u.transition.to));
        break;
    case TRUING_EVT_WAIT_ISSUED:
        write_prompt(&w, "wait", &ev->u.wait);
        break;
    case TRUING_EVT_INTENT_REJECTED:
        truing_json_str(&w, "intent", truing_intent_type_str(ev->u.rejected.intent));
        truing_json_str(&w, "verdict", truing_intent_verdict_str(ev->u.rejected.verdict));
        truing_json_str(&w, "reason", truing_reason_str(truing_intent_verdict_reason(ev->u.rejected.verdict)));
        truing_json_u32(&w, "wait_id", ev->u.rejected.wait_id);
        break;
    case TRUING_EVT_MEASUREMENT_RESULT:
        truing_json_str(&w, "channel", channel_str(ev->u.measurement.channel));
        truing_json_u32(&w, "index", ev->u.measurement.index);
        truing_json_str(&w, "status", truing_status_str(ev->u.measurement.status));
        truing_json_str(&w, "reason", truing_reason_str(ev->u.measurement.reason));
        if (ev->u.measurement.channel == TRUING_EVT_CHANNEL_RUNOUT) {
            truing_json_f32(&w, "lateral_mm", ev->u.measurement.value_a, DP_MM);
            truing_json_f32(&w, "radial_mm", ev->u.measurement.value_b, DP_MM);
        } else {
            truing_json_f32(&w, "tension_n", ev->u.measurement.value_a, DP_N);
            truing_json_f32(&w, "selected_frequency_hz", ev->u.measurement.value_b, DP_HZ);
        }
        break;
    case TRUING_EVT_TERMINAL_RESULT:
        truing_json_str(&w, "result", truing_terminal_str(ev->u.terminal));
        break;
    case TRUING_EVT_NAVIGATION:
        truing_json_str(&w, "target_kind", truing_nav_target_kind_str((truing_nav_target_kind_t)ev->u.navigation.target_kind));
        truing_json_u32(&w, "index", ev->u.navigation.index);
        truing_json_f32(&w, "angle_rad", ev->u.navigation.angle_rad, DP_RAD);
        truing_json_str(&w, "station", truing_station_str((truing_station_id_t)ev->u.navigation.station));
        truing_json_str(&w, "outcome", truing_nav_outcome_str((truing_nav_outcome_t)ev->u.navigation.outcome));
        /* NaN here is not a gap in the frame: it is the position authority declining to
         * vouch for a rotation, and truing_json_f32 renders it as null (SPEC §10A). */
        truing_json_f32(&w, "rotation_rad", ev->u.navigation.rotation_rad, DP_RAD);
        break;
    case TRUING_EVT_LOG: {
        char text[TRUING_EVT_TEXT_MAX + 1u];
        memcpy(text, ev->u.text, TRUING_EVT_TEXT_MAX);
        text[TRUING_EVT_TEXT_MAX] = '\0';
        truing_json_str(&w, "text", text);
        break;
    }
    default:
        break;
    }
    truing_json_obj_close(&w);

    size_t len = 0u;
    return truing_json_finish(&w, &len) ? len : 0u;
}

size_t truing_wire_encode_state(const truing_orch_snapshot_t *s, char *buf, size_t cap)
{
    if (s == NULL || buf == NULL) {
        return 0u;
    }
    truing_json_writer_t w;
    truing_json_init(&w, buf, cap);
    truing_json_obj_open(&w, NULL);
    truing_json_str(&w, "t", "state");
    /* SPEC §12.2: present state only. Nothing below is history, and nothing here lets a
     * client reconstruct events it missed — that is deliberately not offered. */
    truing_json_str(&w, "current_state", truing_state_str(s->state));
    if (s->waiting) {
        write_prompt(&w, "active_wait", &s->active_wait);
    } else {
        truing_json_null(&w, "active_wait");
    }
    truing_json_u32(&w, "cycle_index", s->cycle_index);
    truing_json_u32(&w, "cycles_run", s->cycles_run);
    truing_json_bool(&w, "session_active", s->session_active);
    /* last_known_result is optional in SPEC §12.2 and is present for display continuity. */
    if (s->last_result == TRUING_TERMINAL_NONE) {
        truing_json_null(&w, "last_known_result");
    } else {
        truing_json_str(&w, "last_known_result", truing_terminal_str(s->last_result));
    }
    truing_json_str(&w, "last_reason", truing_reason_str(s->last_reason));
    truing_json_str(&w, "init_error", truing_reason_str(s->init_error));
    truing_json_str(&w, "start_refusal", truing_reason_str(s->start_refusal));
    truing_json_obj_close(&w);

    size_t len = 0u;
    return truing_json_finish(&w, &len) ? len : 0u;
}

static void write_row_mask(truing_json_writer_t *w, const char *key, const truing_row_mask_t *m)
{
    /* Little-endian byte order over the words, so bit i of row-space is byte i/8, bit i%8
     * — the order a client gets from indexing the decoded hex string. */
    uint8_t bytes[TRUING_ROW_MASK_WORDS * 4u];
    for (unsigned i = 0u; i < TRUING_ROW_MASK_WORDS; ++i) {
        bytes[4u * i + 0u] = (uint8_t)(m->words[i] & 0xFFu);
        bytes[4u * i + 1u] = (uint8_t)((m->words[i] >> 8) & 0xFFu);
        bytes[4u * i + 2u] = (uint8_t)((m->words[i] >> 16) & 0xFFu);
        bytes[4u * i + 3u] = (uint8_t)((m->words[i] >> 24) & 0xFFu);
    }
    truing_json_hex(w, key, bytes, sizeof(bytes));
}

static void write_solver_config(truing_json_writer_t *w, const truing_solver_config_t *c)
{
    /* SPEC §12.2 asks for "weights and tolerances applied". Every value the solver ran
     * with is written, not a selection, because the point of the query is to explain a
     * specific adjustment without the reader having to trust a summary. */
    truing_json_obj_open(w, "config_applied");
    truing_json_f32(w, "tol_lateral_mm", c->tol_lateral_mm, DP_MM);
    truing_json_f32(w, "tol_radial_mm", c->tol_radial_mm, DP_MM);
    truing_json_f32(w, "tol_tension_n", c->tol_tension_n, DP_N);
    truing_json_f32(w, "tol_tension_cv", c->tol_tension_cv, DP_RATIO);
    truing_json_f32(w, "tol_mean_tension_error_n", c->tol_mean_tension_error_n, DP_N);
    truing_json_f32(w, "tol_angular_rad", c->tol_angular_rad, DP_RAD);
    truing_json_f32(w, "max_condition_number", c->max_condition_number, DP_N);
    truing_json_f32(w, "rank_tolerance", c->rank_tolerance, DP_RATIO);
    truing_json_f32(w, "n_mt_displacement_tolerance", c->n_mt_displacement_tolerance, DP_RATIO);
    truing_json_f32(w, "n_mt_tension_tolerance", c->n_mt_tension_tolerance, DP_RATIO);
    truing_json_f32(w, "n_mt_uniqueness_threshold", c->n_mt_uniqueness_threshold, DP_RATIO);
    truing_json_f32(w, "max_adjustment_revolutions", c->max_adjustment_revolutions, DP_REV);
    truing_json_f32(w, "trust_radial", c->trust_radial, DP_RATIO);
    truing_json_f32(w, "trust_tension", c->trust_tension, DP_RATIO);
    truing_json_f32(w, "target_tension_n", c->target_tension_n, DP_N);
    truing_json_f32(w, "adjustment_deadband_rev", c->adjustment_deadband_rev, DP_REV);
    truing_json_f32(w, "lateral_deadband_mm", c->lateral_deadband_mm, DP_MM);
    truing_json_f32(w, "min_relative_improvement", c->min_relative_improvement, DP_RATIO);
    truing_json_u32(w, "N_lat", c->N_lat);
    truing_json_u32(w, "N_rad", c->N_rad);
    truing_json_u32(w, "n_rim_angles", c->n_rim_angles);
    truing_json_u32(w, "max_cycles", c->max_cycles);
    truing_json_u32(w, "consecutive_non_improving_cycles", c->consecutive_non_improving_cycles);
    truing_json_u32(w, "measurement_retry_count", c->measurement_retry_count);
    truing_json_u32(w, "partial_state_remeasure_attempts", c->partial_state_remeasure_attempts);
    truing_json_u32(w, "excitation_settle_ms", c->excitation_settle_ms);
    truing_json_u32(w, "tension_model_profile_id", c->tension_model_profile_id);
    truing_json_obj_close(w);
}

static void write_status_array(truing_json_writer_t *w, const char *key,
                               const truing_status_t *st, uint8_t n)
{
    truing_json_arr_open(w, key);
    for (uint8_t i = 0u; i < n; ++i) {
        truing_json_str(w, NULL, truing_status_str(st[i]));
    }
    truing_json_arr_close(w);
}

static void write_reason_array(truing_json_writer_t *w, const char *key,
                               const truing_reason_t *rs, uint8_t n)
{
    truing_json_arr_open(w, key);
    for (uint8_t i = 0u; i < n; ++i) {
        truing_json_str(w, NULL, truing_reason_str(rs[i]));
    }
    truing_json_arr_close(w);
}

size_t truing_wire_encode_provenance(const truing_cycle_provenance_t *p, char *buf, size_t cap)
{
    if (p == NULL || buf == NULL) {
        return 0u;
    }
    const uint8_t n_spokes = p->wheel_summary.n_spokes <= TRUING_MAX_SPOKES
                                 ? p->wheel_summary.n_spokes : (uint8_t)TRUING_MAX_SPOKES;
    const uint8_t n_rim = p->wheel_summary.n_rim_angles <= TRUING_MAX_RIM_ANGLES
                              ? p->wheel_summary.n_rim_angles : (uint8_t)TRUING_MAX_RIM_ANGLES;

    truing_json_writer_t w;
    truing_json_init(&w, buf, cap);
    truing_json_obj_open(&w, NULL);
    truing_json_str(&w, "t", "provenance");
    truing_json_u32(&w, "session_id", p->session_id);

    /* SPEC §11.4: the artifact and the fingerprint that generated it. */
    truing_json_u32(&w, "influence_artifact_id", p->artifact_id);
    if (p->generating_fingerprint.set) {
        truing_json_hex(&w, "generating_fingerprint", p->generating_fingerprint.bytes,
                        TRUING_FINGERPRINT_BYTES);
    } else {
        truing_json_null(&w, "generating_fingerprint");
    }
    truing_json_u32(&w, "tension_model_profile_id", p->tension_model_profile_id);
    truing_json_u32(&w, "chain_profile_id", p->chain_profile_id);
    truing_json_u32(&w, "machine_profile_id", p->machine_profile_id);

    truing_json_str(&w, "active_layout", truing_layout_str(p->active_layout));
    write_row_mask(&w, "active_row_set", &p->active_row_set);
    write_solver_config(&w, &p->solver_config);

    /* Current WheelState summary. */
    truing_json_obj_open(&w, "wheel_state_summary");
    truing_json_u32(&w, "n_spokes", p->wheel_summary.n_spokes);
    truing_json_u32(&w, "n_rim_angles", p->wheel_summary.n_rim_angles);
    truing_json_u32(&w, "spokes_solver_admissible", p->wheel_summary.spokes_solver_admissible);
    truing_json_u32(&w, "runout_solver_admissible", p->wheel_summary.runout_solver_admissible);
    truing_json_u32(&w, "spokes_verification_grade", p->wheel_summary.spokes_verification_grade);
    truing_json_obj_open(&w, "spokes_by_status");
    for (unsigned i = 0u; i < (unsigned)TRUING_STATUS__COUNT; ++i) {
        truing_json_u32(&w, truing_status_str((truing_status_t)i), p->wheel_summary.spokes_by_status[i]);
    }
    truing_json_obj_close(&w);
    truing_json_obj_open(&w, "runout_by_status");
    for (unsigned i = 0u; i < (unsigned)TRUING_STATUS__COUNT; ++i) {
        truing_json_u32(&w, truing_status_str((truing_status_t)i), p->wheel_summary.runout_by_status[i]);
    }
    truing_json_obj_close(&w);
    truing_json_obj_close(&w);

    /* Per-measurement status and reason contributing to this cycle. */
    write_status_array(&w, "spoke_status", p->spoke_status, n_spokes);
    write_reason_array(&w, "spoke_reason", p->spoke_reason, n_spokes);
    write_status_array(&w, "runout_status", p->runout_status, n_rim);
    write_reason_array(&w, "runout_reason", p->runout_reason, n_rim);

    /* The AdjustmentPlan currently on offer — the thing P6 exists to explain. */
    truing_json_obj_open(&w, "plan");
    truing_json_bool(&w, "valid", p->plan.valid);
    truing_json_u32(&w, "cycle_index", p->plan.cycle_index);
    truing_json_u32(&w, "n_spokes", p->plan.n_spokes);
    truing_json_u32(&w, "n_active_rows", p->plan.n_active_rows);
    truing_json_u32(&w, "n_valid_rows", p->plan.n_valid_rows);
    truing_json_u32(&w, "n_suspect_rows", p->plan.n_suspect_rows);
    truing_json_str(&w, "policy_reason", truing_reason_str(p->plan.policy_reason));
    truing_json_bool(&w, "mean_tension_targeting_applied", p->plan.mean_tension_targeting_applied);
    truing_json_f32(&w, "cost_j", p->plan.cost_j, DP_RATIO);
    truing_json_u32(&w, "solver_version", p->plan.solver_version);
    {
        const uint8_t n = p->plan.n_spokes <= TRUING_MAX_SPOKES ? p->plan.n_spokes : (uint8_t)TRUING_MAX_SPOKES;
        truing_json_arr_open(&w, "turns_rev");
        for (uint8_t i = 0u; i < n; ++i) {
            truing_json_f32(&w, NULL, p->plan.turns_rev[i], DP_REV);
        }
        truing_json_arr_close(&w);
        truing_json_arr_open(&w, "skipped");
        for (uint8_t i = 0u; i < n; ++i) {
            truing_json_bool(&w, NULL, p->plan.skipped[i]);
        }
        truing_json_arr_close(&w);
    }
    truing_json_obj_close(&w);

    truing_json_obj_open(&w, "verification");
    truing_json_u32(&w, "cycle_index", p->verification.cycle_index);
    truing_json_str(&w, "layout", truing_layout_str(p->verification.layout));
    truing_json_u32(&w, "n_active_rows", p->verification.n_active_rows);
    truing_json_f32(&w, "cost_j", p->verification.cost_j, DP_RATIO);
    truing_json_f32(&w, "max_lateral_mm", p->verification.max_lateral_mm, DP_MM);
    truing_json_f32(&w, "max_radial_mm", p->verification.max_radial_mm, DP_MM);
    truing_json_bool(&w, "geometric_converged", p->verification.geometric_converged);
    truing_json_bool(&w, "tension_evaluated", p->verification.tension_evaluated);
    truing_json_bool(&w, "tension_compliant", p->verification.tension_compliant);
    truing_json_f32(&w, "non_uniformity_side_a", p->verification.non_uniformity_side_a, DP_RATIO);
    truing_json_f32(&w, "non_uniformity_side_b", p->verification.non_uniformity_side_b, DP_RATIO);
    truing_json_f32(&w, "mean_error_side_a_n", p->verification.mean_error_side_a_n, DP_N);
    truing_json_f32(&w, "mean_error_side_b_n", p->verification.mean_error_side_b_n, DP_N);
    truing_json_str(&w, "reason", truing_reason_str(p->verification.reason));
    truing_json_obj_close(&w);

    /* SPEC §10A: the wheel position authority, reported as one record. */
    truing_json_obj_open(&w, "wheel_position");
    truing_json_str(&w, "status", truing_status_str(p->wheel_position.status));
    truing_json_str(&w, "reason", truing_reason_str(p->wheel_position.reason));
    truing_json_bool(&w, "reference_established", p->wheel_position.reference_established);
    truing_json_f32(&w, "rotation_rad", p->wheel_position.rotation_rad, DP_RAD);
    truing_json_bool(&w, "operator_confirmed", p->wheel_position.operator_confirmed);
    truing_json_bool(&w, "sensor_confirmed", p->wheel_position.sensor_confirmed);
    truing_json_u32(&w, "timestamp_ms", p->wheel_position.timestamp_ms);
    truing_json_obj_close(&w);

    /* SPEC §5.3: whether anything in this cycle came from a non-real implementation.
     * It travels with the provenance so a synthetic run can never be mistaken for a
     * measured one by whoever reads the record. */
    truing_json_bool(&w, "contains_non_real_implementations", p->contains_non_real_implementations);

    /* How much of the acoustic channel this cycle actually holds, and why it holds that much.
     * A reader that sees three tension records on a 32-spoke wheel must be able to tell a
     * bounded demonstration from a wheel that was measured and mostly failed. */
    truing_json_u32(&w, "tension_sample_limit", p->tension_sample_limit);
    truing_json_u32(&w, "tension_sampled", p->tension_sampled);
    truing_json_bool(&w, "tension_omitted_by_layout", p->tension_omitted_by_layout);
    truing_json_obj_close(&w);

    size_t len = 0u;
    return truing_json_finish(&w, &len) ? len : 0u;
}

size_t truing_wire_encode_ack(const truing_wire_command_t *cmd, truing_wire_error_t err,
                              truing_intent_verdict_t verdict, truing_reason_t reason,
                              char *buf, size_t cap)
{
    if (buf == NULL) {
        return 0u;
    }
    truing_json_writer_t w;
    truing_json_init(&w, buf, cap);
    truing_json_obj_open(&w, NULL);
    truing_json_str(&w, "t", "ack");
    if (cmd != NULL && cmd->has_seq) {
        truing_json_u32(&w, "seq", cmd->seq);
    } else {
        truing_json_null(&w, "seq");
    }
    truing_json_str(&w, "cmd_kind", truing_wire_cmd_kind_str(cmd != NULL ? cmd->kind : TRUING_WIRE_CMD_UNSET));
    if (cmd != NULL && cmd->kind == TRUING_WIRE_CMD_INTENT) {
        truing_json_str(&w, "intent", truing_intent_type_str(cmd->intent.type));
    }
    const bool accepted = (err == TRUING_WIRE_OK) && (verdict == TRUING_INTENT_ADMIT_ACCEPT);
    truing_json_bool(&w, "accepted", accepted);
    truing_json_str(&w, "wire", truing_wire_error_str(err));
    /* The verdict is the orchestrator's, and only means anything once the frame decoded. */
    if (err == TRUING_WIRE_OK) {
        truing_json_str(&w, "verdict", truing_intent_verdict_str(verdict));
        truing_json_str(&w, "reason", truing_reason_str(reason));
    } else {
        truing_json_null(&w, "verdict");
        truing_json_null(&w, "reason");
    }
    truing_json_obj_close(&w);

    size_t len = 0u;
    return truing_json_finish(&w, &len) ? len : 0u;
}
