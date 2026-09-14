#include "bringup_acoustic.h"

#include "net_transport.h"

#include <inttypes.h>
#include <math.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "audio_i2s.h"
#include "board/board_profile.h"
#include "pluck_gpio.h"
#include "truing_fixtures/acoustic_golden.h"
#include "truing_fixtures/fixtures.h"
#include "truing_hal/acoustic_real.h"
#include "truing_hal/audio_source_if.h"
#include "truing_hal/clock_if.h"

static const char *TAG = "bringup";

static void check(int *pass, int *fail, bool ok, const char *what)
{
    if (ok) {
        (*pass)++;
        ESP_LOGI(TAG, "  PASS  %s", what);
    } else {
        (*fail)++;
        ESP_LOGE(TAG, "  FAIL  %s", what);
    }
}

static uint32_t boot_clock_now(void *ctx)
{
    (void)ctx;
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static volatile bool s_load_stop;
static volatile uint32_t s_load_iterations;
static void load_task(void *arg)
{
    (void)arg;
    /* A compute-bound competitor on the audio core at the DSP priority: it must not starve the drain. */
    volatile float acc = 0.0f;
    while (!s_load_stop) {
        for (int i = 0; i < 20000; ++i) acc += sinf((float)i * 0.001f);
        s_load_iterations++;
        taskYIELD();
    }
    vTaskDelete(NULL);
}

void truing_bringup_acoustic_section(int *pass, int *fail, bool *ok_out)
{
    const int fail0 = *fail;
    ESP_LOGI(TAG, "-- acoustic subsystem (Phase 1f): I2S front end, layers 2-4 on target --");
    truing_chain_profile_t chain;
    truing_excitation_profile_t excitation;
    truing_tension_model_profile_t profile;
    truing_wheel_class_config_t wheel;
    truing_fixture_chain_profile_inmp441(&chain);
    truing_fixture_excitation_profile(&excitation);
    truing_fixture_tension_model_profile_complete(&profile);
    truing_fixture_wheel_class_sym32(&wheel);
    truing_clock_if_t clock = { boot_clock_now, NULL };

    /* ---- layer 1: the real front end ---------------------------------------------------- */
    truing_audio_source_if_t i2s;
    truing_audio_i2s_config_t icfg = {
        .dma_frame_num = 240u,      /* 5 ms per descriptor, multiple of 3, 960 B <= 4092 B (SPEC 9.4.1) */
        .dma_desc_num = 8u,         /* 40 ms of driver buffering against a 100 ms worst-case drain gap */
        .pre_trigger_words = (uint32_t)(chain.pre_trigger_ms * 48.0f),
        .ring_words = 48000u,
    };
    const char *detail = NULL;
    const bool i2s_ok = truing_audio_i2s_init(&i2s, &icfg, &detail);
    check(pass, fail, i2s_ok, "I2S front end: channel created, drain task running on core 1 (SPEC 9.4)");
    if (!i2s_ok) {
        ESP_LOGE(TAG, "  front end: %s", detail);
    }
    if (i2s_ok) {
        vTaskDelay(pdMS_TO_TICKS(300));   /* let the ring fill past the pre-trigger depth */
        const uint32_t n = 9600u;         /* 200 ms */
        int32_t *buf = heap_caps_malloc(n * sizeof(int32_t), MALLOC_CAP_SPIRAM);
        uint32_t got = 0u;
        volatile bool no_cancel = false;
        const int64_t t0 = esp_timer_get_time();
        const truing_audio_result_t r = i2s.capture(&i2s, buf, n, &no_cancel, &got);
        const int64_t dt = esp_timer_get_time() - t0;
        double sum = 0.0, sumsq = 0.0;
        uint32_t nonzero = 0u;
        int32_t vmin = 0, vmax = 0;
        for (uint32_t i = 0; i < got; ++i) {
            const int32_t s24 = buf[i] >> 8;
            if (s24 != 0) nonzero++;
            if (s24 < vmin) vmin = s24;
            if (s24 > vmax) vmax = s24;
            const double v = (double)s24 / 8388608.0;
            sum += v;
            sumsq += v * v;
        }
        const double mean = got ? sum / got : 0.0;
        const double rms = got ? sqrt(sumsq / got) : 0.0;
        check(pass, fail, r == TRUING_AUDIO_OK && got == n, "200 ms capture completes with the pre-trigger tail from the ring");
        ESP_LOGI(TAG, "capture: %s, %" PRIu32 " words in %lld ms (pre-trigger %" PRIu32 " words) | nonzero %" PRIu32 " | min %ld max %ld | "
                      "dc %.2e | rms %.2e (%.1f dBFS)",
                 truing_audio_result_str(r), got, (long long)(dt / 1000), icfg.pre_trigger_words, nonzero, (long)vmin, (long)vmax,
                 mean, rms, rms > 0.0 ? 20.0 * log10(rms) : -999.0);
        if (nonzero == 0u) {
            ESP_LOGW(TAG, "  every captured word is zero: no microphone signal on GPIO %d. The transducer is NOT verified by this run.",
                     BOARD_I2S_MIC_DIN_GPIO);
        } else if (nonzero < got / 2u || vmin == vmax) {
            ESP_LOGW(TAG, "  the capture is not a plausible microphone signal (constant or mostly zero). Transducer NOT verified.");
        } else {
            ESP_LOGI(TAG, "  a live signal is present; whether it is an INMP441 at a spoke is a matter of the bench, not this check.");
        }
        heap_caps_free(buf);
    }

    /* ---- layers 2-4 on the embedded recorded excerpt: host/firmware parity on target ------- */
    const size_t scratch_bytes = truing_acoustic_real_scratch_bytes(&chain);
    void *scratch = heap_caps_malloc(scratch_bytes, MALLOC_CAP_SPIRAM);
    check(pass, fail, scratch != NULL, "acoustic scratch (capture buffers + DSP workspace) allocated in PSRAM");
    ESP_LOGI(TAG, "acoustic scratch: %u bytes (window %.0f ms x zero-pad %.0f -> %" PRIu32 "-point transform; Hilbert via Bluestein)",
             (unsigned)scratch_bytes, (double)chain.window_ms, (double)chain.zero_pad_factor,
             truing_spectrum_n_fft((uint32_t)(chain.window_ms * 48.0f) + 1u, chain.zero_pad_factor));
    if (scratch != NULL) {
        truing_audio_source_if_t rec;
        truing_audio_buffer_ctx_t rctx;
        truing_audio_buffer_init(&rec, &rctx, acoustic_golden_cases[0].pcm, acoustic_golden_cases[0].n_samples, NULL);
        truing_acoustic_if_t a;
        truing_acoustic_real_ctx_t actx;
        const bool init_ok = truing_acoustic_real_init(&a, &actx, clock, &chain, &excitation, &profile, &rec, NULL, scratch, scratch_bytes, &detail);
        check(pass, fail, init_ok, "acoustic subsystem initialises on a RECORDED front end (provenance follows the source)");
        if (init_ok) {
            bool all_suspect = true, all_segmented = true, all_matched = true, all_model = true;
            float worst_df1 = 0.0f, worst_dsnr = 0.0f;
            uint32_t worst_analysis_ms = 0u;
            for (unsigned i = 0; i < ACOUSTIC_GOLDEN_N_CASES; ++i) {
                /* Each analysis holds core 0 for seconds; without a yield between them the
                 * idle task never runs and the watchdog reports work that is behaving
                 * exactly as designed. Costs one tick against several seconds. */
                vTaskDelay(1);
                const acoustic_golden_case_t *g = &acoustic_golden_cases[i];
                truing_tension_estimate_t e;
                const int64_t t0 = esp_timer_get_time();
                truing_acoustic_real_analyze_words(&a, g->pcm, g->n_samples, 1u, &e);
                const uint32_t ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
                if (ms > worst_analysis_ms) worst_analysis_ms = ms;
                all_suspect = all_suspect && e.meta.status == TRUING_STATUS_SUSPECT &&
                              e.meta.reason_code == TRUING_REASON_PROVISIONAL_MODE_ID &&
                              e.frequency.mode_identity == TRUING_MODE_ID_PRESUMED_FUNDAMENTAL;
                all_segmented = all_segmented && actx.diag.onset_sample == (uint32_t)g->onset_sample &&
                                actx.diag.window.start_sample == (uint32_t)g->window_start_sample &&
                                actx.diag.window.n_samples == (uint32_t)g->window_n_samples &&
                                actx.diag.n_fft == (uint32_t)g->n_fft;
                const float df1 = fabsf(e.frequency.selected_frequency_hz - g->f1_hz);
                const float dsnr = fabsf(e.frequency.snr_db - g->snr_f1_db);
                if (df1 > worst_df1) worst_df1 = df1;
                if (dsnr > worst_dsnr) worst_dsnr = dsnr;
                all_matched = all_matched && df1 <= 0.05f && dsnr <= 0.2f && (int)e.frequency.n_candidates == g->n_candidates &&
                              (int)actx.diag.n_strong_peaks == g->n_strong_peaks;
                all_model = all_model && fabsf(e.tension_n - g->m0_tension_n) <= 0.5f;
                ESP_LOGI(TAG, "  %-8s f1 %.3f Hz (ref %.3f, d %.4f) snr %.2f dB (ref %.2f) candidates %u strong %" PRIu32
                              " | T(M0) %.1f N (ref %.1f) | %" PRIu32 " ms",
                         g->name, (double)e.frequency.selected_frequency_hz, (double)g->f1_hz, (double)df1,
                         (double)e.frequency.snr_db, (double)g->snr_f1_db, (unsigned)e.frequency.n_candidates,
                         actx.diag.n_strong_peaks, (double)e.tension_n, (double)g->m0_tension_n, ms);
            }
            check(pass, fail, all_suspect, "every recorded pluck -> suspect / PROVISIONAL_MODE_ID / presumed_fundamental (SPEC 4.4.1)");
            check(pass, fail, all_segmented, "onset, gate, window and transform length identical to the Python reference");
            check(pass, fail, all_matched, "f1, SNR, candidate and strong-peak counts match the reference (|df1| <= 0.05 Hz, |dSNR| <= 0.2 dB)");
            check(pass, fail, all_model, "ideal-string tension equals the reference closed form");
            ESP_LOGI(TAG, "recorded-pluck parity on target: %u plucks, worst |df1| %.4f Hz, worst |dSNR| %.3f dB, worst analysis %" PRIu32 " ms",
                     (unsigned)ACOUSTIC_GOLDEN_N_CASES, (double)worst_df1, (double)worst_dsnr, worst_analysis_ms);
            ESP_LOGW(TAG, "  the tensiometer read 1332.8 N for the ts00 pluck; the fixture profile's L_eff = crossing distance is "
                          "SYNTHETIC content and yields %.0f N: the effective-length question of SPEC 4.4.1 is open, not answered here",
                     (double)acoustic_golden_cases[0].m0_tension_n);
            /* ---- stage profile: where the per-pluck time actually goes on this silicon ----
             * Calls the DSP primitives directly on the golden window so each stage is timed in
             * isolation. Kept in the build: it is the regression evidence for any optimisation. */
            {
                const acoustic_golden_case_t *g = &acoustic_golden_cases[0];
                for (uint32_t i = 0; i < g->n_samples; ++i) {
                    actx.samples[i] = truing_audio_word_to_float(g->pcm[i]);
                }
                const uint32_t wstart = (uint32_t)g->window_start_sample;
                const uint32_t wlen = (uint32_t)g->window_n_samples;
                const uint32_t smooth_w = (uint32_t)(actx.params.decay_smoothing_ms * 1e-3f * actx.params.sample_rate_hz + 0.5f);
                truing_onset_result_t on;
                int64_t t = esp_timer_get_time();
                truing_onset_detect(actx.samples, g->n_samples, &actx.params, actx.onset_env, actx.onset_env_cap, &on);
                const uint32_t us_onset = (uint32_t)(esp_timer_get_time() - t);
                t = esp_timer_get_time();
                truing_envelope_analytic(&actx.dsp.envelope, actx.samples + wstart, wlen, actx.dsp.env);
                const uint32_t us_hilbert = (uint32_t)(esp_timer_get_time() - t);
                t = esp_timer_get_time();
                truing_envelope_smooth(actx.dsp.env, wlen, smooth_w, actx.dsp.env_smooth);
                const uint32_t us_smooth = (uint32_t)(esp_timer_get_time() - t);
                truing_spectrum_t spec;
                t = esp_timer_get_time();
                truing_spectrum_compute(&actx.dsp.spectrum, actx.samples + wstart, wlen, actx.params.sample_rate_hz,
                                        actx.params.zero_pad_factor, &spec);
                const uint32_t us_fft = (uint32_t)(esp_timer_get_time() - t);
                static truing_peak_t s_peaks[TRUING_DSP_MAX_STRONG_PEAKS];
                bool ovf = false;
                t = esp_timer_get_time();
                const uint32_t np = truing_peaks_find(&spec, actx.params.search_band_lo_hz, actx.params.search_band_hi_hz,
                                                      actx.params.prominence_db, actx.params.max_peak_depth_db, s_peaks,
                                                      TRUING_DSP_MAX_STRONG_PEAKS, &ovf);
                const uint32_t us_peaks = (uint32_t)(esp_timer_get_time() - t);
                t = esp_timer_get_time();
                const float snr = truing_peaks_snr_db(&spec, g->f1_hz, g->f1_magnitude_db, actx.params.snr_noise_offset_lo_hz,
                                                      actx.params.snr_noise_offset_hi_hz, actx.dsp.spectrum.windowed, spec.n_fft);
                const uint32_t us_snr = (uint32_t)(esp_timer_get_time() - t);
                ESP_LOGI(TAG, "stage profile (ts00_e2, window %" PRIu32 " samples -> %" PRIu32 " bins, %" PRIu32 " strong peaks, snr %.1f dB):",
                         wlen, spec.n_bins, np, (double)snr);
                ESP_LOGI(TAG, "  onset %" PRIu32 " us | hilbert %" PRIu32 " us | smooth %" PRIu32 " us | fft %" PRIu32 " us | peaks %" PRIu32
                              " us | snr %" PRIu32 " us | total %" PRIu32 " us",
                         us_onset, us_hilbert, us_smooth, us_fft, us_peaks, us_snr,
                         us_onset + us_hilbert + us_smooth + us_fft + us_peaks + us_snr);
                /* Decompose the two transforms: software double trigonometry vs. the butterflies
                 * themselves, so the optimisation targets are measured rather than assumed. */
                volatile double acc = 0.0;
                const double dstep = -3.14159265358979323846 / 131072.0;
                t = esp_timer_get_time();
                for (uint32_t k = 0; k <= 131072u; ++k) {
                    acc += cos(dstep * (double)k) + sin(dstep * (double)k);
                }
                const uint32_t us_trig131k = (uint32_t)(esp_timer_get_time() - t);
                t = esp_timer_get_time();
                truing_fft_complex(&actx.dsp.spectrum.plan, actx.dsp.spectrum.scratch, false);
                const uint32_t us_cfft131k = (uint32_t)(esp_timer_get_time() - t);
                t = esp_timer_get_time();
                truing_fft_complex(&actx.dsp.envelope.plan, actx.dsp.envelope.a, false);
                const uint32_t us_cfft65k = (uint32_t)(esp_timer_get_time() - t);
                ESP_LOGI(TAG, "  micro: 131073 double cos+sin %" PRIu32 " us | complex FFT 131072 pt %" PRIu32 " us | complex FFT 65536 pt %" PRIu32 " us",
                         us_trig131k, us_cfft131k, us_cfft65k);
                ESP_LOGI(TAG, "  => real FFT 262144 pt = window + cfft(%" PRIu32 ") + split(trig %" PRIu32 " + arith) + log-magnitude; "
                              "Hilbert = 2 x [chirp trig + 3 x cfft(%" PRIu32 ")]",
                         us_cfft131k, us_trig131k, us_cfft65k);
            }
            /* the recorded noise floor must never become a tension */
            truing_tension_estimate_t e;
            truing_acoustic_real_analyze_words(&a, ACOUSTIC_GOLDEN_NOISE_PCM, ACOUSTIC_GOLDEN_NOISE_N_SAMPLES, 1u, &e);
            check(pass, fail, e.meta.status == TRUING_STATUS_REJECTED && isnan(e.tension_n),
                  "recorded noise floor -> rejected, no tension (absolute onset floor from the chain profile)");
            ESP_LOGI(TAG, "noise floor on target (%.1f dBFS rms): %s / %s", (double)ACOUSTIC_GOLDEN_NOISE_RMS_DBFS,
                     truing_status_str(e.meta.status), truing_reason_str(e.meta.reason_code));
            rec.close(&rec);
        }
        /* ---- the same subsystem on the real front end: an honest measurement attempt ---------- */
        if (i2s_ok) {
            /* Only a station whose board profile declares its solenoid is ever driven: with
             * *_PRESENT 0 nothing is fired at boot, and that station's live measurement is
             * reported NOT PERFORMED rather than attempted (plan A9). */
            static const int k_present[TRUING_ACOUSTIC_STATIONS] = { BOARD_PLUCK_ACTUATOR_LEFT_PRESENT,
                                                                     BOARD_PLUCK_ACTUATOR_RIGHT_PRESENT };
            static const int k_gpio[TRUING_ACOUSTIC_STATIONS] = { BOARD_PLUCK_ACTUATOR_LEFT_GPIO,
                                                                  BOARD_PLUCK_ACTUATOR_RIGHT_GPIO };
            static const char *const k_name[TRUING_ACOUSTIC_STATIONS] = { "LEFT", "RIGHT" };
            truing_pluck_if_t pluck[TRUING_ACOUSTIC_STATIONS];
            truing_pluck_gpio_ctx_t pctx[TRUING_ACOUSTIC_STATIONS];
            truing_acoustic_actuators_t actuators;
            memset(pctx, 0, sizeof(pctx));
            for (unsigned i = 0; i < TRUING_ACOUSTIC_STATIONS; ++i) {
                actuators.at[i] = NULL;
                if (k_present[i] && truing_pluck_gpio_init(&pluck[i], &pctx[i], k_gpio[i])) {
                    actuators.at[i] = &pluck[i];
                }
            }
            truing_acoustic_if_t a2;
            truing_acoustic_real_ctx_t actx2;
            const bool ok2 = truing_acoustic_real_init(&a2, &actx2, clock, &chain, &excitation, &profile, &i2s, &actuators,
                                                       scratch, scratch_bytes, &detail);
            check(pass, fail, ok2 && a2.source_impl == TRUING_SOURCE_REAL, "acoustic subsystem on the I2S front end is a REAL implementation");
            /* Spokes 0 and 1 cover both stations through the subsystem's own convention; the slot,
             * and so the label, come from truing_acoustic_station_for_spoke() rather than from an
             * assumption that spoke i belongs to slot i. */
            for (uint8_t spoke = 0u; ok2 && spoke < TRUING_ACOUSTIC_STATIONS; ++spoke) {
                const int slot = truing_acoustic_station_slot(truing_acoustic_station_for_spoke(spoke));
                if (slot < 0 || actuators.at[slot] == NULL) {
                    ESP_LOGW(TAG, "live measurement of spoke %u (%s station): NOT PERFORMED (%s)", (unsigned)spoke,
                             slot >= 0 ? k_name[slot] : "?",
                             slot >= 0 && k_present[slot] ? "GPIO init failed" : "no solenoid declared in the board profile");
                    continue;
                }
                truing_tension_estimate_t e;
                const int64_t t0 = esp_timer_get_time();
                truing_acoustic_measure(&a2, spoke, &wheel, 1u, &e);
                const int64_t dt = esp_timer_get_time() - t0;
                ESP_LOGI(TAG, "measure_spoke_tension spoke %u, actuator %s: %s / %s (capture %s, %" PRIu32 " words, pulse %u us) in %lld ms",
                         (unsigned)spoke, truing_acoustic_actuator_str(actx2.diag.station), truing_status_str(e.meta.status),
                         truing_reason_str(e.meta.reason_code), truing_audio_result_str(actx2.diag.capture_result),
                         actx2.diag.n_captured, pctx[slot].last_pulse_us_measured, (long long)(dt / 1000));
                check(pass, fail, e.meta.status != TRUING_STATUS_VALID && (e.meta.status != TRUING_STATUS_SUSPECT || isfinite(e.tension_n)),
                      "live measurement reports a status, never a fabricated valid value (P2/P7)");
            }
            /* ---- drain integrity with a compute task competing on the audio core ------------- */
            truing_audio_i2s_stats_t before, after;
            truing_audio_i2s_stats(&i2s, &before);
            s_load_stop = false;
            s_load_iterations = 0u;
            TaskHandle_t lt = NULL;
            xTaskCreatePinnedToCore(load_task, "audio_load", 4096, NULL, tskIDLE_PRIORITY + 2, &lt, 1);
            const uint32_t n = 48000u;   /* 1 s */
            int32_t *buf = heap_caps_malloc(n * sizeof(int32_t), MALLOC_CAP_SPIRAM);
            uint32_t got = 0u;
            volatile bool no_cancel = false;
            const truing_audio_result_t r = i2s.capture(&i2s, buf, n, &no_cancel, &got);
            s_load_stop = true;
            vTaskDelay(pdMS_TO_TICKS(50));
            truing_audio_i2s_stats(&i2s, &after);
            check(pass, fail, r == TRUING_AUDIO_OK && after.overrun_events == before.overrun_events,
                  "1 s capture with a compute task at DSP priority on core 1: no DMA overrun (priority separation, SPEC 4.5/9.4)");
            ESP_LOGI(TAG, "drain under load: %s, load iterations %" PRIu32 ", reads %" PRIu32 ", max read gap %" PRIu32 " us, overruns %" PRIu32,
                     truing_audio_result_str(r), s_load_iterations, after.reads - before.reads, after.max_read_gap_us, after.overrun_events);

            /* ---- SPEC 9.4: the same capture with the radio busy (deferred from Phase 1f,
             * which had no WiFi stack to load it with) ------------------------------------ */
            if (!truing_net_started()) {
                ESP_LOGW(TAG, "WiFi-load stress test NOT PERFORMED: the transport is not up");
            } else if (!truing_net_rf_load_start()) {
                ESP_LOGW(TAG, "WiFi-load stress test NOT PERFORMED: the driver refused frame injection");
            } else {
                truing_audio_i2s_stats(&i2s, &before);
                s_load_stop = false;
                s_load_iterations = 0u;
                TaskHandle_t lt2 = NULL;
                xTaskCreatePinnedToCore(load_task, "audio_load", 4096, NULL, tskIDLE_PRIORITY + 2, &lt2, 1);
                uint32_t got2 = 0u;
                volatile bool no_cancel2 = false;
                const truing_audio_result_t r2 = i2s.capture(&i2s, buf, n, &no_cancel2, &got2);
                s_load_stop = true;
                uint32_t injected = 0u, inject_fail = 0u;
                truing_net_rf_load_stop(&injected, &inject_fail);
                vTaskDelay(pdMS_TO_TICKS(50));
                truing_audio_i2s_stats(&i2s, &after);
                /* Injection must actually have happened, or the capture was not loaded and
                 * a clean result would mean nothing. */
                check(pass, fail, injected > 0u && r2 == TRUING_AUDIO_OK &&
                                      after.overrun_events == before.overrun_events,
                      "1 s capture with the WiFi radio transmitting and a compute task on the audio core: "
                      "no DMA overrun (SPEC 9.4)");
                ESP_LOGI(TAG, "WiFi-load capture: %s, frames injected %" PRIu32 " (%" PRIu32 " refused), "
                              "reads %" PRIu32 ", max read gap %" PRIu32 " us, overruns %" PRIu32
                              ", stations associated %d",
                         truing_audio_result_str(r2), injected, inject_fail, after.reads - before.reads,
                         after.max_read_gap_us, after.overrun_events, (int)truing_net_client_connected());
                /* Say plainly what this did and did not load. The driver caps management-frame
                 * injection near 90/s however it is driven, so the radio is genuinely active
                 * but the throughput is modest, and with no station associated there is no
                 * TCP path in the picture. The heavier case - an associated browser streaming
                 * telemetry through a measurement cycle - is a bench step, not this check; the
                 * firmware would report it as CAPTURE_OVERRUN either way (SPEC §13.2). */
                ESP_LOGW(TAG, "  scope: radio active at ~%" PRIu32 " frames/s with no station associated. "
                              "A capture under an associated client's traffic is a bench check, not this one.",
                         injected);
            }
            heap_caps_free(buf);
        }
        heap_caps_free(scratch);
    }
    if (i2s_ok) {
        truing_audio_i2s_deinit(&i2s);
    }
    *ok_out = *fail == fail0;
}
