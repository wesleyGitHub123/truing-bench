#include "truing_fixtures/fixtures.h"

#include <math.h>
#include <string.h>

#include "truing/artifact.h"
#include "truing/limits.h"

static void fill_fingerprint(truing_fingerprint_t *fp, uint8_t mul, uint8_t add)
{
    for (unsigned i = 0; i < TRUING_FINGERPRINT_BYTES; ++i) {
        fp->bytes[i] = (uint8_t)(i * mul + add);
    }
    fp->set = true;
}

static const truing_model_assumptions_t k_assumptions_2p0 = {
    .spoke_diameter_mm = 2.0f,
    .spoke_material_modulus_pa = 200.0e9f,
    .spoke_density_kg_m3 = 7850.0f,
};

void truing_fixture_wheel_class_sym32(truing_wheel_class_config_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->n_spokes = 32u;
    out->n_cross = 3u;
    out->rim_diameter_mm = 622.0f;
    out->spoke_diameter_mm = 2.0f;
    out->spoke_material_modulus_pa = 200.0e9f;
    out->asymmetric = false;
    out->side_a_basis = TRUING_SIDE_A_BASIS_DECLARED_ARBITRARY;
    out->hub_side_a.flange_diameter_mm = 45.0f;
    out->hub_side_a.flange_offset_mm = 35.0f;
    out->hub_side_b.flange_diameter_mm = 45.0f;
    out->hub_side_b.flange_offset_mm = 35.0f;
    out->c_side_a_n_per_rev = 473.0f;   /* reference-wheel figure, fixture content only */
    out->c_side_b_n_per_rev = 473.0f;
    /* bike-wheel-calc lace_cross(): spoke 0 is non-drive-side leading -> Side B leading (SPEC §6.5). */
    out->indexing_origin.side = TRUING_SIDE_B;
    out->indexing_origin.lead_trail = TRUING_LEADING;
    out->symmetry_angle_tolerance_rad = 0.01f;
    /* The sym32 fixture wheel IS the wheel the golden influence artifact was generated for: its
     * expected fingerprint is the artifact's generating fingerprint (SPEC 11.4, sole compatibility
     * authority), declared here INDEPENDENTLY of the blob rather than read from it. */
    if (!truing_fingerprint_from_hex(fixture_sym32_artifact_fingerprint_hex, &out->expected_influence_fingerprint)) {
        fill_fingerprint(&out->expected_influence_fingerprint, 7u, 3u);   /* unreachable with a well-formed hex */
    }
    out->compatible_model_assumptions = k_assumptions_2p0;
}

void truing_fixture_wheel_class_asym36(truing_wheel_class_config_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->n_spokes = 36u;
    out->n_cross = 3u;
    out->rim_diameter_mm = 622.0f;
    out->spoke_diameter_mm = 2.0f;
    out->spoke_material_modulus_pa = 200.0e9f;
    out->asymmetric = true;
    out->side_a_basis = TRUING_SIDE_A_BASIS_ROTOR;
    out->hub_side_a.flange_diameter_mm = 45.0f;
    out->hub_side_a.flange_offset_mm = 20.0f;   /* rotor-side flange inboard */
    out->hub_side_b.flange_diameter_mm = 45.0f;
    out->hub_side_b.flange_offset_mm = 35.0f;
    out->c_side_a_n_per_rev = 450.0f;
    out->c_side_b_n_per_rev = 500.0f;
    out->indexing_origin.side = TRUING_SIDE_B;
    out->indexing_origin.lead_trail = TRUING_LEADING;
    out->symmetry_angle_tolerance_rad = 0.01f;
    fill_fingerprint(&out->expected_influence_fingerprint, 13u, 1u);
    out->compatible_model_assumptions = k_assumptions_2p0;
}

void truing_fixture_solver_config(truing_solver_config_t *out, uint8_t n_rim_angles)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->tol_lateral_mm = 0.1f;
    out->tol_radial_mm = 0.05f;
    out->tol_tension_n = 100.0f;
    out->tol_tension_cv = 0.10f;
    out->tol_mean_tension_error_n = 100.0f;
    out->tol_angular_rad = n_rim_angles != 0u ? TRUING_TWO_PI / (float)n_rim_angles : TRUING_TWO_PI;
    out->max_condition_number = 1.0e4f;
    out->rank_tolerance = 1.0e-6f;
    out->n_mt_displacement_tolerance = 1.0e-6f;
    out->n_mt_tension_tolerance = 1.0e-3f;
    out->n_mt_uniqueness_threshold = 0.1f;
    out->max_adjustment_revolutions = 1.0f;
    out->partial_state_remeasure_attempts = 2u;
    out->trust_radial = 0.5f;
    out->trust_tension = 1.0e-5f;
    out->N_lat = 6u;
    out->N_rad = 13u;
    out->n_rim_angles = n_rim_angles;
    out->target_tension_n = 1000.0f;
    out->tension_model_profile_id = 1u;
    out->adjustment_deadband_rev = 0.05f;
    out->lateral_deadband_mm = 0.02f;
    out->max_cycles = 5u;
    out->min_relative_improvement = 0.05f;
    out->consecutive_non_improving_cycles = 2u;
    out->measurement_retry_count = 2u;
    out->excitation_settle_ms = 500u;
}

void truing_fixture_excitation_profile(truing_excitation_profile_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->excitation_id = 1u;
    /* 20 ms was never fitted to a solenoid; it is the starting point the B3.2 per-station
     * pulse sweep brackets, identical at both stations until evidence says otherwise. */
    out->pulse_ms[0] = 20.0f;
    out->pulse_ms[1] = 20.0f;
}

void truing_fixture_chain_profile_inmp441(truing_chain_profile_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->chain_id = 1u;
    out->transducer = TRUING_TRANSDUCER_INMP441_I2S;
    out->sample_rate_hz = TRUING_AUDIO_SAMPLE_RATE_HZ;
    out->bit_depth = (uint8_t)TRUING_AUDIO_BIT_DEPTH;
    out->noise_floor_dbfs = -80.0f;
    out->preflight_min_snr_db = 12.0f;
    out->preflight_max_noise_floor_dbfs = -60.0f;
    out->f1_band_lo_hz = 350.0f;
    out->f1_band_hi_hz = 600.0f;
    out->window_ms = 500.0f;
    out->gate_start_ms = 40.0f;
    /* Phase 1f: the research repository's config/dsp.yaml values (its commit is recorded in
     * acoustic_prep/golden/acoustic_golden.json). Fixture content, PROVISIONAL by that file's own
     * statement; bench results change them, theory does not. */
    out->capture_ms = 1000.0f;
    out->pre_trigger_ms = 200.0f;
    out->measurement_min_snr_db = 12.0f;     /* research report.snr_warn_db */
    out->onset_frame_ms = 10.0f;
    out->onset_hop_ms = 5.0f;
    out->onset_threshold_rel = 0.15f;
    out->onset_threshold_abs = 0.0017783f;   /* -55 dBFS rms: 14 dB above this chain's recorded noise floor and
                                              * below every recorded pluck's relative threshold (dsp.yaml leaves
                                              * it null; a campaign file always holds plucks) */
    out->onset_refractory_s = 2.5f;
    out->decay_floor_db = 30.0f;
    out->decay_smoothing_ms = 5.0f;
    out->next_onset_margin_ms = 20.0f;
    out->min_window_ms = 60.0f;
    out->zero_pad_factor = 8.0f;
    out->search_band_lo_hz = 300.0f;
    out->search_band_hi_hz = 3000.0f;
    out->prominence_db = 6.0f;
    out->max_peak_depth_db = 30.0f;
    out->max_peaks = 8u;
    out->f2_ratio_lo = 1.9f;
    out->f2_ratio_hi = 2.3f;
    out->snr_noise_offset_lo_hz = 100.0f;
    out->snr_noise_offset_hi_hz = 300.0f;
}

static void profile_base(truing_tension_model_profile_t *out, float gauge_mm)
{
    memset(out, 0, sizeof(*out));
    out->profile_id = 1u;
    out->model_name = TRUING_TENSION_MODEL_IDEAL_STRING;
    out->model_version = 1u;
    const float d = gauge_mm * 1.0e-3f;
    const float area = TRUING_PI * d * d / 4.0f;
    const float inertia = TRUING_PI * d * d * d * d / 64.0f;
    out->L_eff_m.value = 0.1935f;
    out->L_eff_m.provenance = TRUING_PROVENANCE_RESEARCH_DERIVED;
    out->linear_density_kg_per_m.value = 7850.0f * area;
    out->linear_density_kg_per_m.provenance = TRUING_PROVENANCE_RESEARCH_DERIVED;
    out->bending_stiffness_n_m2.value = 200.0e9f * inertia;
    out->bending_stiffness_n_m2.provenance = TRUING_PROVENANCE_RESEARCH_DERIVED;
    out->nominal_free_span_m.value = 0.1935f;
    out->nominal_free_span_m.provenance = TRUING_PROVENANCE_BENCH_CALIBRATED;
    out->empirical_a.value = NAN;
    out->empirical_a.provenance = TRUING_PROVENANCE_UNESTABLISHED;
    out->empirical_n.value = NAN;
    out->empirical_n.provenance = TRUING_PROVENANCE_UNESTABLISHED;
    out->assumptions = k_assumptions_2p0;
    out->assumptions.spoke_diameter_mm = gauge_mm;
    out->l_eff_bounds_lo = 0.5f;   /* research config/models.yaml m2.l_eff_bounds */
    out->l_eff_bounds_hi = 1.5f;
}

void truing_fixture_tension_model_profile_complete(truing_tension_model_profile_t *out)
{
    if (out == NULL) {
        return;
    }
    profile_base(out, 2.0f);
}

void truing_fixture_tension_model_profile_incomplete(truing_tension_model_profile_t *out)
{
    if (out == NULL) {
        return;
    }
    profile_base(out, 2.0f);
    out->profile_id = 2u;
    out->L_eff_m.value = NAN;
    out->L_eff_m.provenance = TRUING_PROVENANCE_UNESTABLISHED;
}

void truing_fixture_tension_model_profile_incompatible(truing_tension_model_profile_t *out)
{
    if (out == NULL) {
        return;
    }
    profile_base(out, 2.34f);
    out->profile_id = 3u;
}

void truing_fixture_machine_profile(truing_machine_profile_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->profile_id = 2u;   /* 2: two acoustic stations (1 was the single-station profile) */
    /* PLACEHOLDER angles for both acoustic stations until the B2 bench measures the rig (plan
     * M5); a measured value is a new profile (§11.6). Nothing in the fast demo depends on them. */
    out->stations[TRUING_STATION_ACOUSTIC_LEFT].present = true;
    out->stations[TRUING_STATION_ACOUSTIC_LEFT].angle_rad = 0.0f;
    out->stations[TRUING_STATION_ACOUSTIC_LEFT].positioning_tolerance_rad = 0.02f;
    out->stations[TRUING_STATION_ACOUSTIC_RIGHT].present = true;
    out->stations[TRUING_STATION_ACOUSTIC_RIGHT].angle_rad = 3.0f * TRUING_PI / 2.0f;
    out->stations[TRUING_STATION_ACOUSTIC_RIGHT].positioning_tolerance_rad = 0.02f;
    out->stations[TRUING_STATION_RUNOUT].present = true;
    out->stations[TRUING_STATION_RUNOUT].angle_rad = TRUING_PI / 2.0f;
    out->stations[TRUING_STATION_RUNOUT].positioning_tolerance_rad = 0.02f;
    out->stations[TRUING_STATION_ADJUSTMENT].present = true;
    out->stations[TRUING_STATION_ADJUSTMENT].angle_rad = TRUING_PI;
    out->stations[TRUING_STATION_ADJUSTMENT].positioning_tolerance_rad = 0.02f;
    out->stations[TRUING_STATION_REFERENCE].present = false;
    out->reference_station = TRUING_STATION_ACOUSTIC_LEFT;   /* spoke 0 is confirmed at its own station */
}
