#include "orch_demo.h"

#include <inttypes.h>
#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "artifact_store.h"
#include "firmware_version.h"
#include "net_transport.h"
#include "truing_proto/session.h"
#include "truing_calc/calc_if.h"
#include "truing_fixtures/fixtures.h"
#include "esp_heap_caps.h"
#include "truing_hal/acoustic_if.h"
#include "truing_hal/acoustic_real.h"
#include "truing_hal/audio_source_if.h"
#include "truing_hal/pluck_if.h"
#include "truing_hal/navigation_manual.h"
#include "truing_hal/navigation_synthetic.h"
#include "truing_hal/runout_if.h"
#include "truing_hal/telemetry_if.h"
#include "truing_orch/auto_operator.h"
#include "truing_orch/orchestrator.h"

static const char *TAG = "orch_demo";

/* Which image this is: see build_mode.h. TRUING_SELF_PLAY says who answers the waits;
 * TRUING_FAST_DEMO says which implementations are wired in before the session starts.
 *
 * Kept as compile-time constants rather than #ifdef around the branches so every path is
 * type-checked in every build; the dead one folds away. */
#include "build_mode.h"

/* Everything static: the wheel state alone is ~7 KB and the rig must outlive app_main. */
static struct {
    truing_wheel_class_config_t wheel;
    truing_solver_config_t solver;
    truing_chain_profile_t chain;
    truing_tension_model_profile_t tmodel;
    truing_machine_profile_t machine;
    truing_clock_if_t clock;
    truing_acoustic_if_t acoustic;
    truing_acoustic_real_ctx_t actx;      /* REAL layers 2-4 (Phase 1f) on a SYNTHETIC front end */
    truing_audio_source_if_t audio;
    truing_audio_synthetic_ctx_t audio_ctx;
    truing_pluck_if_t pluck;
    truing_pluck_fake_ctx_t pluck_ctx;
    void *acoustic_scratch;
    truing_runout_if_t runout;
    truing_runout_manual_ctx_t rctx;
    truing_runout_synthetic_ctx_t rsyn;   /* FAST DEMO: the simulated wheel's rim, read as a snapshot */
    truing_navigation_if_t nav;
    truing_navigation_manual_ctx_t nctx;
    truing_navigation_synthetic_ctx_t nsyn;   /* FAST DEMO: positions itself instead of asking */
    /* FAST DEMO: the adjustment the operator is currently being asked to apply. The
     * simulated wheel reacts when they confirm it, which is what a real wheel would do. */
    bool     adj_pending;
    uint32_t adj_wait_id;
    uint8_t  adj_spoke;
    float    adj_turns_rev;
    truing_calc_if_t calc;
    truing_calc_artifact_ctx_t cctx;      /* REAL calculation on the loaded artifact (Phase 1c) */
    truing_telemetry_if_t sink;
    truing_telemetry_ring_ctx_t ring;
    truing_auto_operator_t op;
    truing_orchestrator_t orch;
    uint32_t dropped_events;
    /* SPEC §12: the wire session runs on THIS task, which is the orchestrator's own —
     * see truing_proto/session.h for why that placement is the whole design. */
    truing_wire_session_t session;
    bool wire_up;
} s;

/* The simulated wheel answers an adjustment through the SAME influence model the solver uses. */
static void model_response(void *user, truing_auto_wheel_model_t *wheel, uint8_t spoke, float turns_rev)
{
    const truing_artifact_t *art = (const truing_artifact_t *)user;
    for (uint8_t k = 0; k < art->n_rim_angles; ++k) {
        wheel->lateral_mm[k] += art->phi_u[k][spoke] * turns_rev;
        wheel->radial_mm[k] += art->phi_v[k][spoke] * turns_rev;
    }
}

/* FAST DEMO: the same physics, applied to the synthetic runout table instead of the
 * auto-operator's model, because in this build the table IS the simulated wheel's rim -
 * it is what truing_runout_read_snapshot() answers from. Nothing here writes wheel state:
 * the orchestrator still has to go and measure the rim to find out what changed. */
static void fastdemo_apply_to_wheel(const truing_artifact_t *art, uint8_t spoke, float turns_rev)
{
    for (uint8_t k = 0; k < art->n_rim_angles && k < TRUING_MAX_RIM_ANGLES; ++k) {
        truing_runout_synthetic_set_index(&s.rsyn, k, s.rsyn.lateral_mm[k] + art->phi_u[k][spoke] * turns_rev,
                                          s.rsyn.radial_mm[k] + art->phi_v[k][spoke] * turns_rev);
    }
}

/* FAST DEMO: watch the adjustment the operator is being asked to apply, and let the
 * simulated wheel react once they confirm it. The wait disappearing is only a
 * confirmation if the session is still running - an abort clears waits too, and a wheel
 * that "moved" because the run was cancelled would be a lie in the next cycle's runout. */
static void fastdemo_track_adjustment(const truing_orch_snapshot_t *snap, const truing_artifact_t *art)
{
    if (snap->waiting && snap->active_wait.kind == TRUING_WAIT_APPLY_ADJUSTMENT) {
        s.adj_pending = true;
        s.adj_wait_id = snap->active_wait.wait_id;
        s.adj_spoke = snap->active_wait.target_index;
        s.adj_turns_rev = snap->active_wait.display_turns_rev;
        return;
    }
    if (!s.adj_pending) {
        return;
    }
    const bool still_that_wait = snap->waiting && snap->active_wait.wait_id == s.adj_wait_id;
    if (still_that_wait) {
        return;
    }
    s.adj_pending = false;
    if (!snap->session_active) {
        ESP_LOGI(TAG, "fast demo: adjustment on spoke %u was not applied (session ended)", (unsigned)s.adj_spoke);
        return;
    }
    fastdemo_apply_to_wheel(art, s.adj_spoke, s.adj_turns_rev);
    ESP_LOGI(TAG, "fast demo: simulated wheel responded to %.3f rev on spoke %u", (double)s.adj_turns_rev,
             (unsigned)s.adj_spoke);
}

static uint32_t boot_clock_now(void *ctx)
{
    (void)ctx;
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* Drain the best-effort ring (SPEC §12.2) into the log; dropped events are counted, never chased. */
/* Collects whatever the web UI has sent and answers it. Runs here rather than on the
 * server task because handle() reaches into the orchestrator (SPEC §12.4: the transport
 * is a driver; the state machine has one owner). */
static void service_wire(void)
{
    if (!s.wire_up) {
        return;
    }
    static char frame[TRUING_WIRE_COMMAND_MAX + 1u];   /* static: not a task-stack budget */
    size_t len;
    while ((len = truing_net_poll_inbound(frame, sizeof(frame))) > 0u) {
        (void)truing_wire_session_handle(&s.session, frame, len);
    }
}

static void drain_telemetry(void)
{
    truing_telemetry_event_t ev;
    while (truing_telemetry_ring_pop(&s.ring, &ev)) {
        /* A ring has ONE consumer, so the event is forwarded here rather than by
         * truing_wire_session_pump(): the console log below wants it too. */
        if (s.wire_up) {
            (void)truing_wire_session_send_event(&s.session, &ev);
        }
        switch (ev.kind) {
        case TRUING_EVT_STATE_TRANSITION:
            ESP_LOGI(TAG, "[%6" PRIu32 " ms] c%u %s -> %s", ev.timestamp_ms, (unsigned)ev.cycle_index,
                     truing_state_str(ev.u.transition.from), truing_state_str(ev.u.transition.to));
            break;
        case TRUING_EVT_WAIT_ISSUED:
            ESP_LOGI(TAG, "[%6" PRIu32 " ms] wait #%" PRIu32 " %s index=%u station=%s turns=%.3f", ev.timestamp_ms,
                     ev.u.wait.wait_id, truing_wait_kind_str(ev.u.wait.kind), (unsigned)ev.u.wait.target_index,
                     truing_station_str(ev.u.wait.station), (double)ev.u.wait.display_turns_rev);
            break;
        case TRUING_EVT_MEASUREMENT_RESULT:
            if (ev.u.measurement.channel == TRUING_EVT_CHANNEL_TENSION) {
                ESP_LOGI(TAG, "[%6" PRIu32 " ms] tension spoke %2u %s/%s T=%.0f N f=%.0f Hz", ev.timestamp_ms,
                         (unsigned)ev.u.measurement.index, truing_status_str(ev.u.measurement.status),
                         truing_reason_str(ev.u.measurement.reason), (double)ev.u.measurement.value_a,
                         (double)ev.u.measurement.value_b);
            } else {
                ESP_LOGI(TAG, "[%6" PRIu32 " ms] runout  index %2u %s lat=%.3f rad=%.3f mm", ev.timestamp_ms,
                         (unsigned)ev.u.measurement.index, truing_status_str(ev.u.measurement.status),
                         (double)ev.u.measurement.value_a, (double)ev.u.measurement.value_b);
            }
            break;
        case TRUING_EVT_NAVIGATION:
            ESP_LOGD(TAG, "[%6" PRIu32 " ms] nav %s %u @%s -> %s R=%.3f", ev.timestamp_ms,
                     truing_nav_target_kind_str((truing_nav_target_kind_t)ev.u.navigation.target_kind),
                     (unsigned)ev.u.navigation.index, truing_station_str((truing_station_id_t)ev.u.navigation.station),
                     truing_nav_outcome_str((truing_nav_outcome_t)ev.u.navigation.outcome), (double)ev.u.navigation.rotation_rad);
            break;
        case TRUING_EVT_INTENT_REJECTED:
            ESP_LOGW(TAG, "[%6" PRIu32 " ms] intent %s rejected: %s (wait_id %" PRIu32 ")", ev.timestamp_ms,
                     truing_intent_type_str(ev.u.rejected.intent), truing_intent_verdict_str(ev.u.rejected.verdict),
                     ev.u.rejected.wait_id);
            break;
        case TRUING_EVT_TERMINAL_RESULT:
            ESP_LOGI(TAG, "[%6" PRIu32 " ms] TERMINAL %s", ev.timestamp_ms, truing_terminal_str(ev.u.terminal));
            break;
        default:
            break;
        }
    }
}

static void demo_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "== Stage 1 workflow, %s (%s; "
                  "REAL acoustic layers 2-4 on a SYNTHETIC 460 Hz pluck source; REAL truing calculation on the golden "
                  "fixture artifact; the simulated wheel responds through the same influence model) ==",
             TRUING_FAST_DEMO ? "FAST DEMO: a person starts the session and applies the adjustments; acquisition is "
                                "automatic and SYNTHETIC - this is not a physical wheel result"
                              : (TRUING_SELF_PLAY ? "SELF-PLAY: the auto-operator starts the session and answers its own waits"
                                                  : "INTERACTIVE: the session starts and the waits are answered from the web UI"),
             TRUING_FAST_DEMO ? "SYNTHETIC navigation + SYNTHETIC runout" : "manual navigation + manual runout");
    truing_fixture_wheel_class_sym32(&s.wheel);
    truing_fixture_solver_config(&s.solver, 32u);
    truing_fixture_chain_profile_inmp441(&s.chain);
    truing_fixture_tension_model_profile_complete(&s.tmodel);
    truing_fixture_machine_profile(&s.machine);
    s.clock.now_ms = boot_clock_now;
    s.clock.ctx = NULL;
    /* Acoustic: the real subsystem (onset, spectrum, candidates, interim selection, model) on a
     * synthetic pluck at 460 Hz with a faint noise floor; the fake actuator is "attached". */
    truing_audio_synthetic_init(&s.audio, &s.audio_ctx, 460.0f, 0.3f, 0.25f, 0.3f, 1e-4f);
    truing_pluck_fake_init(&s.pluck, &s.pluck_ctx, true);
    const size_t scratch_bytes = truing_acoustic_real_scratch_bytes(&s.chain);
    s.acoustic_scratch = heap_caps_malloc(scratch_bytes, MALLOC_CAP_SPIRAM);
    const char *adetail = NULL;
    if (s.acoustic_scratch == NULL ||
        !truing_acoustic_real_init(&s.acoustic, &s.actx, s.clock, &s.chain, &s.tmodel, &s.audio, &s.pluck, s.acoustic_scratch,
                                   scratch_bytes, &adetail)) {
        ESP_LOGE(TAG, "acoustic subsystem init failed (%s)", adetail != NULL ? adetail : "scratch");
        vTaskDelete(NULL);
        return;
    }
    /* THE fast-demo substitution, and the only one. Both implementations already exist and
     * both already declare what they are; picking between them here - before
     * truing_orch_init() - is what makes the whole session honest downstream.
     *
     *   manual     TRUING_SOURCE_REAL       a person turns the wheel and reads the gauges,
     *                                       so navigation returns PENDING_OPERATOR and
     *                                       runout advertises MANUAL_ENTRY: the
     *                                       orchestrator stops and asks.
     *   synthetic  TRUING_SOURCE_SYNTHETIC  the implementation can do it itself, so
     *                                       navigation returns DONE and runout answers a
     *                                       snapshot: the orchestrator walks the same
     *                                       states without stopping.
     *
     * No orchestrator branch is involved and no wait is bypassed: POSITION and READ_RUNOUT
     * are entered and left exactly as before. The only difference is that the thing being
     * asked can answer for itself, which is the entire point of the HAL boundary. */
    if (TRUING_FAST_DEMO) {
        truing_runout_synthetic_init(&s.runout, &s.rsyn, s.clock, 32u);
        truing_navigation_synthetic_init(&s.nav, &s.nsyn, s.clock, 32u, 32u, &s.machine, NULL, 0.0f, 0u);
    } else {
        truing_runout_manual_init(&s.runout, &s.rctx, s.clock);
        truing_navigation_manual_init(&s.nav, &s.nctx, s.clock, 32u, 32u, &s.machine);
    }
    truing_artifact_store_status_t ast;
    const truing_artifact_t *art = truing_artifact_store_load_fixture(&s.wheel, &s.solver, &ast);
    if (art == NULL) {
        ESP_LOGE(TAG, "no usable artifact (%s: %s): the session cannot start (SPEC 7.5 ABORT_NO_MODEL)",
                 truing_artifact_result_str(ast.result), ast.detail != NULL ? ast.detail : "");
        vTaskDelete(NULL);
        return;
    }
    truing_calc_artifact_init(&s.calc, &s.cctx, art, &s.wheel);
    truing_telemetry_ring_init(&s.sink, &s.ring);
    truing_auto_operator_init(&s.op);
    /* Starting state: the model's own response to three mis-set spokes (+0.30 rev on 3, -0.25 rev on 10,
     * +0.15 rev on 21), clearly outside the lateral tolerance and inside the 1.0 rev safety bound, so the
     * simulated wheel and the calculation share one linear world (Eq. 2). */
    float d0[TRUING_MAX_SPOKES], lateral[TRUING_MAX_RIM_ANGLES], radial[TRUING_MAX_RIM_ANGLES];
    memset(d0, 0, sizeof(d0));
    d0[3] = 0.30f;
    d0[10] = -0.25f;
    d0[21] = 0.15f;
    for (uint8_t k = 0; k < 32u; ++k) {
        float uu = 0.0f, vv = 0.0f;
        for (uint8_t i = 0; i < 32u; ++i) {
            uu += art->phi_u[k][i] * d0[i];
            vv += art->phi_v[k][i] * d0[i];
        }
        lateral[k] = uu;
        radial[k] = vv;
    }
    truing_auto_operator_set_wheel(&s.op, 32u, lateral, radial, 0.0f);
    s.op.response_fn = model_response;
    s.op.user = (void *)art;
    /* FAST DEMO: the same starting wheel, put where the synthetic runout implementation
     * reads from. One mis-set wheel, two ways of being asked about it. */
    if (TRUING_FAST_DEMO) {
        for (uint8_t k = 0; k < 32u; ++k) {
            truing_runout_synthetic_set_index(&s.rsyn, k, lateral[k], radial[k]);
        }
    }
    float start_max = 0.0f;
    for (uint8_t k = 0; k < 32u; ++k) {
        if (fabsf(lateral[k]) > start_max) start_max = fabsf(lateral[k]);
    }
    ESP_LOGI(TAG, "starting state from the model: max|lateral| = %.3f mm (tolerance %.2f mm)", (double)start_max,
             (double)s.solver.tol_lateral_mm);

    truing_orch_deps_t deps;
    memset(&deps, 0, sizeof(deps));
    deps.wheel = &s.wheel;
    deps.solver = &s.solver;
    deps.chain = &s.chain;
    deps.tension_model = &s.tmodel;
    deps.machine = &s.machine;
    deps.acoustic = &s.acoustic;
    deps.runout = &s.runout;
    deps.navigation = &s.nav;
    deps.calc = &s.calc;
    deps.telemetry = &s.sink;
    deps.clock = s.clock;
    deps.firmware_version = TRUING_FIRMWARE_VERSION;
    if (!truing_orch_init(&s.orch, &deps)) {
        ESP_LOGE(TAG, "orchestrator init failed");
        vTaskDelete(NULL);
        return;
    }

    /* SPEC §12.1 transport. Failure is not fatal: §12.2 makes the host optional for
     * autonomous work, so the machine runs and the console keeps the record. */
    s.wire_up = truing_net_start() &&
                truing_wire_session_init(&s.session, &s.orch, &s.ring, truing_net_sink(), false);
    if (!s.wire_up) {
        ESP_LOGW(TAG, "no wire transport; running with the console as the only observer");
    }

    int64_t t0 = esp_timer_get_time();
    bool started = false;
    uint32_t steps = 0u, waits = 0u, delays = 0u;
    truing_orch_step_t r = TRUING_ORCH_ADVANCED;
    for (;;) {
        r = truing_orch_step(&s.orch);
        drain_telemetry();
        service_wire();
        if (TRUING_FAST_DEMO) {
            truing_orch_snapshot_t fd;
            truing_orch_snapshot(&s.orch, &fd);
            fastdemo_track_adjustment(&fd, art);
        }
        /* The auto-operator is a DEVELOPMENT DOUBLE for the human, not machine behaviour.
         * While a browser is attached the real operator answers the wait, which is the
         * point of the transport; with nobody attached the double keeps the existing
         * self-play so the on-target evidence does not regress. Waiting here is correct
         * behaviour, not a failure (SPEC §12.2) — and a human taking their time is not
         * progress to be charged against the step budget. */
        if (r == TRUING_ORCH_WAITING_OPERATOR && (!TRUING_SELF_PLAY || truing_net_client_connected())) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        /* An interactive build does not start a session on its own. Sitting in READY with
         * nobody asking is the correct behaviour, not a stall, and it is what makes the
         * operator's Start truing a real control rather than a decoration. */
        if (!TRUING_SELF_PLAY && r == TRUING_ORCH_IDLE) {
            truing_orch_snapshot_t idle_snap;
            truing_orch_snapshot(&s.orch, &idle_snap);
            if (idle_snap.state == TRUING_STATE_READY) {
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
        }
        ++steps;
        /* Let the core-0 idle task run between steps. This task outranks IDLE0, which must
         * run for the scheduler's own housekeeping and now shares core 0 with the web
         * server. It has to be EVERY step, not every N: a single step can be a whole
         * acoustic measurement, so yielding every 64 would mean minutes of starvation.
         * One tick against a 3.7 s measurement is not a cost worth optimising. */
        vTaskDelay(1);
        if (r == TRUING_ORCH_WAITING_OPERATOR) {
            truing_orch_snapshot_t snap;
            truing_intent_t intent;
            truing_orch_snapshot(&s.orch, &snap);
            /* Only reachable in a self-play build; the interactive one continued above. */
            if (!truing_auto_operator_answer(&s.op, &snap, &intent)) {
                ESP_LOGE(TAG, "auto-operator cannot answer wait kind %s", truing_wait_kind_str(snap.active_wait.kind));
                break;
            }
            ++waits;
            truing_reason_t why = TRUING_REASON_NONE;
            const truing_intent_verdict_t v = truing_orch_submit_intent(&s.orch, &intent, &why);
            if (v != TRUING_INTENT_ADMIT_ACCEPT) {
                ESP_LOGE(TAG, "answer rejected: %s", truing_intent_verdict_str(v));
                break;
            }
        } else if (r == TRUING_ORCH_DELAY) {
            ++delays;
            vTaskDelay(pdMS_TO_TICKS(s.orch.requested_delay_ms));
        } else if (r == TRUING_ORCH_POLL_NAVIGATION) {
            vTaskDelay(pdMS_TO_TICKS(10));
        } else if (r == TRUING_ORCH_IDLE) {
            truing_orch_snapshot_t snap;
            truing_orch_snapshot(&s.orch, &snap);
            if (snap.state == TRUING_STATE_READY && !started && TRUING_SELF_PLAY) {
                started = true;
                truing_intent_t go;
                memset(&go, 0, sizeof(go));
                go.type = TRUING_INTENT_START_TRUING;
                truing_reason_t why = TRUING_REASON_NONE;
                const truing_intent_verdict_t v = truing_orch_submit_intent(&s.orch, &go, &why);
                ESP_LOGI(TAG, "START_TRUING -> %s (%s)", truing_intent_verdict_str(v), truing_reason_str(why));
                if (v != TRUING_INTENT_ADMIT_ACCEPT) {
                    break;
                }
            } else if (snap.init_error != TRUING_REASON_NONE) {
                /* The only genuinely terminal idle: the machine could not initialise. */
                ESP_LOGE(TAG, "idle in %s (init_error=%s)", truing_state_str(snap.state),
                         truing_reason_str(snap.init_error));
                break;
            } else {
                /* IDLE is only ever returned from READY or a failed INITIALIZE, so a
                 * different state here means an intent arrived through service_wire()
                 * between the step and this snapshot and has already moved the machine
                 * on - which is exactly what pressing Start truing does. The IDLE we are
                 * holding is stale, not an error. */
                vTaskDelay(pdMS_TO_TICKS(20));
            }
        } else if (r == TRUING_ORCH_TERMINAL) {
            if (TRUING_SELF_PLAY) {
                break;
            }
            /* Interactive: say what happened on the console, then hand the machine back so
             * the next session is a button press rather than a power cycle. The long
             * summary below belongs to the unattended evidence run; here the operator has
             * the same thing in front of them in the UI. */
            truing_orch_snapshot_t done;
            truing_orch_snapshot(&s.orch, &done);
            ESP_LOGI(TAG, "run finished: %s (%s) after %" PRIu32 " steps in %lld ms",
                     truing_terminal_str(done.last_result), truing_reason_str(done.last_reason),
                     steps, (long long)((esp_timer_get_time() - t0) / 1000));
            if (!truing_orch_reset_to_ready(&s.orch)) {
                ESP_LOGW(TAG, "cannot return to READY; the endpoint stays up for queries only");
                break;
            }
            ESP_LOGI(TAG, "back in READY - press Start truing for another session");
            steps = 0u;
            waits = 0u;
            delays = 0u;
            t0 = esp_timer_get_time();
            continue;
        }
        if (steps > 20000u) {
            ESP_LOGE(TAG, "step budget exhausted");
            break;
        }
    }
    drain_telemetry();
    const int64_t elapsed_us = esp_timer_get_time() - t0;

    truing_orch_snapshot_t snap;
    truing_cycle_provenance_t prov;
    truing_orch_snapshot(&s.orch, &snap);
    truing_orch_provenance(&s.orch, &prov);
    ESP_LOGI(TAG, "ORCH DEMO SUMMARY: result=%s reason=%s cycles_run=%u steps=%" PRIu32 " waits_answered=%" PRIu32
                  " settle_delays=%" PRIu32 " elapsed=%lld ms",
             truing_terminal_str(snap.last_result), truing_reason_str(snap.last_reason), (unsigned)snap.cycles_run, steps,
             waits, delays, (long long)(elapsed_us / 1000));
    ESP_LOGI(TAG, "auto-operator: positions_confirmed=%" PRIu32 " runouts_entered=%" PRIu32 " adjustments_applied=%" PRIu32
                  " turns[3]=%.3f turns[10]=%.3f final lateral[3]=%.3f lateral[10]=%.3f",
             s.op.positions_confirmed, s.op.runouts_entered, s.op.adjustments_applied, (double)s.op.turns_applied[3],
             (double)s.op.turns_applied[10], (double)s.op.wheel.lateral_mm[3], (double)s.op.wheel.lateral_mm[10]);
    ESP_LOGI(TAG, "provenance: session=%" PRIu32 " artifact=%" PRIu32 " fp_set=%d model_profile=%" PRIu32 " chain=%" PRIu32
                  " machine=%" PRIu32 " layout=%s active_rows=%u valid_rows=%u suspect_rows=%u wheel_rotation=%.3f rad "
                  "(operator_confirmed=%d) non_real=%d",
             prov.session_id, prov.artifact_id, (int)prov.generating_fingerprint.set, prov.tension_model_profile_id,
             prov.chain_profile_id, prov.machine_profile_id, truing_layout_str(prov.active_layout),
             (unsigned)truing_row_mask_popcount(&prov.active_row_set), (unsigned)prov.plan.n_valid_rows,
             (unsigned)prov.plan.n_suspect_rows, (double)prov.wheel_position.rotation_rad,
             (int)prov.wheel_position.operator_confirmed, (int)prov.contains_non_real_implementations);
    ESP_LOGI(TAG, "telemetry ring: emitted=%" PRIu32 " dropped=%" PRIu32 " | orchestrator transitions=%" PRIu32
                  " intents_rejected=%" PRIu32,
             s.ring.emitted, s.ring.dropped, s.orch.transitions, s.orch.intents_rejected);
    float final_max = 0.0f;
    for (uint8_t k = 0; k < 32u; ++k) {
        if (fabsf(s.op.wheel.lateral_mm[k]) > final_max) final_max = fabsf(s.op.wheel.lateral_mm[k]);
    }
    ESP_LOGI(TAG, "simulated wheel after the run: max|lateral| = %.4f mm (tolerance %.2f mm)", (double)final_max,
             (double)s.solver.tol_lateral_mm);
    ESP_LOGI(TAG, "acoustic: calls=%" PRIu32 " estimates=%" PRIu32 " rejections=%" PRIu32 " plucks_commanded=%" PRIu32
                  " | last f1=%.3f Hz snr=%.1f dB analysis=%" PRIu32 " ms (source %s)",
             s.actx.calls, s.actx.estimates, s.actx.rejections, s.pluck_ctx.fires, (double)s.actx.diag.f1_hz,
             (double)s.actx.diag.snr_db, s.actx.diag.analysis_us / 1000u, s.audio.impl_name);
    ESP_LOGI(TAG, "NOTE: %s is the honest Stage 1 ceiling (SPEC 8.6.3). The calculation and the acoustic layers 2-4 are "
                  "real; the audio front end is a synthetic pluck and the wheel is a simulation of the artifact's own model, so "
                  "this proves the workflow, the solver and the DSP path, not the truing of a physical wheel.",
             truing_terminal_str(TRUING_TERMINAL_CONVERGED_GEOMETRIC_ONLY));

    if (!s.wire_up) {
        vTaskDelete(NULL);
        return;
    }
    /* The run is over but the record is not: both SPEC §12.2 queries answer from RAM,
     * and the provenance explaining the last adjustment is exactly what an operator
     * wants to read after the fact. So this task stays alive to serve them rather than
     * taking the endpoint down with it. */
    ESP_LOGI(TAG, "run complete; the wire endpoint stays up for state and provenance queries");
    for (;;) {
        drain_telemetry();
        service_wire();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void truing_orch_demo_start(void)
{
    /* Orchestrator side of the SPEC 4.5 task model: core 0, normal priority. */
    xTaskCreatePinnedToCore(demo_task, "orch_demo", 12288, NULL, tskIDLE_PRIORITY + 2, NULL, 0);
}
