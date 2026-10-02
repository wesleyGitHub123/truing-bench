/**
 * @file build_mode.h
 * Which image this is, decided at compile time and nowhere else.
 *
 * The images differ in WHICH IMPLEMENTATIONS ARE WIRED IN, never a runtime switch. A session
 * cannot change its own nature half way through, and no browser command can turn a physical
 * run into a synthetic one: the substitution happens before truing_orch_init(), so session
 * admission records it (SPEC §6.2) and every provenance answer for that session carries it.
 *
 *   INTERACTIVE (default)  manual navigation + manual runout, both TRUING_SOURCE_REAL.
 *                          A person positions the wheel and reads the dial gauges. The
 *                          operator owns the session; the firmware answers nothing.
 *
 *   SELF-PLAY              the auto-operator starts the session and answers every wait,
 *                          for the unattended on-target evidence run in
 *                          docs/BRINGUP_LOG.md. Stands down while a browser is attached.
 *
 *   FAST DEMO              interactive lifecycle - a person still presses Start and still
 *                          applies the adjustments - but the repetitive ACQUISITION is
 *                          done by SYNTHETIC implementations of navigation and runout.
 *                          The orchestrator asks the same interfaces it always asks; those
 *                          interfaces simply answer for themselves instead of stopping for
 *                          a human, so no state is skipped and no wait is faked.
 *
 *   INTERACTIVE + MIC      the interactive lifecycle with the PHYSICAL INMP441 front end
 *                          (TRUING_REAL_FRONT_END): the real microphone over I2S replaces
 *                          the synthetic pluck source, opened once at boot and drained
 *                          continuously; every measurement fires the solenoid at the
 *                          spoke's own acoustic station (LEFT/RIGHT), captures a bounded
 *                          window of real audio and runs the existing DSP on it. A station
 *                          whose solenoid the board profile does not declare refuses the
 *                          session (EXCITATION_UNAVAILABLE); nobody plucks by hand. The
 *                          estimate stays suspect / PROVISIONAL_MODE_ID (SPEC 4.4.1): a real
 *                          front end is a real acquisition, not a validated measurement.
 *
 *   FAST DEMO + MIC        the acoustic demonstration: the real INMP441 front end with the
 *                          synthetic acquisition path -- except that the two acoustic stations
 *                          are answered by the OPERATOR (composite navigation), because the
 *                          solenoids are real and each strike needs a real spoke under the
 *                          plunger -- and a BOUND on how many spokes are
 *                          struck (TRUING_ACOUSTIC_DEMO_SPOKES). A few real strikes prove
 *                          the microphone -> DSP path; the remaining tension rows are left
 *                          uncollected because the active layout does not contain them.
 *                          Legal only while that is true - see the run-time guard below.
 *
 *   FAST DEMO + MIC +      the campaign bench variant of the line above (TRUING_CAMPAIGN_DEBUG):
 *   CAMPAIGN               same wiring, plus the debug channel (SPEC §12.5) so MEASURE_ONCE can
 *                          drive a single station capture outside a session for bench
 *                          characterization (docs/SOLENOID_CAMPAIGN.md). Only this dedicated
 *                          build carries it; the demo image the audience sees never does.
 *                          One difference from the line above: it keeps SYNTHETIC navigation.
 *                          MEASURE_ONCE runs outside a session with the physical spoke declared
 *                          per shot, and composite navigation would put a human confirmation of
 *                          the reference between every boot and READY -- where MEASURE_ONCE is
 *                          admitted -- stalling the bench flow.
 *
 *   FAST DEMO + ROLLER     the roller development image (TRUING_ROLLER_NAV, DevKit only): the
 *                          fast demo's synthetic acoustic and runout, but navigation is the
 *                          AUTOMATED roller implementation driving a real TMC2209 + NEMA 17
 *                          (navigation_roller over wheel_drive_tmc2209). The operator places
 *                          spoke 0 once and confirms; every positioning after that turns the
 *                          real wheel. Open loop: slip is not detected (SPEC 10A.6). Refuses
 *                          every positioning until src/roller_calibration.h holds a measured
 *                          calibration (docs/ROLLER_BENCH.md).
 *
 * Fast demo is deliberately NOT self-play with a nicer label. Self-play leaves the REAL
 * manual implementations in place and has a robot press the buttons, which is right for a
 * test rig and wrong for a demonstration: provenance would record runout_manual/REAL for
 * numbers no gauge ever produced. Fast demo swaps the implementations instead, so the
 * synthetic origin is a fact about the session rather than a claim on a web page.
 */
#ifndef TRUING_BUILD_MODE_H
#define TRUING_BUILD_MODE_H

#ifndef TRUING_SELF_PLAY
#define TRUING_SELF_PLAY 0
#endif

#ifndef TRUING_FAST_DEMO
#define TRUING_FAST_DEMO 0
#endif

#ifndef TRUING_REAL_FRONT_END
#define TRUING_REAL_FRONT_END 0
#endif

#if TRUING_SELF_PLAY && TRUING_FAST_DEMO
#error "TRUING_SELF_PLAY and TRUING_FAST_DEMO are different images: pick one."
#endif

/* The physical front end strikes whatever spoke is physically at the acoustic station, and the
 * auto-operator can answer a positioning wait but cannot rotate the wheel: under self-play the
 * solenoid would fire at an unmoved spoke and file the result under the index it was asked for.
 * A synthetic answer driving a physical action is a wrong attribution, not a demo convenience. */
#if TRUING_REAL_FRONT_END && TRUING_SELF_PLAY
#error "TRUING_SELF_PLAY answers its own waits and cannot pluck a spoke: not with TRUING_REAL_FRONT_END."
#endif

/* Fast demo WITH the real front end is the acoustic demonstration image, and the bound below
 * is the only reason it is coherent. Real tension from the wheel in front of you and
 * synthetic runout from a simulated rim describe two different objects; a row vector built
 * from both would be a wheel that does not exist. That is only contained because the tension
 * channel is excluded from the solve by policy (SPEC 8.11), which makes the real plucks
 * displayed evidence rather than solver input. So the image samples a few spokes to show the
 * INMP441 -> DSP path works and does not spend acquisition on rows the solver has already
 * decided to discard.
 *
 * The condition is CHECKED AT RUN TIME, not assumed: the orchestrator applies the bound only
 * while the selected layout really is TENSION_ABSENT. If the artifact ever identifies its
 * common mode the layout becomes FULL, tension enters the solve, the bound stops applying and
 * every spoke is measured again. The failure direction is "it measured everything", which is
 * never a dishonest one. */
#ifndef TRUING_ACOUSTIC_DEMO_SPOKES
#if TRUING_FAST_DEMO && TRUING_REAL_FRONT_END
#define TRUING_ACOUSTIC_DEMO_SPOKES 3
#else
#define TRUING_ACOUSTIC_DEMO_SPOKES 0   /* 0 = no bound: measure every spoke, as the machine must */
#endif
#endif

#if TRUING_ACOUSTIC_DEMO_SPOKES && !(TRUING_FAST_DEMO && TRUING_REAL_FRONT_END)
#error "TRUING_ACOUSTIC_DEMO_SPOKES is the fast-demo acoustic image only: the real machine measures every spoke."
#endif

#ifndef TRUING_CAMPAIGN_DEBUG
#define TRUING_CAMPAIGN_DEBUG 0
#endif

/* TRUING_CAMPAIGN_DEBUG unlocks the debug channel (SPEC §12.5: development/bring-up only,
 * never the demonstration script) on the acoustic-demo image family, so MEASURE_ONCE-style
 * bench characterization (single capture, no session, docs/SOLENOID_CAMPAIGN.md) can be run
 * against the same real INMP441 front end and station excitation the demo uses, without a
 * full session's positioning and runout waits in the way. It is legal only stacked on top of
 * the acoustic demonstration combination — a bench build, not a new image family — and the
 * demo image itself (`nano_esp32_fastdemo_mic`) must never carry it: that image is what gets
 * shown to an audience, and the debug channel is a bench tool, not a script that runs
 * unattended in front of one. */
#if TRUING_CAMPAIGN_DEBUG && !(TRUING_FAST_DEMO && TRUING_REAL_FRONT_END)
#error "TRUING_CAMPAIGN_DEBUG is the acoustic-demo bench variant only: it needs both TRUING_FAST_DEMO and TRUING_REAL_FRONT_END."
#endif

#ifndef TRUING_ROLLER_NAV
#define TRUING_ROLLER_NAV 0
#endif

/* The roller image turns a real wheel for every positioning, so its acquisition must be the fast
 * demo's automatic path. Self-play is excluded twice over: it is not a fast demo, and its
 * auto-operator would confirm a spoke-0 reference nobody placed -- a synthetic answer anchoring a
 * physical drive. The real front end is excluded because roller NAVIGATION waits for the operator
 * to confirm spoke 0 before the machine reaches READY, and the acoustic bench channel
 * (MEASURE_ONCE) is admitted only in READY: the two would deadlock. A board wired for both mics and
 * roller uses TRUING_BENCH_DRIVE below instead, which moves the roller in raw steps with no
 * navigation and no reference. */
#if TRUING_ROLLER_NAV && TRUING_SELF_PLAY
#error "TRUING_ROLLER_NAV turns a real wheel from an operator-confirmed reference: not with TRUING_SELF_PLAY."
#endif
#if TRUING_ROLLER_NAV && !TRUING_FAST_DEMO
#error "TRUING_ROLLER_NAV is the fast-demo roller image: it needs TRUING_FAST_DEMO."
#endif
#if TRUING_ROLLER_NAV && TRUING_REAL_FRONT_END
#error "TRUING_ROLLER_NAV is the DevKit roller image: not with TRUING_REAL_FRONT_END (roller navigation waits for the operator to confirm spoke 0 before READY, and MEASURE_ONCE is admitted only in READY -- use TRUING_BENCH_DRIVE for a board wired for both)."
#endif

#ifndef TRUING_BENCH_DRIVE
#define TRUING_BENCH_DRIVE 0
#endif

/* TRUING_BENCH_DRIVE wires the roller's TMC2209 to the debug channel (DRIVE_* codes, raw steps) on
 * the campaign bench build, so one DevKit carrying both mics, both solenoids and the roller can run
 * mic experiments and move the roller from one command set without a reflash. It is a bench tool:
 * the drive is brought up DISABLED, navigation stays synthetic, the roller calibration is not
 * consulted, and it is legal only where the debug channel is (TRUING_CAMPAIGN_DEBUG, which needs
 * FAST_DEMO and REAL_FRONT_END). It is never the navigation drive, so it excludes ROLLER_NAV: two
 * owners of one STEP pin. */
#if TRUING_BENCH_DRIVE && !TRUING_CAMPAIGN_DEBUG
#error "TRUING_BENCH_DRIVE is the campaign bench build only: it needs TRUING_CAMPAIGN_DEBUG."
#endif
#if TRUING_BENCH_DRIVE && TRUING_ROLLER_NAV
#error "TRUING_BENCH_DRIVE and TRUING_ROLLER_NAV both claim the wheel drive: pick one owner."
#endif

#ifndef TRUING_DUAL_MIC
#define TRUING_DUAL_MIC 0
#endif

/* TRUING_DUAL_MIC reads both I2S slots: a second INMP441 on the shared bus, one microphone per
 * acoustic station. Each capture still hands the DSP ONE mono stream -- the struck station's own
 * mic -- and keeps the other only as a debug-dump companion. It is an exploratory seam for the
 * dual-mic characterization workbench (tools/workbench.py), legal on the campaign bench build; no
 * evidence-bearing session image may change what it listens with until a configuration has earned it
 * and per-station provenance exists (SPEC 11.3). The one exception is the pipeline-demonstration build
 * above, which bears no evidence by construction and uses it as a presentation choice. */
#ifndef TRUING_DEMO_ALWAYS_READ
#define TRUING_DEMO_ALWAYS_READ 0
#endif

/* TRUING_DEMO_ALWAYS_READ is the PIPELINE-DEMONSTRATION build (operator's decision, 2026-10-02, for the
 * 2026-10-03 demonstration): an excitation whose capture the DSP's quality gates would reject still yields a
 * frequency candidate, so the whole chain -- strike, capture, analysis, tension, display -- can be shown
 * working on the spot. Such a capture is reported SUSPECT / FORCED_CANDIDATE (acoustic_real.c,
 * "pipeline-demonstration fallback"); one the gates accept is untouched. Still rejected: a capture with
 * no signal on the line (all zeros or stuck), the rare one where no spectrum or finite SNR can be formed,
 * and a frequency the tension model refuses. It claims nothing about the acoustic method: no reading from
 * this build is evidence, and the page says so. It is an audience image, so the debug channel (a bench
 * tool) is excluded.
 *
 * DELIBERATE DEVIATION, scoped to this build: SPEC 7.4 maps no-onset / out-of-range / ambiguous captures
 * to REJECTED, and SPEC 8.11 makes SUSPECT solver-admissible. A forced candidate is therefore an
 * admissible row in principle; it stays display-only because this image's layout keeps tension out of
 * the solve (TENSION_ABSENT). That is CHECKED AT BOOT, not assumed: unless the loaded artifact selects
 * TENSION_ABSENT, orch_demo.c leaves always-read OFF (fails closed), and /id reports always_read false.
 *
 * It also listens with each station's OWN microphone when both are wired (TRUING_DUAL_MIC), which the
 * rule below otherwise reserves for the bench: that is this build's presentation choice (the mics sit
 * beside their solenoids), not a characterized configuration. */
#if TRUING_DEMO_ALWAYS_READ && !(TRUING_FAST_DEMO && TRUING_REAL_FRONT_END)
#error "TRUING_DEMO_ALWAYS_READ is the acoustic demonstration image: it needs TRUING_FAST_DEMO and TRUING_REAL_FRONT_END."
#endif
#if TRUING_DEMO_ALWAYS_READ && TRUING_CAMPAIGN_DEBUG
#error "TRUING_DEMO_ALWAYS_READ is an audience image: not with the bench debug channel (TRUING_CAMPAIGN_DEBUG)."
#endif

#if TRUING_DUAL_MIC && !TRUING_CAMPAIGN_DEBUG && !TRUING_DEMO_ALWAYS_READ
#error "TRUING_DUAL_MIC is the campaign bench build (or the pipeline-demonstration build): it needs TRUING_CAMPAIGN_DEBUG or TRUING_DEMO_ALWAYS_READ."
#endif

#ifndef TRUING_DEMO_ROLLER_JOG
#define TRUING_DEMO_ROLLER_JOG 0
#endif

/* TRUING_DEMO_ROLLER_JOG: the MANUAL roller mode of the demonstration build. The TMC2209 is brought up
 * disabled and the operator turns the wheel with it from the page (GET /demo/jog), then confirms the
 * spoke at the station BY EYE as before: navigation stays the operator's (composite: the acoustic
 * stations are manual). Step counts are a jog distance, never a position: a back-and-forth on the
 * spring-loaded roller (2026-10-01) did not return the wheel to the same spoke, so no step-based
 * position or roller calibration is used here. Not with the bench drive or roller navigation (one
 * owner of the STEP pin). */
#if TRUING_DEMO_ROLLER_JOG && !TRUING_DEMO_ALWAYS_READ
#error "TRUING_DEMO_ROLLER_JOG is part of the pipeline-demonstration build: it needs TRUING_DEMO_ALWAYS_READ."
#endif
#if TRUING_DEMO_ROLLER_JOG && (TRUING_BENCH_DRIVE || TRUING_ROLLER_NAV)
#error "TRUING_DEMO_ROLLER_JOG, TRUING_BENCH_DRIVE and TRUING_ROLLER_NAV all claim the wheel drive: pick one owner."
#endif

#ifndef TRUING_SPOKE_SENSOR
#define TRUING_SPOKE_SENSOR 0
#endif

/* TRUING_SPOKE_SENSOR: RESERVED for the AUTOMATIC roller mode (planned, hardware not delivered): a laser
 * break-beam across the strike point (transmitter on one side, receiver on the other) reports a spoke
 * passing; the roller decelerates from a known speed so the wheel stops with that spoke at the
 * solenoid. Position knowledge then comes from beam edges, not step counts. The seam is
 * truing_hal/spoke_sensor_if.h; there is no implementation yet, so the flag refuses to build. */
#if TRUING_SPOKE_SENSOR
#error "TRUING_SPOKE_SENSOR is reserved: the laser spoke sensor is not delivered yet (scaffold: truing_hal/spoke_sensor_if.h, docs/DEMO_BUILD.md)."
#endif

#ifndef TRUING_DEMO_TENSION_ONLY
#define TRUING_DEMO_TENSION_ONLY 0
#endif

/* TRUING_DEMO_TENSION_ONLY: the TENSION-SURVEY form of the pipeline-demonstration build (operator's
 * decision, 2026-10-03, for the demonstration). The session is the acoustic pass alone: once every spoke it
 * collects has been struck and read, it ends TENSION_SURVEY_COMPLETE -- no runout, no solve, no
 * adjustment waits (truing_orch_deps_t.end_after_tension_pass). The page then shows the recorded
 * tensions and a plan computed from them. Demonstration image only. */
#if TRUING_DEMO_TENSION_ONLY && !TRUING_DEMO_ALWAYS_READ
#error "TRUING_DEMO_TENSION_ONLY is part of the pipeline-demonstration build: it needs TRUING_DEMO_ALWAYS_READ."
#endif

/* One string, used by the boot log, GET /id and the page header, so that "which image is
 * this" has a single answer no matter who asks. */
#if TRUING_FAST_DEMO && TRUING_REAL_FRONT_END && TRUING_DEMO_ALWAYS_READ && TRUING_DUAL_MIC && TRUING_DEMO_ROLLER_JOG && TRUING_DEMO_TENSION_ONLY
#define TRUING_BUILD_MODE_STR "fastdemo+inmp441x2+alwaysread+jog+survey"
#elif TRUING_FAST_DEMO && TRUING_REAL_FRONT_END && TRUING_DEMO_ALWAYS_READ && TRUING_DUAL_MIC && TRUING_DEMO_ROLLER_JOG
#define TRUING_BUILD_MODE_STR "fastdemo+inmp441x2+alwaysread+jog"
#elif TRUING_FAST_DEMO && TRUING_REAL_FRONT_END && TRUING_DEMO_ALWAYS_READ && TRUING_DUAL_MIC
#define TRUING_BUILD_MODE_STR "fastdemo+inmp441x2+alwaysread"
#elif TRUING_FAST_DEMO && TRUING_REAL_FRONT_END && TRUING_DEMO_ALWAYS_READ && TRUING_DEMO_ROLLER_JOG
#define TRUING_BUILD_MODE_STR "fastdemo+inmp441+alwaysread+jog"
#elif TRUING_FAST_DEMO && TRUING_REAL_FRONT_END && TRUING_DEMO_ALWAYS_READ
#define TRUING_BUILD_MODE_STR "fastdemo+inmp441+alwaysread"
#elif TRUING_FAST_DEMO && TRUING_ROLLER_NAV
#define TRUING_BUILD_MODE_STR "fastdemo+roller"
#elif TRUING_FAST_DEMO && TRUING_REAL_FRONT_END && TRUING_CAMPAIGN_DEBUG && TRUING_DUAL_MIC && TRUING_BENCH_DRIVE
#define TRUING_BUILD_MODE_STR "fastdemo+inmp441x2+campaign+drive"
#elif TRUING_FAST_DEMO && TRUING_REAL_FRONT_END && TRUING_CAMPAIGN_DEBUG && TRUING_BENCH_DRIVE
#define TRUING_BUILD_MODE_STR "fastdemo+inmp441+campaign+drive"
#elif TRUING_FAST_DEMO && TRUING_REAL_FRONT_END && TRUING_CAMPAIGN_DEBUG && TRUING_DUAL_MIC
#define TRUING_BUILD_MODE_STR "fastdemo+inmp441x2+campaign"
#elif TRUING_FAST_DEMO && TRUING_REAL_FRONT_END && TRUING_CAMPAIGN_DEBUG
#define TRUING_BUILD_MODE_STR "fastdemo+inmp441+campaign"
#elif TRUING_FAST_DEMO && TRUING_REAL_FRONT_END
#define TRUING_BUILD_MODE_STR "fastdemo+inmp441"
#elif TRUING_FAST_DEMO
#define TRUING_BUILD_MODE_STR "fastdemo"
#elif TRUING_SELF_PLAY
#define TRUING_BUILD_MODE_STR "selfplay"
#elif TRUING_REAL_FRONT_END
#define TRUING_BUILD_MODE_STR "interactive+inmp441"
#else
#define TRUING_BUILD_MODE_STR "interactive"
#endif

#endif /* TRUING_BUILD_MODE_H */
