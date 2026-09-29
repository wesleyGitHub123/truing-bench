#include "orch_demo.h"

#include <inttypes.h>
#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "artifact_store.h"
#include "audio_i2s.h"
#include "board/board_profile.h"
#include "pluck_gpio.h"
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
#include "truing_hal/navigation_composite.h"
#include "truing_hal/navigation_manual.h"
#include "truing_hal/navigation_roller.h"
#include "truing_hal/navigation_synthetic.h"
#include "truing_hal/runout_if.h"
#include "truing_hal/telemetry_if.h"
#include "truing_orch/auto_operator.h"
#include "truing_orch/orchestrator.h"
#include "roller_calibration.h"
#include "truing_hal/tmc2209_frame.h"
#include "wheel_drive_tmc2209.h"

static const char *TAG = "orch_demo";

/* Which image this is: see build_mode.h. TRUING_SELF_PLAY says who answers the waits;
 * TRUING_FAST_DEMO says which implementations are wired in before the session starts.
 *
 * Kept as compile-time constants rather than #ifdef around the branches so every path is
 * type-checked in every build; the dead one folds away. */
#include "build_mode.h"

/* Every board profile must state whether each acoustic station's solenoid is wired; an undefined
 * macro would silently preprocess as 0 and hide a profile that simply forgot it. */
#if !defined(BOARD_PLUCK_ACTUATOR_LEFT_PRESENT) || !defined(BOARD_PLUCK_ACTUATOR_RIGHT_PRESENT)
#error "board profile does not define BOARD_PLUCK_ACTUATOR_LEFT_PRESENT and _RIGHT_PRESENT (0 or 1)"
#endif

/* The roller prototype is wired to the DevKit (docs/ROLLER_BENCH.md R0); the Nano's pins carry the
 * acoustic campaign's microphone and solenoids. */
#if TRUING_ROLLER_NAV && !defined(BOARD_PROFILE_S3_DEVKIT)
#error "TRUING_ROLLER_NAV is DevKit only (s3_devkit_fastdemo_roller)."
#endif
#if TRUING_BENCH_DRIVE && !defined(BOARD_PROFILE_S3_DEVKIT)
#error "TRUING_BENCH_DRIVE is DevKit only (s3_devkit_lab): the Nano's pins carry no roller."
#endif

/* Bench drive ramp (TRUING_BENCH_DRIVE): roller_smoke's cautious first-spin knobs, not a measured
 * calibration. `roll speed` (DRIVE_MOTION) changes them for the running image. */
#define BENCH_DRIVE_CRUISE_HZ 800u
#define BENCH_DRIVE_ACCEL_SS  1600u

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
    truing_excitation_profile_t excitation;
    /* One excitation actuator per acoustic station, indexed by truing_acoustic_station_slot(). */
    truing_pluck_if_t pluck[TRUING_ACOUSTIC_STATIONS];
    truing_pluck_fake_ctx_t pluck_ctx[TRUING_ACOUSTIC_STATIONS];        /* synthetic front end */
    truing_pluck_gpio_ctx_t pluck_gpio_ctx[TRUING_ACOUSTIC_STATIONS];   /* real front end: the stations' GPIOs */
    void *acoustic_scratch;
    truing_runout_if_t runout;
    truing_runout_manual_ctx_t rctx;
    truing_runout_synthetic_ctx_t rsyn;   /* FAST DEMO: the simulated wheel's rim, read as a snapshot */
    truing_navigation_if_t nav;
    truing_navigation_manual_ctx_t nctx;
    truing_navigation_synthetic_ctx_t nsyn;   /* FAST DEMO: positions itself instead of asking */
    /* FAST DEMO on a REAL front end: nav is the composite of these two (plan A6 / B1b). */
    truing_navigation_if_t nav_phys, nav_sim;
    truing_navigation_composite_ctx_t ncomp;
    bool composite_nav;   /* the wiring above is in use; reported by /id so the page does not infer it */
    /* ROLLER image: the automated navigation over the real TMC2209 drive (TRUING_ROLLER_NAV). */
    truing_navigation_roller_ctx_t nroll;
    truing_wheel_drive_if_t drive;
    truing_wheel_drive_tmc2209_ctx_t drive_ctx;
    bool drive_inited;
    bool drive_ok;
    /* FAST DEMO: the adjustment the operator is currently being asked to apply. The
     * simulated wheel reacts when they confirm it, which is what a real wheel would do. */
    bool     adj_pending;
    uint32_t adj_wait_id;
    uint8_t  adj_spoke;
    float    adj_turns_rev;
    /* Which acquisition path the CURRENT wiring provides, and one requested by the
     * operator that has not been applied yet. Applied only while idle (see the loop). */
    bool     acq_automatic;
    bool     acq_change_pending;
    bool     acq_requested;
    /* The starting wheel, kept so a re-wire can re-seed the simulated rim. */
    float    seed_lateral[TRUING_MAX_RIM_ANGLES];
    float    seed_radial[TRUING_MAX_RIM_ANGLES];
    bool     seeded;
    truing_calc_if_t calc;
    truing_calc_artifact_ctx_t cctx;      /* REAL calculation on the loaded artifact (Phase 1c) */
    truing_telemetry_if_t sink;
    truing_telemetry_ring_ctx_t ring;
    truing_auto_operator_t op;
    truing_orch_deps_t deps;              /* kept: a path change re-inits against it */
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

/* Point navigation and runout at one path or the other. Both implementations already
 * declare what they are; this is the whole of the difference between the two paths, and it
 * is why provenance stays honest without anyone maintaining a second story about it. */
/* Which images get the composite navigation: those that fire REAL solenoids inside an orchestrator
 * session -- the acoustic demonstration. NOT the campaign bench image (TRUING_CAMPAIGN_DEBUG): it runs
 * MEASURE_ONCE outside any session, and there the runner declares the physical spoke for every shot
 * (plan A6), so it never relied on the navigation belief. Giving it the composite would also put a
 * human confirmation of the reference between every boot and READY, and MEASURE_ONCE is admitted only
 * in READY, so the bench flow would stall. Reported to the page as a fact (/id composite_navigation). */
#define DEMO_COMPOSITE_NAV (TRUING_REAL_FRONT_END && !TRUING_CAMPAIGN_DEBUG)

/* ROLLER image: the TMC2209 drive, brought up once and left DISABLED. Its motion profile and the
 * microstep count it enforces come from the measured calibration; while that is empty the drive
 * refuses moves and navigation refuses positioning (CALIBRATION_MISSING), so nothing turns. */
#if TRUING_ROLLER_NAV || TRUING_BENCH_DRIVE
static void roller_drive_init_once(void)
{
    if (s.drive_inited) {
        return;
    }
    s.drive_inited = true;
    const truing_tmc2209_pins_t pins = {
        .step_gpio = BOARD_WHEEL_DRIVE_STEP_GPIO,
        .dir_gpio = BOARD_WHEEL_DRIVE_DIR_GPIO,
        .en_gpio = BOARD_WHEEL_DRIVE_ENABLE_GPIO,
        .diag_gpio = BOARD_WHEEL_DRIVE_DIAG_GPIO,
        .uart_num = BOARD_WHEEL_DRIVE_UART_NUM,
        .uart_tx_gpio = BOARD_WHEEL_DRIVE_UART_TX_GPIO,
        .uart_rx_gpio = BOARD_WHEEL_DRIVE_UART_RX_GPIO,
        .node = 0u,
    };
#if TRUING_BENCH_DRIVE
    /* The bench drive never consults the roller calibration: raw steps and the bench sketch's ramp. It asks
     * for 16 microsteps -- the count measured on this motor (ROLLER_BENCH: 3200 steps = 1 revolution) -- and
     * the driver verifies it over the UART; without the UART it leaves MRES to the MS1/MS2 pins. */
    const truing_ramp_profile_t bench_prof = { 1000000u, BENCH_DRIVE_CRUISE_HZ, BENCH_DRIVE_ACCEL_SS };
    s.drive_ok = truing_wheel_drive_tmc2209_init(&s.drive, &s.drive_ctx, &pins, &bench_prof, 16u);
    ESP_LOGW(TAG, "BENCH DRIVE: the roller's TMC2209 answers the DRIVE_* debug codes in raw steps (%s); it starts "
                  "DISABLED at %u steps/s, %u steps/s2. Navigation stays synthetic.",
             s.drive_ok ? "initialised" : "INIT FAILED", (unsigned)BENCH_DRIVE_CRUISE_HZ, (unsigned)BENCH_DRIVE_ACCEL_SS);
#else
    const truing_ramp_profile_t prof = { 1000000u, k_roller_calibration.max_step_hz, k_roller_calibration.accel_steps_s2 };
    s.drive_ok = truing_wheel_drive_tmc2209_init(&s.drive, &s.drive_ctx, &pins, &prof, k_roller_calibration.microsteps);
    if (!truing_roller_calibration_valid(&k_roller_calibration)) {
        ESP_LOGW(TAG, "roller calibration is EMPTY (src/roller_calibration.h): every positioning will be refused "
                      "CALIBRATION_MISSING until docs/ROLLER_BENCH.md R3/R4 fill it");
    } else {
        ESP_LOGI(TAG, "roller calibration: %.2f steps/wheel rev at %u microsteps, %lu steps/s, %lu steps/s2 (%s)",
                 (double)k_roller_calibration.steps_per_wheel_rev, (unsigned)k_roller_calibration.microsteps,
                 (unsigned long)k_roller_calibration.max_step_hz, (unsigned long)k_roller_calibration.accel_steps_s2,
                 k_roller_calibration.evidence != NULL ? k_roller_calibration.evidence : "?");
    }
#endif
}
#endif

static void wire_acquisition(bool automatic)
{
    s.composite_nav = false;
    if (automatic) {
        truing_runout_synthetic_init(&s.runout, &s.rsyn, s.clock, 32u);
#if DEMO_COMPOSITE_NAV
        /* The solenoids here are REAL. Purely synthetic navigation would tell the orchestrator that a
         * spoke was positioned at its station while nothing moved, so the derived station's actuator
         * would strike whatever spoke happened to be under it and the result would be filed under a
         * spoke that was never struck (plan A6). So the two acoustic stations are answered by the
         * operator -- who places and confirms each struck spoke -- while runout and adjustment, whose
         * data is simulated, stay simulated, behind one navigation authority. The composite is
         * labelled SYNTHETIC (the weakest of its parts). This removes the orchestrator's belief that a
         * spoke is positioned when nothing moved; it does NOT verify that the spoke the operator placed
         * is the one requested -- nothing senses placement (sensor_confirmed is false). */
        static const truing_station_id_t k_physical[] = { TRUING_STATION_ACOUSTIC_LEFT, TRUING_STATION_ACOUSTIC_RIGHT };
        truing_navigation_manual_init(&s.nav_phys, &s.nctx, s.clock, 32u, 32u, &s.machine);
        truing_navigation_synthetic_init(&s.nav_sim, &s.nsyn, s.clock, 32u, 32u, &s.machine, NULL, 0.0f, 0u);
        if (!truing_navigation_composite_init(&s.nav, &s.ncomp, &s.nav_phys, &s.nav_sim, k_physical, 2u)) {
            /* Fail closed: a composite that did not initialise refuses every request, so no session can
             * start. Falling back to synthetic navigation here would reintroduce the wrong-actuator hazard. */
            ESP_LOGE(TAG, "navigation composite failed to initialise; sessions cannot start");
        }
        s.composite_nav = true;
#elif TRUING_ROLLER_NAV
        /* The roller turns the real wheel for every positioning; the operator places spoke 0 once. A
         * drive that failed to initialise is not handed over, so the reference is refused and no
         * session can start -- never a silent fall back to synthetic positioning. */
        roller_drive_init_once();
        truing_navigation_roller_init(&s.nav, &s.nroll, s.clock, 32u, 32u, &s.machine, s.drive_ok ? &s.drive : NULL,
                                      &k_roller_calibration);
#else
        truing_navigation_synthetic_init(&s.nav, &s.nsyn, s.clock, 32u, 32u, &s.machine, NULL, 0.0f, 0u);
#endif
        if (s.seeded) {
            for (uint8_t k = 0; k < 32u; ++k) {
                truing_runout_synthetic_set_index(&s.rsyn, k, s.seed_lateral[k], s.seed_radial[k]);
            }
        }
    } else {
#if TRUING_ROLLER_NAV
        if (s.drive_ok) {
            /* Manual path on the roller image: a person turns the wheel, so the coils let go. */
            truing_wheel_drive_enable(&s.drive, false, NULL);
        }
#endif
        truing_runout_manual_init(&s.runout, &s.rctx, s.clock);
        truing_navigation_manual_init(&s.nav, &s.nctx, s.clock, 32u, 32u, &s.machine);
    }
    s.acq_automatic = automatic;
}

bool truing_demo_acquisition_is_automatic(void)
{
    return s.acq_automatic;
}

bool truing_demo_navigation_is_composite(void)
{
    return s.composite_nav;
}

/* SPEC §12.5. Read on the HTTP task while the orchestrator may be measuring: the view is a
 * snapshot of the bookkeeping plus a pointer into the live capture buffer, and the sequence
 * number is how the reader finds out whether that buffer changed underneath it. Deliberately
 * no lock — a debug dump must not be able to stall a measurement (SPEC §13.3). */
bool truing_demo_last_capture(truing_acoustic_capture_view_t *out)
{
    return truing_acoustic_real_last_capture(&s.acoustic, out);
}

uint32_t truing_demo_capture_seq(void)
{
    return truing_acoustic_real_capture_seq(&s.acoustic);
}

bool truing_demo_capture_pending(void)
{
    return truing_acoustic_real_capture_pending(&s.acoustic);
}

bool truing_demo_far_capture(const int32_t **words, uint32_t *n_words, uint8_t *input)
{
#if TRUING_REAL_FRONT_END && TRUING_DUAL_MIC
    return truing_audio_i2s_companion(&s.audio, words, n_words, input);
#else
    (void)words;
    (void)n_words;
    (void)input;
    return false;
#endif
}

bool truing_demo_fire(uint32_t station, float pulse_ms)
{
#if TRUING_REAL_FRONT_END
    if (station >= TRUING_ACOUSTIC_STATIONS || !s.pluck_gpio_ctx[station].configured) return false;
    return s.pluck[station].fire != NULL && s.pluck[station].fire(&s.pluck[station], pulse_ms);
#else
    (void)station;
    (void)pulse_ms;
    return false;
#endif
}

bool truing_demo_fire_ready(uint32_t station)
{
#if TRUING_REAL_FRONT_END
    return station < TRUING_ACOUSTIC_STATIONS && s.pluck_gpio_ctx[station].configured;
#else
    (void)station;
    return false;
#endif
}

bool truing_demo_listen_start(uint32_t *pos)
{
#if TRUING_REAL_FRONT_END
    return truing_audio_i2s_listen_start(&s.audio, pos);
#else
    (void)pos;
    return false;
#endif
}

uint32_t truing_demo_listen_read(uint32_t station, uint32_t *pos, int32_t *out, uint32_t max, uint32_t *lost)
{
#if TRUING_REAL_FRONT_END
    return truing_audio_i2s_listen_read(&s.audio, station, pos, out, max, lost);
#else
    (void)station;
    (void)pos;
    (void)out;
    (void)max;
    (void)lost;
    return 0u;
#endif
}

bool truing_demo_bench_drive_state(truing_demo_drive_state_t *out)
{
#if TRUING_BENCH_DRIVE
    if (out == NULL || !s.drive_ok) {
        return false;
    }
    /* Plain reads of the driver's own fields: this runs on the HTTP task, so it must not call fault(),
     * which samples DIAG and can latch a fault / halt pulses. DIAG is reported raw beside it. */
    const truing_wheel_drive_tmc2209_ctx_t *c = &s.drive_ctx;
    out->enabled = c->enabled;
    out->moving = c->moving;
    out->faulted = c->faulted;
    out->fault_detail = c->fault_detail;
    out->diag_high = truing_wheel_drive_tmc2209_diag_level(c);
    out->uart_ok = c->uart_ok;
    out->position_steps = c->position;
    out->max_step_hz = c->profile.max_step_hz;
    out->accel_steps_s2 = c->profile.accel_steps_s2;
    /* The driver's own registers over the single-wire UART, on this HTTP task: two short reads (a few ms). The
     * step pulses come from a timer ISR and do not use the UART, so a move is not disturbed. */
    out->drv_ok = false;
    out->drv_status = 0u;
    out->gstat = 0u;
    out->cs_actual = 0u;
    out->drv_flags[0] = '\0';
    if (c->uart_ok) {
        uint32_t st = 0u, gs = 0u;
        if (truing_wheel_drive_tmc2209_read(&s.drive_ctx, TRUING_TMC2209_DRV_STATUS, &st)) {
            out->drv_ok = true;
            out->drv_status = st;
            out->cs_actual = truing_tmc2209_cs_actual(st);
            /* STST (standstill) is a state, not a fault. */
            truing_tmc2209_describe_drv_status(st & ~TRUING_TMC2209_DRV_STST, out->drv_flags, sizeof(out->drv_flags));
            (void)truing_wheel_drive_tmc2209_read(&s.drive_ctx, TRUING_TMC2209_GSTAT, &gs);
            out->gstat = gs;
        }
    }
    return true;
#else
    (void)out;
    return false;
#endif
}

truing_source_impl_t truing_demo_acoustic_source(void)
{
    return s.acoustic.source_impl;
}

const truing_chain_profile_t *truing_demo_chain_profile(void)
{
    return &s.chain;
}

const truing_excitation_profile_t *truing_demo_excitation_profile(void)
{
    return &s.excitation;
}

bool truing_demo_request_acquisition(bool automatic, const char **detail)
{
    if (!TRUING_FAST_DEMO) {
        if (detail != NULL) {
            *detail = "this image has no automatic acquisition path";
        }
        return false;
    }
    truing_orch_snapshot_t snap;
    truing_orch_snapshot(&s.orch, &snap);
    if (snap.session_active) {
        if (detail != NULL) {
            *detail = "a session is running; abort it first";
        }
        return false;
    }
    /* Recorded for the orchestrator task to apply at a safe point. Two plain words written
     * from the server task and read from the demo task; nothing here touches the state
     * machine, which has one owner (SPEC §12.4). */
    s.acq_requested = automatic;
    s.acq_change_pending = true;
    return true;
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

/* The acoustic phase events go STRAIGHT out of the wire session and deliberately not through
 * the telemetry ring, and the reason is the entire point of them.
 *
 * The ring is drained by this task between orchestrator steps, and one acoustic measurement IS
 * a step: the task sits inside real_measure for the whole capture and the seconds of FFT after
 * it. A LISTENING event posted to the ring would therefore be delivered after the window it
 * announces had already closed - it would tell the operator to pluck into a window that ended
 * four seconds ago, which is worse than saying nothing.
 *
 * This runs on the measuring task, inside the measurement, so it only enqueues: net_transport's
 * outbound queue is zero-tick and drops when full (SPEC 12.2). A dropped phase frame costs the
 * page a cue and costs the measurement nothing. */
static void acoustic_phase_observer(void *ctx, const truing_telemetry_event_t *ev)
{
    (void)ctx;
    if (s.wire_up) {
        (void)truing_wire_session_send_event(&s.session, ev);
    }
    /* Short on purpose: this is on the path to opening the capture window. */
    ESP_LOGI(TAG, "[%6" PRIu32 " ms] acoustic %s spoke %u attempt %" PRIu32 " actuator %s%s", ev->timestamp_ms,
             truing_acoustic_phase_str((truing_acoustic_phase_t)ev->u.acoustic.phase),
             (unsigned)ev->u.acoustic.spoke_index, ev->u.acoustic.attempt,
             truing_acoustic_actuator_str(ev->u.acoustic.station), ev->u.acoustic.fired ? " fired" : "");
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
    ESP_LOGI(TAG, "== Stage 1 workflow, %s (%s; %s; REAL truing calculation on the golden "
                  "fixture artifact%s) ==",
             (TRUING_FAST_DEMO && TRUING_REAL_FRONT_END)
                     ? "ACOUSTIC DEMONSTRATION: a few spokes are struck by the station solenoids on the real microphone "
                       "to show the front end works; runout is automatic and SYNTHETIC - this is not a physical wheel result"
             : TRUING_FAST_DEMO ? "FAST DEMO: a person starts the session and applies the adjustments; acquisition is "
                                "automatic and SYNTHETIC - this is not a physical wheel result"
                              : (TRUING_SELF_PLAY ? "SELF-PLAY: the auto-operator starts the session and answers its own waits"
                                                  : (TRUING_REAL_FRONT_END
                                                          ? "INTERACTIVE with the PHYSICAL INMP441 front end: a person positions the wheel and reads the gauges; each spoke is struck by its station's solenoid"
                                                          : "INTERACTIVE: the session starts and the waits are answered from the web UI")),
             TRUING_FAST_DEMO ? (TRUING_ROLLER_NAV
                                         ? "acquisition path selectable in the UI; starts on ROLLER navigation (the TMC2209 "
                                           "roller turns the real wheel from an operator-confirmed spoke 0, open loop) + SYNTHETIC runout"
                                 : DEMO_COMPOSITE_NAV
                                         ? "acquisition path selectable in the UI; starts on COMPOSITE navigation (the operator "
                                           "places each struck spoke at its solenoid station, the rest is simulated) + SYNTHETIC runout"
                                         : "acquisition path selectable in the UI; starts on SYNTHETIC navigation + "
                                           "SYNTHETIC runout")
                              : "manual navigation + manual runout",
             !TRUING_REAL_FRONT_END
                     ? "REAL acoustic layers 2-4 on a SYNTHETIC 460 Hz pluck source"
                     : (BOARD_PLUCK_ACTUATOR_LEFT_PRESENT && BOARD_PLUCK_ACTUATOR_RIGHT_PRESENT
                                ? "REAL acoustic layers 1-4 on the INMP441 microphone; each spoke is excited by the "
                                  "solenoid at its own acoustic station (LEFT/RIGHT)"
                                : "REAL acoustic layers 1-4 on the INMP441 microphone; at least one acoustic station has "
                                  "NO solenoid, so sessions are refused (EXCITATION_UNAVAILABLE) - no hand pluck"),
             TRUING_REAL_FRONT_END ? "" : "; the simulated wheel responds through the same influence model");
    truing_fixture_wheel_class_sym32(&s.wheel);
    truing_fixture_solver_config(&s.solver, 32u);
    truing_fixture_chain_profile_inmp441(&s.chain);
    truing_fixture_excitation_profile(&s.excitation);
    truing_fixture_tension_model_profile_complete(&s.tmodel);
    truing_fixture_machine_profile(&s.machine);
    s.clock.now_ms = boot_clock_now;
    s.clock.ctx = NULL;
#if TRUING_REAL_FRONT_END
    /* Acoustic with the PHYSICAL front end: the INMP441 over I2S, opened ONCE here and drained
     * continuously by its own core-1 task into a PSRAM ring; every measurement below captures
     * one bounded window from that ring (SPEC 9.3, 9.4). The sizing is what the acoustic
     * bring-up proved on target. Each acoustic station's actuator is wired below only if the
     * board profile declares its solenoid present; a station without one rejects its spokes and
     * refuses sessions (EXCITATION_UNAVAILABLE). There is no hand-pluck fallback. */
    const truing_audio_i2s_config_t icfg = {
        .dma_frame_num = 240u,      /* 5 ms per descriptor, multiple of 3, 960 B <= 4092 B (SPEC 9.4.1) */
        .dma_desc_num = 8u,         /* 40 ms of driver buffering against a 100 ms worst-case drain gap */
        .pre_trigger_words = (uint32_t)(s.chain.pre_trigger_ms * 48.0f),
        .ring_words = 48000u,
#if TRUING_DUAL_MIC
        /* One mic per acoustic station on the shared bus; the far one is a companion record. */
        .n_inputs = 2u,
        .station_input = { BOARD_I2S_MIC_INPUT_LEFT_STATION, BOARD_I2S_MIC_INPUT_RIGHT_STATION },
        .companion_words = (uint32_t)((s.chain.pre_trigger_ms + s.chain.capture_ms) * 48.0f + 0.5f),
#endif
    };
    const char *idetail = NULL;
    if (!truing_audio_i2s_init(&s.audio, &icfg, &idetail)) {
        ESP_LOGE(TAG, "I2S front end init failed (%s); no session can measure a spoke",
                 idetail != NULL ? idetail : "?");
        vTaskDelete(NULL);
        return;
    }
    /* The acoustic stations' actuators on their GPIOs (SPEC 4.3: pin numbers exist only in the
     * board profile). Wired ONLY when the board profile says that station's solenoid is attached
     * and bench-verified - gpio_config() succeeding is not evidence of a solenoid, and a bare pin
     * makes gpio_fire() return true for nothing (SPEC 17.3). */
#if BOARD_PLUCK_ACTUATOR_LEFT_PRESENT
    if (!truing_pluck_gpio_init(&s.pluck[0], &s.pluck_gpio_ctx[0], BOARD_PLUCK_ACTUATOR_LEFT_GPIO)) {
        ESP_LOGW(TAG, "LEFT actuator GPIO init failed (%d); sessions will be refused", BOARD_PLUCK_ACTUATOR_LEFT_GPIO);
    }
#endif
#if BOARD_PLUCK_ACTUATOR_RIGHT_PRESENT
    if (!truing_pluck_gpio_init(&s.pluck[1], &s.pluck_gpio_ctx[1], BOARD_PLUCK_ACTUATOR_RIGHT_GPIO)) {
        ESP_LOGW(TAG, "RIGHT actuator GPIO init failed (%d); sessions will be refused", BOARD_PLUCK_ACTUATOR_RIGHT_GPIO);
    }
#endif
#else
    /* Acoustic: the real subsystem (onset, spectrum, candidates, interim selection, model) on a
     * synthetic pluck at 460 Hz with a faint noise floor; a fake actuator is "attached" at each
     * acoustic station. */
    truing_audio_synthetic_init(&s.audio, &s.audio_ctx, 460.0f, 0.3f, 0.25f, 0.3f, 1e-4f);
    for (unsigned i = 0; i < TRUING_ACOUSTIC_STATIONS; ++i) {
        truing_pluck_fake_init(&s.pluck[i], &s.pluck_ctx[i], true);
    }
#endif
    /* Constant-folded, so both branches stay type-checked in every build (see build_mode.h).
     * Real front end: a station's GPIO actuator only if the board says it is wired AND it
     * configured; otherwise NULL, which refuses sessions and rejects that station's spokes. */
    truing_acoustic_actuators_t actuators;
    for (unsigned i = 0; i < TRUING_ACOUSTIC_STATIONS; ++i) {
        actuators.at[i] = TRUING_REAL_FRONT_END ? (s.pluck_gpio_ctx[i].configured ? &s.pluck[i] : NULL) : &s.pluck[i];
    }
    const size_t scratch_bytes = truing_acoustic_real_scratch_bytes(&s.chain);
    s.acoustic_scratch = heap_caps_malloc(scratch_bytes, MALLOC_CAP_SPIRAM);
    const char *adetail = NULL;
    if (s.acoustic_scratch == NULL ||
        !truing_acoustic_real_init(&s.acoustic, &s.actx, s.clock, &s.chain, &s.excitation, &s.tmodel, &s.audio, &actuators,
                                   s.acoustic_scratch, scratch_bytes, &adetail)) {
        ESP_LOGE(TAG, "acoustic subsystem init failed (%s)", adetail != NULL ? adetail : "scratch");
        vTaskDelete(NULL);
        return;
    }
    /* Wired by the composition root, which is the only place that knows there is a transport to
     * push to. The orchestrator's contract with the acoustic subsystem is unchanged. */
    truing_acoustic_real_set_observer(&s.acoustic, acoustic_phase_observer, NULL);
    /* THE fast-demo substitution. Both implementations already exist and both already declare
     * what they are; picking between them here - before truing_orch_init() - is what makes the
     * whole session honest downstream.
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
     * asked can answer for itself, which is the entire point of the HAL boundary.
     *
     * One refinement, and it is a second substitution rather than a variation of the first: on the
     * acoustic demonstration (real solenoids, DEMO_COMPOSITE_NAV) the "synthetic" navigation is a
     * composite -- the operator answers the two acoustic stations, the stand-in the rest -- because
     * real strikes need a real spoke under the plunger. See wire_acquisition(). */
    wire_acquisition(TRUING_FAST_DEMO ? true : false);
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
    /* The same starting wheel, kept so that whichever path is selected asks about ONE
     * mis-set wheel. The manual path asks a person for these numbers; the automatic path
     * reads them from the simulated rim. */
    for (uint8_t k = 0; k < 32u; ++k) {
        s.seed_lateral[k] = lateral[k];
        s.seed_radial[k] = radial[k];
    }
    s.seeded = true;
    if (s.acq_automatic) {
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
    /* 0 in every image but the acoustic demonstration, where it bounds the pluck pass to a few
     * spokes. The orchestrator ignores it unless the layout in force excludes tension, so this
     * cannot shorten a pass whose measurements the solver is going to use. */
    deps.tension_sample_limit = (uint8_t)TRUING_ACOUSTIC_DEMO_SPOKES;
    if (TRUING_ACOUSTIC_DEMO_SPOKES > 0) {
        ESP_LOGW(TAG, "ACOUSTIC DEMONSTRATION: at most %d spokes will be plucked; the rest are left "
                      "uncollected while the active layout is TENSION_ABSENT. Not a physical wheel measurement.",
                 (int)TRUING_ACOUSTIC_DEMO_SPOKES);
    }
    /* False in every image but the campaign bench build: TRUING_CAMPAIGN_DEBUG is the only
     * thing that ever sets this true, so TRUING_INTENT_DEBUG (SPEC §12.5, MEASURE_ONCE) stays
     * unreachable everywhere else, including the demo image the audience sees. */
    deps.debug_channel_enabled = TRUING_CAMPAIGN_DEBUG ? true : false;
    if (TRUING_CAMPAIGN_DEBUG) {
        ESP_LOGW(TAG, "CAMPAIGN DEBUG BUILD: the debug channel is enabled (SPEC 12.5). Bench "
                      "characterization only, never the demonstration script.");
    }
#if TRUING_BENCH_DRIVE
    /* Brought up DISABLED and handed to the debug channel only: navigation stays synthetic, so nothing
     * in a session can move it. A drive that failed to initialise is not handed over (DRIVE_* refused). */
    roller_drive_init_once();
    deps.bench_drive = s.drive_ok ? &s.drive : NULL;
#endif
    /* The interface structs are re-initialised IN PLACE when the path changes, so these
     * pointers stay valid and keep pointing at whichever implementation is wired now. */
    s.deps = deps;
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
        /* The simulated wheel exists only where the rim is simulated. On the manual path a
         * real person turns a real nipple and the change shows up in what they type. */
        if (s.acq_automatic) {
            truing_orch_snapshot_t fd;
            truing_orch_snapshot(&s.orch, &fd);
            fastdemo_track_adjustment(&fd, art);
        }
        /* A requested acquisition path is applied HERE and only here: on the orchestrator's
         * own task, between steps, with no session running. Re-initialising takes the
         * machine back through BOOT -> INITIALIZE, which re-establishes the spoke-0
         * reference through whichever navigation implementation is now wired - the manual
         * one will ask a person for it, the synthetic one will not. The next session is
         * then admitted against the implementations that actually ran (SPEC §6.2). */
        if (s.acq_change_pending) {
            truing_orch_snapshot_t now;
            truing_orch_snapshot(&s.orch, &now);
            if (!now.session_active) {
                s.acq_change_pending = false;
                if (s.acq_requested != s.acq_automatic) {
                    wire_acquisition(s.acq_requested);
                    (void)truing_orch_init(&s.orch, &s.deps);
                    ESP_LOGW(TAG, "acquisition path -> %s; re-initialising so the reference is "
                                  "established through it",
                             s.acq_automatic ? (TRUING_ROLLER_NAV
                                                        ? "AUTOMATIC (roller navigation on the real wheel + synthetic runout)"
                                                : DEMO_COMPOSITE_NAV
                                                        ? "AUTOMATIC (composite navigation: operator at the acoustic stations, synthetic elsewhere + synthetic runout)"
                                                        : "AUTOMATIC (synthetic navigation + synthetic runout)")
                                             : "MANUAL (operator positions and reads the gauges)");
                    steps = 0u;
                    waits = 0u;
                    delays = 0u;
                    started = false;
                    t0 = esp_timer_get_time();
                    continue;
                }
            }
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
    const uint32_t fires_left = TRUING_REAL_FRONT_END ? s.pluck_gpio_ctx[0].fires : s.pluck_ctx[0].fires;
    const uint32_t fires_right = TRUING_REAL_FRONT_END ? s.pluck_gpio_ctx[1].fires : s.pluck_ctx[1].fires;
    ESP_LOGI(TAG, "acoustic: calls=%" PRIu32 " estimates=%" PRIu32 " rejections=%" PRIu32 " fires LEFT=%" PRIu32
                  " RIGHT=%" PRIu32 " | last f1=%.3f Hz snr=%.1f dB analysis=%" PRIu32 " ms (source %s)",
             s.actx.calls, s.actx.estimates, s.actx.rejections, fires_left, fires_right, (double)s.actx.diag.f1_hz,
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
