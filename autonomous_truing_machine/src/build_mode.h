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

/* One string, used by the boot log, GET /id and the page header, so that "which image is
 * this" has a single answer no matter who asks. */
#if TRUING_FAST_DEMO && TRUING_REAL_FRONT_END && TRUING_CAMPAIGN_DEBUG
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
