#include "truing_hal/acoustic_real.h"

#include <math.h>
#include <string.h>

#include "truing_dsp/tension_model.h"
#include "truing_hal/records.h"

static size_t align8(size_t v)
{
    return (v + 7u) & ~(size_t)7u;
}

/* One acoustic call is two things to the person at the station - a window to pluck into and then
 * seconds of arithmetic - and until now it looked like one. These say which is running. Dropping
 * every one of them changes nothing: the window opens and closes on the firmware's clock. */
static void emit_phase(truing_acoustic_real_ctx_t *c, uint8_t cycle_index, truing_acoustic_phase_t phase,
                       uint8_t spoke_index, uint32_t window_ms)
{
    if (c->observer == NULL) {
        return;
    }
    truing_telemetry_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind = TRUING_EVT_ACOUSTIC_PHASE;
    ev.timestamp_ms = truing_clock_now_ms(&c->clock);
    ev.cycle_index = cycle_index;
    ev.u.acoustic.phase = (uint8_t)phase;
    ev.u.acoustic.spoke_index = spoke_index;
    ev.u.acoustic.pluck_commanded = c->diag.pluck_commanded;
    /* Reported from what actually happened this attempt, not from what is wired: an attached
     * actuator whose fire() failed did NOT command anything, and the ARMED lead-in re-enables
     * for exactly that case - so HAND is true of everything downstream, not just of the label.
     * Deriving this from available() instead would let a wired-but-failing actuator put "the
     * actuator was commanded" on the page while nothing happened (SPEC §17.3: a silent wrong
     * answer). An actuator that fired is the ACTUATOR case; everything else is a hand. */
    ev.u.acoustic.excitation = (uint8_t)(c->diag.pluck_commanded ? TRUING_EXCITATION_ACTUATOR
                                                                 : TRUING_EXCITATION_HAND);
    ev.u.acoustic.window_ms = window_ms;
    ev.u.acoustic.attempt = c->attempt;
    c->observer(c->observer_ctx, &ev);
}

/* The window the operator actually has: the pre-trigger is already in the ring when the call
 * starts, so only capture_ms is time they can still pluck into. */
static uint32_t listen_window_ms(const truing_chain_profile_t *chain)
{
    return (uint32_t)(chain->capture_ms + 0.5f);
}

/* The DSP working set is megabytes of double-precision arrays streamed out of PSRAM, and its
 * cost depends on where that block starts: measured on the DevKitC-1, moving the base by 20
 * bytes off a cache-line boundary costs 30% of the per-pluck time (Hilbert 2,078 -> 3,110 ms),
 * because every 64-byte line fill then serves a fraction of the doubles it fetched.
 *
 * heap_caps_malloc promises only 4-byte alignment, so which side of that a caller lands on is
 * decided by whatever allocated before it -- Phase 1f was lucky and Phase 1e's WiFi allocations
 * were not. The base is therefore aligned here rather than trusted from the caller, and
 * scratch_bytes() carries the padding that guarantees room to do it. */
#define ACOUSTIC_SCRATCH_ALIGN 64u

static size_t align_scratch(size_t v)
{
    return (v + (ACOUSTIC_SCRATCH_ALIGN - 1u)) & ~(size_t)(ACOUSTIC_SCRATCH_ALIGN - 1u);
}

static uint32_t capture_words(const truing_chain_profile_t *c)
{
    const float fs = (float)c->sample_rate_hz;
    return (uint32_t)((c->pre_trigger_ms + c->capture_ms) * 1e-3f * fs + 0.5f);
}

static uint32_t max_window_samples(const truing_chain_profile_t *c)
{
    return (uint32_t)(c->window_ms * 1e-3f * (float)c->sample_rate_hz + 0.5f) + 1u;
}

static uint32_t onset_frames(const truing_chain_profile_t *c, uint32_t n)
{
    const float fs = (float)c->sample_rate_hz;
    uint32_t frame = (uint32_t)(c->onset_frame_ms * 1e-3f * fs + 0.5f), hop = (uint32_t)(c->onset_hop_ms * 1e-3f * fs + 0.5f);
    if (frame == 0u) frame = 1u;
    if (hop == 0u) hop = 1u;
    return n >= frame ? 1u + (n - frame) / hop : 0u;
}

size_t truing_acoustic_real_scratch_bytes(const truing_chain_profile_t *chain)
{
    if (chain == NULL) {
        return 0u;
    }
    const uint32_t n = capture_words(chain);
    const size_t dsp = truing_dsp_workspace_bytes(max_window_samples(chain), chain->zero_pad_factor);
    if (dsp == 0u) {
        return 0u;
    }
    return align8((size_t)n * sizeof(int32_t)) + align8((size_t)n * sizeof(float)) +
           align8((size_t)(onset_frames(chain, n) + 1u) * sizeof(float)) + align8(dsp) +
           (ACOUSTIC_SCRATCH_ALIGN - 1u);
}

/* ---- the measurement ------------------------------------------------------------------ */
static void fill_rejected(truing_tension_estimate_t *out, truing_reason_t reason, uint8_t cycle_index, uint32_t now,
                          truing_source_impl_t src, const truing_tension_model_profile_t *profile)
{
    truing_hal_fill_unavailable_estimate(out, reason, cycle_index, now, src);
    out->meta.status = TRUING_STATUS_REJECTED;
    out->frequency.meta.status = TRUING_STATUS_REJECTED;
    out->frequency.meta.reason_code = reason;
    if (profile != NULL) {
        out->model_name = profile->model_name;
        out->model_version = profile->model_version;
    }
}

static void analyze(truing_acoustic_if_t *self, truing_acoustic_real_ctx_t *c, uint32_t n_words, uint8_t cycle_index,
                    uint8_t spoke_index, truing_tension_estimate_t *out)
{
    const uint32_t now = truing_clock_now_ms(&c->clock);
    const truing_source_impl_t src = self->source_impl;
    truing_acoustic_real_diag_t *d = &c->diag;
    /* Layer 1 output -> float, the research reference's scale. */
    for (uint32_t i = 0; i < n_words; ++i) {
        c->samples[i] = truing_audio_word_to_float(c->words[i]);
    }
    /* Onset (S3.1). */
    truing_onset_result_t on;
    if (!truing_onset_detect(c->samples, n_words, &c->params, c->onset_env, c->onset_env_cap, &on) || on.n_onsets == 0u) {
        d->onsets = on.n_onsets;
        d->onset_threshold = on.threshold;
        c->rejections++;
        fill_rejected(out, TRUING_REASON_NO_ONSET_DETECTED, cycle_index, now, src, c->profile);
        return;
    }
    d->onsets = on.n_onsets;
    d->onset_sample = on.onset_sample[0];
    d->onset_threshold = on.threshold;
    /* The honest moment the pluck is known to have landed. It is only knowable here, at the end
     * of the window: onset detection runs over the finished capture, so the station learns its
     * pluck was heard AFTER the window closed, never during it. */
    emit_phase(c, cycle_index, TRUING_ACOUSTIC_PHASE_ONSET_DETECTED, spoke_index, 0u);
    if (c->cancel_requested) {
        c->cancel_requested = false;
        truing_hal_fill_unavailable_estimate(out, TRUING_REASON_CANCELLED, cycle_index, now, src);
        return;
    }
    /* Window (S3.2): the first onset in the capture is the excitation; a second one bounds it. */
    const int64_t next = on.n_onsets > 1u ? (int64_t)on.onset_sample[1] : -1;
    truing_dsp_window_t w;
    if (!truing_dsp_select_window(&c->dsp, c->samples, n_words, (int64_t)on.onset_sample[0], next, &c->params, &w)) {
        c->rejections++;
        fill_rejected(out, TRUING_REASON_VALUE_OUT_OF_RANGE, cycle_index, now, src, c->profile);   /* too little signal survives gating */
        return;
    }
    d->window = w;
    /* The spectral analysis proper. NOT the whole of the cost: measured on the Nano, the
     * envelope and window selection above already spent ~1.75 s, against ~1.2 s for what
     * follows. The boundary is still worth marking - it is the last thing before the FFT - but
     * ONSET_DETECTED is what tells the station to stop plucking, and it is the frame that
     * matters. Nothing here re-opens the window. */
    emit_phase(c, cycle_index, TRUING_ACOUSTIC_PHASE_ANALYZING, spoke_index, 0u);
    /* Layers 2 + interim 3. */
    truing_dsp_event_t ev;
    if (!truing_dsp_analyze_window(&c->dsp, c->samples, &w, &c->params, &ev)) {
        c->rejections++;
        fill_rejected(out, TRUING_REASON_VALUE_OUT_OF_RANGE, cycle_index, now, src, c->profile);
        return;
    }
    d->n_fft = ev.n_fft;
    d->n_strong_peaks = ev.n_strong_peaks;
    d->n_peaks_in_band = ev.n_peaks_in_band;
    d->f1_hz = ev.f1_found ? ev.f1.freq_hz : NAN;
    d->f2_hz = ev.f2_found ? ev.f2.freq_hz : NAN;
    d->snr_db = ev.snr_db;
    if (ev.peaks_overflow) {
        c->rejections++;
        fill_rejected(out, TRUING_REASON_AMBIGUOUS_PEAK, cycle_index, now, src, c->profile);
        return;
    }
    if (ev.n_strong_peaks == 0u) {
        c->rejections++;
        fill_rejected(out, TRUING_REASON_LOW_SNR, cycle_index, now, src, c->profile);
        return;
    }
    if (!ev.f1_found) {
        c->rejections++;
        fill_rejected(out, TRUING_REASON_FREQ_OUT_OF_RANGE, cycle_index, now, src, c->profile);
        return;
    }
    if (!isfinite(ev.snr_db) || ev.snr_db < c->chain->measurement_min_snr_db) {
        c->rejections++;
        fill_rejected(out, TRUING_REASON_LOW_SNR, cycle_index, now, src, c->profile);
        return;
    }
    /* The frequency measurement record (SPEC 6.3): a SELECTION with provisional identity. */
    memset(out, 0, sizeof(*out));
    truing_frequency_measurement_t *f = &out->frequency;
    f->meta.status = TRUING_STATUS_SUSPECT;
    f->meta.reason_code = TRUING_REASON_PROVISIONAL_MODE_ID;
    f->meta.cycle_index = cycle_index;
    f->meta.timestamp_ms = now;
    f->meta.source_impl = src;
    f->selected_frequency_hz = ev.f1.freq_hz;
    f->mode_identity = TRUING_MODE_ID_PRESUMED_FUNDAMENTAL;
    f->n_candidates = ev.n_candidates;
    for (uint8_t i = 0; i < ev.n_candidates && i < TRUING_MAX_CANDIDATE_PEAKS; ++i) {
        f->candidates[i].frequency_hz = ev.candidates[i].freq_hz;
        f->candidates[i].magnitude_db = ev.candidates[i].magnitude_db;
        f->candidates[i].prominence_db = ev.candidates[i].prominence_db;
    }
    f->snr_db = ev.snr_db;
    f->selection_rule_version = TRUING_DSP_SELECTION_RULE_VERSION;
    /* Layer 4: the session-fixed model. */
    out->meta = f->meta;
    out->model_name = c->profile != NULL ? c->profile->model_name : TRUING_TENSION_MODEL_UNSET;
    out->model_version = c->profile != NULL ? c->profile->model_version : 0u;
    out->sigma_n = NAN;   /* repeat-scatter propagation needs repeated plucks (research S4.4); one pluck has none */
    const truing_tension_result_t t = truing_tension_model_apply(c->profile, ev.f1.freq_hz, ev.f2_found ? ev.f2.freq_hz : NAN);
    d->l_eff_m = t.l_eff_m;
    if (!t.ok) {
        c->rejections++;
        out->tension_n = NAN;
        if (t.reason == TRUING_REASON_CALIBRATION_MISSING) {
            out->meta.status = TRUING_STATUS_UNAVAILABLE;
            f->meta.status = TRUING_STATUS_UNAVAILABLE;
        } else {
            out->meta.status = TRUING_STATUS_REJECTED;
            f->meta.status = TRUING_STATUS_REJECTED;
        }
        out->meta.reason_code = t.reason;
        f->meta.reason_code = t.reason;
        return;
    }
    out->tension_n = t.tension_n;
    out->meta.status = TRUING_STATUS_SUSPECT;
    out->meta.reason_code = TRUING_REASON_PROVISIONAL_MODE_ID;   /* SPEC 4.4.1: never valid while layer 3 is interim */
    c->estimates++;
}

static void measure_run(truing_acoustic_if_t *self, uint8_t spoke_id, const truing_wheel_class_config_t *wheel_geometry,
                        uint8_t cycle_index, truing_tension_estimate_t *out)
{
    (void)wheel_geometry;   /* a damping ritual targeting a neighbour would resolve it here (SPEC 9.1); none is implemented */
    truing_acoustic_real_ctx_t *c = (truing_acoustic_real_ctx_t *)self->ctx;
    if (out == NULL) {
        return;
    }
    if (c == NULL || !c->source_open) {
        truing_hal_fill_unavailable_estimate(out, TRUING_REASON_NOT_IMPLEMENTED, cycle_index, 0u, self->source_impl);
        return;
    }
    c->calls++;
    /* Consecutive calls for the same spoke in the same cycle are the orchestrator's retries
     * (SPEC §7.4), so this is the attempt number the station cares about. Any other spoke, or a
     * new cycle, starts again at one. */
    if (c->attempt_valid && c->attempt_spoke == spoke_id && c->attempt_cycle == cycle_index) {
        c->attempt++;
    } else {
        c->attempt = 1u;
        c->attempt_spoke = spoke_id;
        c->attempt_cycle = cycle_index;
        c->attempt_valid = true;
    }
    /* pluck_commanded is decided for THIS attempt below (fire at the excitation seam); the rest
     * of diag still describes the previous capture. Wiping the whole struct here destroyed that
     * evidence - diag.n_captured -> 0 makes truing_acoustic_real_last_capture refuse, i.e. the
     * /debug/capture dump 404s - ~3.7 s before the new capture replaces the samples, which is
     * how a session loses exactly the attempt a slow fetch came for. The reset lives next to
     * capture_seq++ below: nothing may invalidate evidence of the last measurement before the
     * thing that replaces it exists (same rule real_reset_session applies at a boundary). */
    c->diag.pluck_commanded = false;
    const uint32_t now = truing_clock_now_ms(&c->clock);
    if (!c->profile_ok) {
        /* SPEC 11.3.1: defensive behaviour when invoked despite admission being bypassed. */
        truing_hal_fill_unavailable_estimate(out, TRUING_REASON_CALIBRATION_MISSING, cycle_index, now, self->source_impl);
        return;
    }
    if (c->cancel_requested) {
        c->cancel_requested = false;
        truing_hal_fill_unavailable_estimate(out, TRUING_REASON_CANCELLED, cycle_index, now, self->source_impl);
        return;
    }
    /* Excitation: commanded when an actuator is attached; otherwise the capture records whatever
     * excitation arrives (a hand pluck at the station), and NO_ONSET_DETECTED says when none did. */
    if (c->pluck != NULL && c->pluck->available != NULL && c->pluck->available(c->pluck) && c->chain->excitation_pulse_ms > 0.0f) {
        c->diag.pluck_commanded = c->pluck->fire(c->pluck, c->chain->excitation_pulse_ms);
    }
    /* A person at the station otherwise gets no warning at all: the window is fixed-length and
     * cannot end early on a pluck, so a cue arriving with LISTENING is already racing the ~1 s
     * close. ARMED gives them a lead-in. Skipped when an actuator did the excitation (nobody to
     * count in) and when no lead is configured (host tests, replay: timing unchanged). */
    if (c->pluck_lead_ms > 0u && c->delay_fn != NULL && !c->diag.pluck_commanded) {
        emit_phase(c, cycle_index, TRUING_ACOUSTIC_PHASE_ARMED, spoke_id, c->pluck_lead_ms);
        c->delay_fn(c->delay_ctx, c->pluck_lead_ms);
        if (c->cancel_requested) {
            c->cancel_requested = false;
            truing_hal_fill_unavailable_estimate(out, TRUING_REASON_CANCELLED, cycle_index, now, self->source_impl);
            return;
        }
    }
    /* Last thing before the window opens, so the cue reaches the station while it is still open
     * rather than describing something already over. Emitted AFTER any actuator was commanded,
     * so pluck_commanded on this frame is the truth for this attempt. */
    emit_phase(c, cycle_index, TRUING_ACOUSTIC_PHASE_LISTENING, spoke_id, listen_window_ms(c->chain));
    const uint32_t t0 = truing_clock_now_ms(&c->clock);
    uint32_t got = 0u;
    /* The previous capture is about to be replaced, so its diagnostics may be reset only now -
     * after every early return above has had its chance to leave the evidence standing. The
     * commanded flag survives the reset: the LISTENING frame above already reported it and the
     * capture dump must say the same thing about the same attempt. */
    const bool pluck_commanded = c->diag.pluck_commanded;
    memset(&c->diag, 0, sizeof(c->diag));
    c->diag.f1_hz = NAN;
    c->diag.f2_hz = NAN;
    c->diag.snr_db = NAN;
    c->diag.l_eff_m = NAN;
    c->diag.pluck_commanded = pluck_commanded;
    c->capture_seq++;   /* the buffer is about to change under any reader (SPEC §12.5) */
    const truing_audio_result_t r = c->source->capture(c->source, c->words, c->n_capture, &c->cancel_requested, &got);
    c->diag.capture_result = r;
    c->diag.n_captured = got;
    c->diag.capture_us = (truing_clock_now_ms(&c->clock) - t0) * 1000u;
    if (r == TRUING_AUDIO_ERR_CANCELLED || (c->cancel_requested)) {
        c->cancel_requested = false;
        truing_hal_fill_unavailable_estimate(out, TRUING_REASON_CANCELLED, cycle_index, now, self->source_impl);
        return;
    }
    if (r == TRUING_AUDIO_ERR_OVERRUN) {
        /* SPEC 9.4: a dropped-sample discontinuity corrupts the spectrum; the capture is discarded. */
        c->rejections++;
        fill_rejected(out, TRUING_REASON_CAPTURE_OVERRUN, cycle_index, now, self->source_impl, c->profile);
        return;
    }
    if (r != TRUING_AUDIO_OK || got == 0u) {
        truing_hal_fill_unavailable_estimate(out, TRUING_REASON_SENSOR_TIMEOUT, cycle_index, now, self->source_impl);
        return;
    }
    const uint32_t t1 = truing_clock_now_ms(&c->clock);
    analyze(self, c, got, cycle_index, spoke_id, out);
    c->diag.analysis_us = (truing_clock_now_ms(&c->clock) - t1) * 1000u;
}

/* Records which measurement produced the words now sitting in the capture buffer, so a dump
 * can say what it is evidence OF. Purely a record of what already happened: measure_run() has
 * returned, `out` is final, and nothing downstream reads any of this (SPEC §13.3). real_measure
 * calls it only when the attempt reached the capture point (seq moved) AND delivered words
 * (n_captured > 0): an attempt that returned early or captured nothing must not relabel the
 * capture still standing in the buffer - that identity belongs to the attempt that captured
 * the words the buffer holds. */
static void note_outcome(truing_acoustic_real_ctx_t *c, uint8_t spoke_id, uint8_t cycle_index,
                         const truing_tension_estimate_t *out)
{
    if (c == NULL || out == NULL || c->diag.n_captured == 0u) {
        return;
    }
    c->last_spoke = spoke_id;
    c->last_cycle = cycle_index;
    c->last_attempt = c->attempt;
    c->last_status = out->meta.status;
    c->last_reason = out->meta.reason_code;
}

static void real_measure(truing_acoustic_if_t *self, uint8_t spoke_id, const truing_wheel_class_config_t *wheel_geometry,
                         uint8_t cycle_index, truing_tension_estimate_t *out)
{
    truing_acoustic_real_ctx_t *const c = (truing_acoustic_real_ctx_t *)self->ctx;
    const uint32_t seq_before = (c != NULL) ? c->capture_seq : 0u;
    measure_run(self, spoke_id, wheel_geometry, cycle_index, out);
    if (out != NULL && c != NULL && c->capture_seq != seq_before) {
        note_outcome(c, spoke_id, cycle_index, out);
    }
}

bool truing_acoustic_real_last_capture(const truing_acoustic_if_t *self, truing_acoustic_capture_view_t *out)
{
    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (self == NULL || self->ctx == NULL || self->measure_spoke_tension != real_measure) {
        return false;
    }
    const truing_acoustic_real_ctx_t *c = (const truing_acoustic_real_ctx_t *)self->ctx;
    if (c->capture_seq == 0u || c->words == NULL || c->diag.n_captured == 0u) {
        return false;
    }
    out->words = c->words;
    out->n_words = c->diag.n_captured;
    out->seq = c->capture_seq;
    out->spoke_id = c->last_spoke;
    out->cycle_index = c->last_cycle;
    out->attempt = c->last_attempt;
    out->status = c->last_status;
    out->reason = c->last_reason;
    out->diag = c->diag;
    return true;
}

uint32_t truing_acoustic_real_capture_seq(const truing_acoustic_if_t *self)
{
    if (self == NULL || self->ctx == NULL || self->measure_spoke_tension != real_measure) {
        return 0u;
    }
    return ((const truing_acoustic_real_ctx_t *)self->ctx)->capture_seq;
}

void truing_acoustic_real_set_observer(truing_acoustic_if_t *self, truing_acoustic_observer_fn fn, void *observer_ctx)
{
    if (self == NULL || self->ctx == NULL) {
        return;
    }
    truing_acoustic_real_ctx_t *c = (truing_acoustic_real_ctx_t *)self->ctx;
    c->observer = fn;
    c->observer_ctx = observer_ctx;
}

void truing_acoustic_real_set_pluck_lead(truing_acoustic_if_t *self, uint32_t lead_ms,
                                         void (*delay_fn)(void *ctx, uint32_t ms), void *delay_ctx)
{
    if (self == NULL || self->ctx == NULL) {
        return;
    }
    truing_acoustic_real_ctx_t *c = (truing_acoustic_real_ctx_t *)self->ctx;
    c->pluck_lead_ms = lead_ms;
    c->delay_fn = delay_fn;
    c->delay_ctx = delay_ctx;
}

static void real_cancel(truing_acoustic_if_t *self)
{
    truing_acoustic_real_ctx_t *c = (truing_acoustic_real_ctx_t *)self->ctx;
    if (c != NULL) {
        c->cancel_requested = true;
    }
}

static void real_reset_session(truing_acoustic_if_t *self)
{
    truing_acoustic_real_ctx_t *c = (truing_acoustic_real_ctx_t *)self->ctx;
    if (c == NULL) {
        return;
    }
    /* An ABORT at a positioning wait sets cancel_requested with no measurement in flight to
     * consume it; left alone it makes the next session's first spoke return CANCELLED before
     * the LISTENING cue is even emitted (SPEC §7.4 / §12.3). */
    c->cancel_requested = false;
    /* Attempt numbering keys on spoke+cycle and both repeat across sessions, so a stale
     * attempt_valid would show the new session's first pluck as "attempt 2". */
    c->attempt_valid = false;
    /* The capture buffer, its seq and the last-outcome fields are evidence of the last
     * measurement (SPEC §12.5) and deliberately survive the boundary. */
}

void truing_acoustic_real_analyze_words(truing_acoustic_if_t *self, const int32_t *words, uint32_t n_words,
                                        uint8_t cycle_index, truing_tension_estimate_t *out)
{
    truing_acoustic_real_ctx_t *c = (truing_acoustic_real_ctx_t *)self->ctx;
    if (out == NULL) {
        return;
    }
    if (c == NULL || words == NULL) {
        truing_hal_fill_unavailable_estimate(out, TRUING_REASON_NOT_IMPLEMENTED, cycle_index, 0u, self->source_impl);
        return;
    }
    c->calls++;
    if (!c->profile_ok) {
        /* Same rule as measure_run: an early return leaves the standing capture's evidence
         * intact - the diag reset waits until the buffer is actually about to change. */
        truing_hal_fill_unavailable_estimate(out, TRUING_REASON_CALIBRATION_MISSING, cycle_index, truing_clock_now_ms(&c->clock), self->source_impl);
        return;
    }
    const uint32_t n = n_words < c->n_capture ? n_words : c->n_capture;
    memset(&c->diag, 0, sizeof(c->diag));
    c->diag.f1_hz = NAN;
    c->diag.f2_hz = NAN;
    c->diag.snr_db = NAN;
    c->diag.l_eff_m = NAN;
    c->capture_seq++;   /* replay overwrites the same buffer a dump would be reading */
    memcpy(c->words, words, (size_t)n * sizeof(int32_t));
    c->diag.n_captured = n;
    c->diag.capture_result = TRUING_AUDIO_OK;
    const uint32_t t1 = truing_clock_now_ms(&c->clock);
    /* No spoke and no window: this path replays words that were captured elsewhere. */
    analyze(self, c, n, cycle_index, 0u, out);
    c->diag.analysis_us = (truing_clock_now_ms(&c->clock) - t1) * 1000u;
    note_outcome(c, 0u, cycle_index, out);
}

bool truing_acoustic_real_init(truing_acoustic_if_t *self, truing_acoustic_real_ctx_t *ctx, truing_clock_if_t clock,
                               const truing_chain_profile_t *chain, const truing_tension_model_profile_t *profile,
                               truing_audio_source_if_t *source, truing_pluck_if_t *pluck, void *scratch, size_t scratch_bytes,
                               const char **detail)
{
    if (detail != NULL) *detail = "";
    if (self == NULL || ctx == NULL) {
        return false;
    }
    memset(ctx, 0, sizeof(*ctx));
    self->impl_name = "acoustic_real";
    self->source_impl = TRUING_SOURCE_REAL;
    self->measure_spoke_tension = real_measure;
    self->request_cancel = real_cancel;
    self->reset_session = real_reset_session;
    self->ctx = ctx;
    ctx->clock = clock;
    ctx->chain = chain;
    ctx->profile = profile;
    ctx->source = source;
    ctx->pluck = pluck;
    const char *field = NULL;
    if (chain == NULL || truing_chain_profile_check(chain, &field) != TRUING_CFG_OK) {
        if (detail != NULL) *detail = "chain profile invalid";
        return false;
    }
    if (!truing_dsp_params_from_chain(chain, &ctx->params)) {
        if (detail != NULL) *detail = "chain profile not usable as DSP parameters";
        return false;
    }
    if (source == NULL || source->open == NULL || source->capture == NULL) {
        if (detail != NULL) *detail = "no audio source";
        return false;
    }
    truing_audio_format_t fmt;
    memset(&fmt, 0, sizeof(fmt));
    if (source->format != NULL) source->format(source, &fmt);
    if (!truing_audio_format_matches_system(&fmt) || source->open(source) != TRUING_AUDIO_OK) {
        if (detail != NULL) *detail = "audio source cannot deliver 48 kHz / 24-bit in 32-bit slots (refused, not resampled)";
        return false;
    }
    /* The subsystem's provenance follows its front end: a recorded or synthetic source makes the
     * whole estimate non-real even though layers 2-4 are the real code (SPEC 6.2). */
    self->source_impl = source->source_impl == TRUING_SOURCE_REAL ? TRUING_SOURCE_REAL : source->source_impl;
    const size_t need = truing_acoustic_real_scratch_bytes(chain);
    if (scratch == NULL || need == 0u || scratch_bytes < need) {
        if (detail != NULL) *detail = "scratch too small";
        source->close(source);
        return false;
    }
    const uint32_t n = capture_words(chain);
    /* Start on a cache line whatever the caller's allocator returned; scratch_bytes() reserves
     * the lead-in this can consume. */
    uint8_t *p = (uint8_t *)(uintptr_t)align_scratch((size_t)(uintptr_t)scratch);
    ctx->words = (int32_t *)p;    p += align8((size_t)n * sizeof(int32_t));
    ctx->samples = (float *)p;    p += align8((size_t)n * sizeof(float));
    ctx->onset_env_cap = onset_frames(chain, n) + 1u;
    ctx->onset_env = (float *)p;  p += align8((size_t)ctx->onset_env_cap * sizeof(float));
    const size_t dsp_bytes = truing_dsp_workspace_bytes(max_window_samples(chain), chain->zero_pad_factor);
    if (!truing_dsp_workspace_init(&ctx->dsp, max_window_samples(chain), chain->zero_pad_factor, p, dsp_bytes)) {
        if (detail != NULL) *detail = "dsp workspace";
        source->close(source);
        return false;
    }
    ctx->n_capture = n;
    ctx->source_open = true;
    uint32_t missing = 0u;
    ctx->profile_ok = profile != NULL && truing_tension_model_profile_check(profile, &missing, &field) == TRUING_CFG_OK;
    return true;
}
