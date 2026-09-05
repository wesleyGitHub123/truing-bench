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
 *                          continuously; every measurement captures a bounded window of
 *                          real audio and the existing DSP runs on it. No excitation
 *                          actuator is built, so the pluck seam is NULL: the capture
 *                          listens for the hand pluck at the station and reports
 *                          NO_ONSET_DETECTED when none arrives. The estimate stays
 *                          suspect / PROVISIONAL_MODE_ID (SPEC 4.4.1): a real front end
 *                          is a real acquisition, not a validated measurement.
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

/* The physical front end only makes sense where a person is at the station to pluck: the
 * auto-operator cannot excite a spoke, and a microphone pointed at the fast demo's
 * synthetic rim while the session claims synthetic runout is a story told twice. */
#if TRUING_REAL_FRONT_END && (TRUING_SELF_PLAY || TRUING_FAST_DEMO)
#error "TRUING_REAL_FRONT_END is an interactive-image variant: pick it or the other image, not both."
#endif

/* One string, used by the boot log, GET /id and the page header, so that "which image is
 * this" has a single answer no matter who asks. */
#if TRUING_FAST_DEMO
#define TRUING_BUILD_MODE_STR "fastdemo"
#elif TRUING_SELF_PLAY
#define TRUING_BUILD_MODE_STR "selfplay"
#elif TRUING_REAL_FRONT_END
#define TRUING_BUILD_MODE_STR "interactive+inmp441"
#else
#define TRUING_BUILD_MODE_STR "interactive"
#endif

#endif /* TRUING_BUILD_MODE_H */
