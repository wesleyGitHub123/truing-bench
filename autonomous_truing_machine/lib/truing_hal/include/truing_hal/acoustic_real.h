/**
 * @file acoustic_real.h
 * The acoustic subsystem behind the one-call contract of acoustic_if.h (SPEC 9.1), composed
 * of the four seams (SPEC 9.2): a front end (audio_source_if), the ported layer 2, the
 * INTERIM layer 3 and the ported layer 4 (truing_dsp). Excitation, capture, onset
 * detection, analysis and model application are internal; the orchestrator sees a
 * TensionEstimate and nothing else.
 *
 * Status rules (SPEC 4.4.1, 7.4, 13.2):
 *   every successful estimate is `suspect` / PROVISIONAL_MODE_ID with mode identity
 *   presumed_fundamental, never `valid`, while layer 3 is interim;
 *   incomplete tension-model profile      -> unavailable / CALIBRATION_MISSING
 *   front end cannot deliver the format   -> init refuses; the interface answers unavailable / NOT_IMPLEMENTED
 *   capture timeout / hardware            -> unavailable / SENSOR_TIMEOUT
 *   dropped samples                       -> rejected    / CAPTURE_OVERRUN
 *   cancelled                             -> unavailable / CANCELLED
 *   no onset in the capture               -> rejected    / NO_ONSET_DETECTED
 *   too little signal after gating        -> rejected    / VALUE_OUT_OF_RANGE
 *   no strong peak at all                 -> rejected    / LOW_SNR
 *   strong peaks but none in the f1 band  -> rejected    / FREQ_OUT_OF_RANGE
 *   SNR below the chain's measurement gate-> rejected    / LOW_SNR
 *   model refuses                         -> rejected    / MODEL_REJECTED or NO_F2_PARTNER
 *   no actuator at the spoke's station,
 *   or it failed to fire                  -> rejected    / EXCITATION_UNAVAILABLE (no capture)
 *                                            (except a debug no_fire control, which commands no actuator
 *                                            and so needs none -- truing_acoustic_real_set_debug_override)
 *
 * Excitation: each acoustic station has its own actuator (truing_acoustic_station_for_spoke()
 * picks the station, and with it the actuator). There is no hand-pluck fallback.
 */
#ifndef TRUING_HAL_ACOUSTIC_REAL_H
#define TRUING_HAL_ACOUSTIC_REAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "truing/config.h"
#include "truing_dsp/analysis.h"
#include "truing_dsp/onset.h"
#include "truing_hal/acoustic_if.h"
#include "truing_hal/audio_source_if.h"
#include "truing_hal/clock_if.h"
#include "truing_hal/pluck_if.h"
#include "truing_hal/telemetry_if.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Diagnostics of the last measurement, for telemetry and bring-up (not part of the record). */
typedef struct {
    truing_audio_result_t capture_result;
    uint32_t n_captured;
    uint32_t onsets;
    uint32_t onset_sample;
    float    onset_threshold;
    truing_dsp_window_t window;
    uint32_t n_fft;
    uint32_t n_strong_peaks;
    uint32_t n_peaks_in_band;
    float    f1_hz, f2_hz, snr_db;
    float    l_eff_m;
    uint32_t capture_us, analysis_us;   /* measured by the clock if it has that resolution */
    uint8_t  station;                   /* truing_station_id_t of the spoke's own station -- the actuator that WOULD strike it,
                                         * recorded whether or not it fired. Read it as an excitation only together with
                                         * `fired`: a no_fire control still carries its spoke's station. UNSET on replay */
    bool     fired;                     /* that actuator was commanded for this capture; false for a no_fire control */
    float    pulse_ms;                  /* the pulse it was commanded with (a debug override's width if one was applied);
                                         * 0 for a no_fire control, NaN on replay */
    bool     pulse_measured;            /* the actuator reported a measured width for this fire */
    uint32_t pulse_us_measured;         /* that measured width, in us; 0 unless pulse_measured */
    /* The front end's own per-capture diagnostics (audio_source_if.h capture_report), when it
     * can report them -- additive detail alongside capture_result, never a new decision. */
    bool     audio_report;              /* the source reported per-capture audio diagnostics */
    uint32_t pre_roll_words_delivered;
    uint32_t pre_roll_words_configured;
    uint32_t capture_overrun_events;
    uint32_t ring_age_us;
    uint32_t worst_read_gap_us;
    /* True iff this capture was fired by the debug channel's MEASURE_ONCE (SPEC §12.5), not by
     * the orchestrator's own state machine. A safety discriminator: it must never be easy for
     * campaign tooling, or any future report generator reading /debug/capture.json, to mistake
     * a bench-triggered capture for real session evidence. Defaults false; set only by
     * truing_acoustic_real_mark_debug_measurement() and consumed exactly once. */
    bool     debug_triggered;
} truing_acoustic_real_diag_t;

/* The excitation actuators, one per acoustic station, indexed by truing_acoustic_station_slot().
 * A NULL entry is a station with no actuator installed: its spokes are rejected, never plucked
 * by hand. */
typedef struct {
    truing_pluck_if_t *at[TRUING_ACOUSTIC_STATIONS];
} truing_acoustic_actuators_t;

/* Told, as it happens, which part of a measurement is running. Deliberately NOT on
 * acoustic_if.h: the orchestrator's contract with the acoustic subsystem is one call in, one
 * estimate out (SPEC §9.1), and that stays exactly as it was. This is a side channel wired the
 * same way the pluck seam is, by whoever composes the system, and only the application does.
 *
 * Called ON the measuring task, inside the measurement. It must not block and must not touch
 * the acoustic subsystem; an implementation that enqueues and returns is the only correct
 * shape. Nothing checks the return, because nothing may depend on it (SPEC §13.3). */
typedef void (*truing_acoustic_observer_fn)(void *ctx, const truing_telemetry_event_t *event);

typedef struct {
    truing_clock_if_t                  clock;
    const truing_chain_profile_t      *chain;
    const truing_excitation_profile_t *excitation;
    const truing_tension_model_profile_t *profile;   /* session-fixed (SPEC 12.3.1) */
    truing_audio_source_if_t          *source;
    truing_pluck_if_t                 *actuator[TRUING_ACOUSTIC_STATIONS];   /* NULL: none at that station */
    truing_dsp_params_t                params;
    truing_dsp_workspace_t             dsp;
    int32_t                           *words;        /* capture buffer, n_capture words */
    float                             *samples;      /* float view, n_capture */
    float                             *onset_env;    /* onset frame envelope scratch */
    uint32_t                           n_capture;
    uint32_t                           onset_env_cap;
    volatile bool                      cancel_requested;
    bool                               source_open;
    bool                               profile_ok;
    truing_acoustic_real_diag_t        diag;
    uint32_t                           calls;
    uint32_t                           estimates;
    uint32_t                           rejections;
    /* Phase observation (optional; NULL means the subsystem is silent as before). */
    truing_acoustic_observer_fn        observer;
    void                              *observer_ctx;
    /* This attempt's excitation, as reported on its phase frames (diag keeps the capture's). */
    uint8_t                            attempt_station;
    bool                               attempt_fired;
    /* The orchestrator retries by calling again with the same spoke, so consecutive calls for
     * one spoke in one cycle ARE the attempts. Counting them here is what lets the station say
     * "attempt 2" without the orchestrator having to report its own retry bookkeeping. */
    uint8_t                            attempt_spoke;
    uint8_t                            attempt_cycle;
    uint32_t                           attempt;
    bool                               attempt_valid;
    /* Capture evidence, written on the measuring task and read by whoever dumps it. */
    uint32_t                           capture_seq;
    uint8_t                            last_spoke;
    uint8_t                            last_cycle;
    uint32_t                           last_attempt;
    truing_status_t                    last_status;
    truing_reason_t                    last_reason;
    /* One-shot: set by truing_acoustic_real_mark_debug_measurement(), read and cleared by the
     * very next measure_run() -- AFTER that call's diag reset, never before it, so it survives
     * to land in the diag it was meant to mark rather than being wiped by it. A call that never
     * arrives leaves this false, which is the ordinary (non-debug) case. */
    bool                                debug_triggered_pending;
    /* One-shot campaign overrides set by truing_acoustic_real_set_debug_override(), snapshotted
     * and cleared at the TOP of the next measure_run() -- before any early return, so a rejected
     * attempt cannot leave them armed for whatever measurement comes next (a leaked no_fire
     * would silently skip a real session's strike). analyze_words() and reset_session() clear
     * them too, so no path that ends without a measure_run leaves them armed either.
     * override_pulse_ms_pending == 0 means none. */
    bool                                override_no_fire_pending;
    float                               override_pulse_ms_pending;
    /* capture_seq bumps the instant the buffer starts changing (SPEC §12.5); last_status/
     * last_reason are not written until note_outcome() runs, which can be well after that --
     * analyze() alone is seconds of arithmetic. outcome_seq catches up to capture_seq exactly
     * once, unconditionally, at the end of the attempt that moved it, so a reader can tell the
     * two apart: outcome_seq != capture_seq means an attempt's bookkeeping is still in flight
     * and last_status/last_reason may still describe the PREVIOUS attempt. */
    uint32_t                           outcome_seq;
} truing_acoustic_real_ctx_t;

/* Bytes of scratch the subsystem needs for this chain profile (capture buffers + DSP workspace).
 * Includes the padding that lets init() put the working set on a cache line whatever alignment
 * the caller's allocator happened to return; the placement is worth 30% of the per-pluck time. */
size_t truing_acoustic_real_scratch_bytes(const truing_chain_profile_t *chain);

/* Wire the seams. `scratch` must hold scratch_bytes(chain); it may live in PSRAM, and needs no
 * particular alignment -- init() aligns the working set itself. Returns false
 * (and leaves the interface answering `unavailable`) when the chain profile is invalid, the
 * source refuses the system format, or the scratch is too small. An incomplete tension-model
 * profile does NOT fail init: every measurement then reports CALIBRATION_MISSING (SPEC 11.3.1). */
bool truing_acoustic_real_init(truing_acoustic_if_t *self, truing_acoustic_real_ctx_t *ctx, truing_clock_if_t clock,
                               const truing_chain_profile_t *chain, const truing_excitation_profile_t *excitation,
                               const truing_tension_model_profile_t *profile, truing_audio_source_if_t *source,
                               const truing_acoustic_actuators_t *actuators, void *scratch, size_t scratch_bytes,
                               const char **detail);
/* Attach (or, with NULL, detach) the phase observer. Wired by the composition root after init,
 * never by the orchestrator. Safe to leave unset: the subsystem then emits nothing at all. */
void truing_acoustic_real_set_observer(truing_acoustic_if_t *self, truing_acoustic_observer_fn fn, void *observer_ctx);

/* Mark the NEXT measurement on this interface as debug-triggered (SPEC §12.5 MEASURE_ONCE):
 * its diag.debug_triggered will read true, once, and then the flag clears itself. Called by
 * the orchestrator's debug dispatch immediately before truing_acoustic_measure(), never by the
 * measurement path itself. A no-op for anything that is not this implementation, mirroring
 * every other truing_acoustic_real_* query below. Not part of acoustic_if.h: the orchestrator's
 * contract with the acoustic subsystem stays one call in, one estimate out (SPEC §9.1) -- this
 * is provenance, wired the same way the observer is, and changes no measurement outcome. */
void truing_acoustic_real_mark_debug_measurement(truing_acoustic_if_t *self);

/* Arm campaign overrides for the NEXT measurement only (SPEC §12.5 "inject values"; the MEASURE_ONCE
 * arg layout is in truing/debug_code.h). `no_fire`: run the capture and analysis but never command
 * an actuator -- a control, recorded as fired=false, and it needs no actuator to be present.
 * `pulse_ms` > 0: use this width instead of the excitation profile's for that one strike (recorded
 * as the applied pulse; the profile and its digest are untouched -- so a capture's excitation_digest
 * does NOT identify an overridden width: identify the level by pulse_ms together with station).
 * Consumed at the start of the next measure_run whether or not it goes on to fire, and cleared by
 * analyze_words() and reset_session() if one of those comes first. Called by the orchestrator's
 * debug dispatch beside mark_debug_measurement(); never by the measurement path. Same wiring rules
 * as that function, including that it is a silent no-op for any other implementation -- a synthetic
 * or recorded backend ignores the override and measures normally (it has no actuator to withhold). */
void truing_acoustic_real_set_debug_override(truing_acoustic_if_t *self, bool no_fire, float pulse_ms);

/* Analyse an already-captured buffer (n words) with no excitation: the path the bring-up and the
 * golden tests use. Identical to measure() after the capture step. */
void truing_acoustic_real_analyze_words(truing_acoustic_if_t *self, const int32_t *words, uint32_t n_words,
                                        uint8_t cycle_index, truing_tension_estimate_t *out);

/* ---- Capture evidence (SPEC §12.5 debug channel: "dump internals") -----------------------
 *
 * What the last measurement actually captured, exactly as the front end delivered it: int32
 * little-endian words with the 24-bit sample in the upper bits. That is the same
 * representation the golden fixtures carry, which is the point — a capture taken off a real
 * board replays through truing_acoustic_real_analyze_words() with no conversion, so a
 * failure seen once on the bench becomes a fixture that fails the same way on the host.
 *
 * Strictly observational. Nothing here participates in a measurement, no measurement waits
 * on it, and a system that never asks behaves identically (SPEC §13.3).
 *
 * `words` is the live capture buffer, not a copy — this subsystem allocates one and reuses
 * it. `seq` changes whenever a measurement begins overwriting it, so a reader samples seq,
 * reads, samples again: unchanged means it read one whole capture rather than the tail of
 * one and the head of the next. There is no lock, and adding one would put the reader in a
 * position to stall a measurement. */
typedef struct {
    const int32_t *words;        /* NULL until something has been captured */
    uint32_t       n_words;      /* valid words in the buffer (diag.n_captured) */
    uint32_t       seq;          /* 0 = nothing captured yet */
    uint8_t        spoke_id;     /* 0 for the analyze_words() replay path */
    uint8_t        cycle_index;
    uint32_t       attempt;
    truing_status_t status;      /* the outcome these words produced */
    truing_reason_t reason;
    truing_acoustic_real_diag_t diag;
} truing_acoustic_capture_view_t;

/* False when there is nothing to show (never measured, or not this implementation). */
bool truing_acoustic_real_last_capture(const truing_acoustic_if_t *self, truing_acoustic_capture_view_t *out);

/* The sequence number alone, for the before/after check around a long read. */
uint32_t truing_acoustic_real_capture_seq(const truing_acoustic_if_t *self);

/* True iff a capture exists whose outcome bookkeeping (last_status/last_reason) has not yet
 * caught up to the words already sitting in the buffer -- an attempt is mid-flight. A reader
 * that observes this should not trust truing_acoustic_real_last_capture() yet: the words may be
 * the new attempt's while last_status/last_reason still describe the previous one (SPEC
 * §12.5/§13.3: a dump must describe the attempt it is evidence of, not a mix of two). False
 * for anything that is not this implementation, mirroring truing_acoustic_real_capture_seq(). */
bool truing_acoustic_real_capture_pending(const truing_acoustic_if_t *self);

#ifdef __cplusplus
}
#endif
#endif /* TRUING_HAL_ACOUSTIC_REAL_H */
