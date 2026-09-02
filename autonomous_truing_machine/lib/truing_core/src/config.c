#include "truing/config.h"

#include <math.h>
#include <string.h>

#include "truing/wheel_state.h"

static const char *const k_cfg_str[TRUING_CFG__COUNT] = {
    "OK",
    "ERR_NULL",
    "ERR_UNSET",
    "ERR_NOT_FINITE",
    "ERR_NOT_POSITIVE",
    "ERR_OUT_OF_RANGE",
    "ERR_N_SPOKES_UNSUPPORTED",
    "ERR_SYMMETRY_CLAIM_CONTRADICTS_GEOMETRY",
    "ERR_AUDIO_FORMAT",
    "ERR_BAND",
    "ERR_MODEL_UNSET",
    "ERR_CALIBRATION_MISSING",
    "ERR_RIM_ANGLES_NOT_EQUAL_SPOKES",
    "ERR_MODEL_INCOMPATIBLE",
};

const char *truing_cfg_check_str(truing_cfg_check_t c)
{
    return (unsigned)c < TRUING_CFG__COUNT ? k_cfg_str[c] : "?";
}

static void set_field(const char **field, const char *name)
{
    if (field != NULL) {
        *field = name;
    }
}

/* Positive, finite float. */
static truing_cfg_check_t check_pos(float v, const char *name, const char **field)
{
    if (!isfinite(v)) {
        set_field(field, name);
        return TRUING_CFG_ERR_NOT_FINITE;
    }
    if (v <= 0.0f) {
        set_field(field, name);
        return TRUING_CFG_ERR_NOT_POSITIVE;
    }
    return TRUING_CFG_OK;
}

/* Non-negative, finite float. */
static truing_cfg_check_t check_nonneg(float v, const char *name, const char **field)
{
    if (!isfinite(v)) {
        set_field(field, name);
        return TRUING_CFG_ERR_NOT_FINITE;
    }
    if (v < 0.0f) {
        set_field(field, name);
        return TRUING_CFG_ERR_OUT_OF_RANGE;
    }
    return TRUING_CFG_OK;
}

static bool declarations_match(float a, float b)
{
    const float scale = fmaxf(fabsf(a), fabsf(b));
    return fabsf(a - b) <= TRUING_DECLARATION_MATCH_REL_EPS * scale;
}

#define CHECK(expr)                          \
    do {                                     \
        const truing_cfg_check_t r_ = (expr); \
        if (r_ != TRUING_CFG_OK) {           \
            return r_;                       \
        }                                    \
    } while (0)

bool truing_fingerprint_equals(const truing_fingerprint_t *a, const truing_fingerprint_t *b)
{
    if (a == NULL || b == NULL || !a->set || !b->set) {
        return false;
    }
    return memcmp(a->bytes, b->bytes, TRUING_FINGERPRINT_BYTES) == 0;
}

bool truing_wheel_class_bracing_angles(const truing_wheel_class_config_t *cfg, float *alpha_a_rad, float *alpha_b_rad)
{
    if (cfg == NULL || alpha_a_rad == NULL || alpha_b_rad == NULL) {
        return false;
    }
    const float rim_radius = cfg->rim_diameter_mm * 0.5f;
    const float span_a = rim_radius - cfg->hub_side_a.flange_diameter_mm * 0.5f;
    const float span_b = rim_radius - cfg->hub_side_b.flange_diameter_mm * 0.5f;
    if (!isfinite(rim_radius) || !isfinite(span_a) || !isfinite(span_b) || span_a <= 0.0f || span_b <= 0.0f) {
        return false;
    }
    if (!isfinite(cfg->hub_side_a.flange_offset_mm) || !isfinite(cfg->hub_side_b.flange_offset_mm)) {
        return false;
    }
    *alpha_a_rad = atan2f(cfg->hub_side_a.flange_offset_mm, span_a);
    *alpha_b_rad = atan2f(cfg->hub_side_b.flange_offset_mm, span_b);
    return true;
}

static truing_cfg_check_t check_assumptions(const truing_model_assumptions_t *a, const char **field)
{
    CHECK(check_pos(a->spoke_diameter_mm, "model_assumptions.spoke_diameter_mm", field));
    CHECK(check_pos(a->spoke_material_modulus_pa, "model_assumptions.spoke_material_modulus_pa", field));
    CHECK(check_pos(a->spoke_density_kg_m3, "model_assumptions.spoke_density_kg_m3", field));
    return TRUING_CFG_OK;
}

truing_cfg_check_t truing_wheel_class_config_check(const truing_wheel_class_config_t *cfg, const char **field)
{
    set_field(field, "");
    if (cfg == NULL) {
        return TRUING_CFG_ERR_NULL;
    }
    if (!truing_wheel_dims_supported(cfg->n_spokes, cfg->n_spokes)) {
        set_field(field, "n_spokes");
        return TRUING_CFG_ERR_N_SPOKES_UNSUPPORTED;
    }
    CHECK(check_pos(cfg->rim_diameter_mm, "rim_diameter_mm", field));
    CHECK(check_pos(cfg->spoke_diameter_mm, "spoke_diameter_mm", field));
    CHECK(check_pos(cfg->spoke_material_modulus_pa, "spoke_material_modulus_pa", field));
    if (cfg->side_a_basis == TRUING_SIDE_A_BASIS_UNSET) {
        set_field(field, "side_a_basis");
        return TRUING_CFG_ERR_UNSET;
    }
    CHECK(check_pos(cfg->hub_side_a.flange_diameter_mm, "hub_side_a.flange_diameter_mm", field));
    CHECK(check_pos(cfg->hub_side_a.flange_offset_mm, "hub_side_a.flange_offset_mm", field));
    CHECK(check_pos(cfg->hub_side_b.flange_diameter_mm, "hub_side_b.flange_diameter_mm", field));
    CHECK(check_pos(cfg->hub_side_b.flange_offset_mm, "hub_side_b.flange_offset_mm", field));
    if (cfg->hub_side_a.flange_diameter_mm >= cfg->rim_diameter_mm ||
        cfg->hub_side_b.flange_diameter_mm >= cfg->rim_diameter_mm) {
        set_field(field, "hub flange_diameter_mm vs rim_diameter_mm");
        return TRUING_CFG_ERR_OUT_OF_RANGE;
    }
    CHECK(check_pos(cfg->c_side_a_n_per_rev, "c_side_a_n_per_rev", field));
    CHECK(check_pos(cfg->c_side_b_n_per_rev, "c_side_b_n_per_rev", field));
    if (cfg->indexing_origin.side == TRUING_SIDE_UNSET) {
        set_field(field, "indexing_origin.side");
        return TRUING_CFG_ERR_UNSET;
    }
    if (cfg->indexing_origin.lead_trail == TRUING_LEAD_TRAIL_UNSET) {
        set_field(field, "indexing_origin.lead_trail");
        return TRUING_CFG_ERR_UNSET;
    }
    CHECK(check_nonneg(cfg->symmetry_angle_tolerance_rad, "symmetry_angle_tolerance_rad", field));
    if (!cfg->asymmetric) {
        /* SPEC §11.1: asymmetric=false REQUIRES the bracing angles to agree within tolerance.
         * (The second requirement — the artifact passing §14.3 symmetric property tests — is
         * checked at artifact load, host-side model prep.) Classifying as asymmetric is always safe. */
        float a, b;
        if (!truing_wheel_class_bracing_angles(cfg, &a, &b)) {
            set_field(field, "hub geometry");
            return TRUING_CFG_ERR_OUT_OF_RANGE;
        }
        if (fabsf(a - b) > cfg->symmetry_angle_tolerance_rad) {
            set_field(field, "asymmetric");
            return TRUING_CFG_ERR_SYMMETRY_CLAIM_CONTRADICTS_GEOMETRY;
        }
    }
    if (!cfg->expected_influence_fingerprint.set) {
        set_field(field, "expected_influence_fingerprint");
        return TRUING_CFG_ERR_UNSET;
    }
    CHECK(check_assumptions(&cfg->compatible_model_assumptions, field));
    /* The assumptions a profile must agree with must themselves agree with this wheel's facts. */
    if (!declarations_match(cfg->compatible_model_assumptions.spoke_diameter_mm, cfg->spoke_diameter_mm)) {
        set_field(field, "compatible_model_assumptions.spoke_diameter_mm");
        return TRUING_CFG_ERR_OUT_OF_RANGE;
    }
    if (!declarations_match(cfg->compatible_model_assumptions.spoke_material_modulus_pa, cfg->spoke_material_modulus_pa)) {
        set_field(field, "compatible_model_assumptions.spoke_material_modulus_pa");
        return TRUING_CFG_ERR_OUT_OF_RANGE;
    }
    return TRUING_CFG_OK;
}

truing_cfg_check_t truing_solver_config_check(const truing_solver_config_t *cfg, const char **field)
{
    set_field(field, "");
    if (cfg == NULL) {
        return TRUING_CFG_ERR_NULL;
    }
    CHECK(check_pos(cfg->tol_lateral_mm, "tol_lateral_mm", field));
    CHECK(check_pos(cfg->tol_radial_mm, "tol_radial_mm", field));
    CHECK(check_pos(cfg->tol_tension_n, "tol_tension_n", field));
    CHECK(check_pos(cfg->tol_tension_cv, "tol_tension_cv", field));
    if (cfg->tol_tension_cv >= 1.0f) {
        set_field(field, "tol_tension_cv");
        return TRUING_CFG_ERR_OUT_OF_RANGE;
    }
    CHECK(check_pos(cfg->tol_mean_tension_error_n, "tol_mean_tension_error_n", field));
    CHECK(check_pos(cfg->tol_angular_rad, "tol_angular_rad", field));   /* SPEC §6.5: not zero */
    CHECK(check_pos(cfg->max_condition_number, "max_condition_number", field));
    if (cfg->max_condition_number <= 1.0f) {
        set_field(field, "max_condition_number");
        return TRUING_CFG_ERR_OUT_OF_RANGE;
    }
    CHECK(check_pos(cfg->rank_tolerance, "rank_tolerance", field));
    if (cfg->rank_tolerance >= 1.0f) {
        set_field(field, "rank_tolerance");
        return TRUING_CFG_ERR_OUT_OF_RANGE;
    }
    CHECK(check_pos(cfg->n_mt_displacement_tolerance, "n_mt_displacement_tolerance", field));
    CHECK(check_pos(cfg->n_mt_tension_tolerance, "n_mt_tension_tolerance", field));
    CHECK(check_pos(cfg->n_mt_uniqueness_threshold, "n_mt_uniqueness_threshold", field));
    CHECK(check_pos(cfg->max_adjustment_revolutions, "max_adjustment_revolutions", field));
    CHECK(check_pos(cfg->trust_radial, "trust_radial", field));
    CHECK(check_pos(cfg->trust_tension, "trust_tension", field));
    if (cfg->N_lat == 0u) {
        set_field(field, "N_lat");
        return TRUING_CFG_ERR_OUT_OF_RANGE;
    }
    if (cfg->N_rad == 0u) {
        set_field(field, "N_rad");
        return TRUING_CFG_ERR_OUT_OF_RANGE;
    }
    if (cfg->n_rim_angles == 0u || cfg->n_rim_angles > TRUING_MAX_RIM_ANGLES) {
        set_field(field, "n_rim_angles");
        return TRUING_CFG_ERR_OUT_OF_RANGE;
    }
    CHECK(check_pos(cfg->target_tension_n, "target_tension_n", field));
    if (cfg->tension_model_profile_id == 0u) {
        /* SPEC §9.2: a run with no model selected is a configuration error. */
        set_field(field, "tension_model_profile_id");
        return TRUING_CFG_ERR_UNSET;
    }
    CHECK(check_nonneg(cfg->adjustment_deadband_rev, "adjustment_deadband_rev", field));
    CHECK(check_nonneg(cfg->lateral_deadband_mm, "lateral_deadband_mm", field));
    if (cfg->max_cycles == 0u) {
        set_field(field, "max_cycles");
        return TRUING_CFG_ERR_OUT_OF_RANGE;
    }
    CHECK(check_nonneg(cfg->min_relative_improvement, "min_relative_improvement", field));
    if (cfg->min_relative_improvement >= 1.0f) {
        set_field(field, "min_relative_improvement");
        return TRUING_CFG_ERR_OUT_OF_RANGE;
    }
    if (cfg->consecutive_non_improving_cycles == 0u) {
        set_field(field, "consecutive_non_improving_cycles");
        return TRUING_CFG_ERR_OUT_OF_RANGE;
    }
    return TRUING_CFG_OK;
}

truing_cfg_check_t truing_chain_profile_check(const truing_chain_profile_t *p, const char **field)
{
    set_field(field, "");
    if (p == NULL) {
        return TRUING_CFG_ERR_NULL;
    }
    if (p->chain_id == 0u) {
        set_field(field, "chain_id");
        return TRUING_CFG_ERR_UNSET;
    }
    if (p->transducer == TRUING_TRANSDUCER_UNSET) {
        set_field(field, "transducer");
        return TRUING_CFG_ERR_UNSET;
    }
    /* SPEC §9.3 fixed format; §14.4: a wrong sample rate is rejected, never resampled. */
    if (p->sample_rate_hz != TRUING_AUDIO_SAMPLE_RATE_HZ || p->bit_depth != TRUING_AUDIO_BIT_DEPTH) {
        set_field(field, "sample_rate_hz/bit_depth");
        return TRUING_CFG_ERR_AUDIO_FORMAT;
    }
    if (!isfinite(p->noise_floor_dbfs) || !isfinite(p->preflight_max_noise_floor_dbfs)) {
        set_field(field, "noise_floor_dbfs");
        return TRUING_CFG_ERR_NOT_FINITE;
    }
    CHECK(check_nonneg(p->preflight_min_snr_db, "preflight_min_snr_db", field));
    CHECK(check_pos(p->f1_band_lo_hz, "f1_band_lo_hz", field));
    CHECK(check_pos(p->f1_band_hi_hz, "f1_band_hi_hz", field));
    if (p->f1_band_lo_hz >= p->f1_band_hi_hz || p->f1_band_hi_hz > (float)TRUING_AUDIO_SAMPLE_RATE_HZ * 0.5f) {
        set_field(field, "f1_band");
        return TRUING_CFG_ERR_BAND;
    }
    CHECK(check_pos(p->window_ms, "window_ms", field));
    CHECK(check_nonneg(p->gate_start_ms, "gate_start_ms", field));
    /* Phase 1f DSP constants (SPEC 9.5): present, finite and mutually consistent. */
    CHECK(check_pos(p->capture_ms, "capture_ms", field));
    CHECK(check_nonneg(p->pre_trigger_ms, "pre_trigger_ms", field));
    CHECK(check_nonneg(p->excitation_pulse_ms, "excitation_pulse_ms", field));
    CHECK(check_nonneg(p->measurement_min_snr_db, "measurement_min_snr_db", field));
    CHECK(check_pos(p->onset_frame_ms, "onset_frame_ms", field));
    CHECK(check_pos(p->onset_hop_ms, "onset_hop_ms", field));
    CHECK(check_pos(p->onset_threshold_rel, "onset_threshold_rel", field));
    CHECK(check_nonneg(p->onset_threshold_abs, "onset_threshold_abs", field));
    CHECK(check_pos(p->onset_refractory_s, "onset_refractory_s", field));
    CHECK(check_pos(p->decay_floor_db, "decay_floor_db", field));
    CHECK(check_nonneg(p->decay_smoothing_ms, "decay_smoothing_ms", field));
    CHECK(check_nonneg(p->next_onset_margin_ms, "next_onset_margin_ms", field));
    CHECK(check_pos(p->min_window_ms, "min_window_ms", field));
    CHECK(check_pos(p->zero_pad_factor, "zero_pad_factor", field));
    CHECK(check_pos(p->search_band_lo_hz, "search_band_lo_hz", field));
    CHECK(check_pos(p->search_band_hi_hz, "search_band_hi_hz", field));
    CHECK(check_pos(p->prominence_db, "prominence_db", field));
    CHECK(check_nonneg(p->max_peak_depth_db, "max_peak_depth_db", field));
    CHECK(check_pos(p->f2_ratio_lo, "f2_ratio_lo", field));
    CHECK(check_pos(p->f2_ratio_hi, "f2_ratio_hi", field));
    CHECK(check_nonneg(p->snr_noise_offset_lo_hz, "snr_noise_offset_lo_hz", field));
    CHECK(check_pos(p->snr_noise_offset_hi_hz, "snr_noise_offset_hi_hz", field));
    if (p->search_band_lo_hz >= p->search_band_hi_hz || p->search_band_hi_hz > (float)TRUING_AUDIO_SAMPLE_RATE_HZ * 0.5f ||
        p->f1_band_lo_hz < p->search_band_lo_hz || p->f1_band_hi_hz > p->search_band_hi_hz) {
        set_field(field, "search_band");
        return TRUING_CFG_ERR_BAND;
    }
    if (p->f2_ratio_lo >= p->f2_ratio_hi || p->snr_noise_offset_lo_hz >= p->snr_noise_offset_hi_hz) {
        set_field(field, "f2_ratio/snr_noise_offset");
        return TRUING_CFG_ERR_BAND;
    }
    if (p->zero_pad_factor < 1.0f || p->max_peaks == 0u || p->max_peaks > TRUING_MAX_CANDIDATE_PEAKS ||
        p->onset_threshold_rel > 1.0f || p->min_window_ms > p->window_ms ||
        p->capture_ms < p->gate_start_ms + p->window_ms) {
        set_field(field, "zero_pad_factor/max_peaks/onset_threshold_rel/min_window_ms/capture_ms");
        return TRUING_CFG_ERR_OUT_OF_RANGE;
    }
    return TRUING_CFG_OK;
}

uint32_t truing_tension_model_required_fields(truing_tension_model_t model)
{
    switch (model) {
    case TRUING_TENSION_MODEL_IDEAL_STRING:
        return TRUING_TMP_FIELD_L_EFF | TRUING_TMP_FIELD_LINEAR_DENSITY;
    case TRUING_TENSION_MODEL_STIFFNESS_CORRECTED:
        return TRUING_TMP_FIELD_L_EFF | TRUING_TMP_FIELD_LINEAR_DENSITY | TRUING_TMP_FIELD_BENDING_STIFFNESS;
    case TRUING_TENSION_MODEL_HIGHER_MODE:
        /* Two-mode inversion recovers L_eff itself; it needs mu, EI and a nominal span for bounds. */
        return TRUING_TMP_FIELD_LINEAR_DENSITY | TRUING_TMP_FIELD_BENDING_STIFFNESS | TRUING_TMP_FIELD_NOMINAL_FREE_SPAN;
    case TRUING_TENSION_MODEL_EMPIRICAL:
        return TRUING_TMP_FIELD_EMPIRICAL_A | TRUING_TMP_FIELD_EMPIRICAL_N;
    default:
        return 0u;
    }
}

static bool established(const truing_established_param_t *p)
{
    return p->provenance != TRUING_PROVENANCE_UNESTABLISHED && isfinite(p->value);
}

uint32_t truing_tension_model_profile_established_fields(const truing_tension_model_profile_t *p)
{
    if (p == NULL) {
        return 0u;
    }
    uint32_t m = 0u;
    if (established(&p->L_eff_m)) {
        m |= TRUING_TMP_FIELD_L_EFF;
    }
    if (established(&p->linear_density_kg_per_m)) {
        m |= TRUING_TMP_FIELD_LINEAR_DENSITY;
    }
    if (established(&p->bending_stiffness_n_m2)) {
        m |= TRUING_TMP_FIELD_BENDING_STIFFNESS;
    }
    if (established(&p->nominal_free_span_m)) {
        m |= TRUING_TMP_FIELD_NOMINAL_FREE_SPAN;
    }
    if (established(&p->empirical_a)) {
        m |= TRUING_TMP_FIELD_EMPIRICAL_A;
    }
    if (established(&p->empirical_n)) {
        m |= TRUING_TMP_FIELD_EMPIRICAL_N;
    }
    return m;
}

truing_cfg_check_t truing_tension_model_profile_check(const truing_tension_model_profile_t *p,
                                                      uint32_t *missing_fields, const char **field)
{
    set_field(field, "");
    if (missing_fields != NULL) {
        *missing_fields = 0u;
    }
    if (p == NULL) {
        return TRUING_CFG_ERR_NULL;
    }
    if (p->profile_id == 0u) {
        set_field(field, "profile_id");
        return TRUING_CFG_ERR_UNSET;
    }
    if (p->model_name == TRUING_TENSION_MODEL_UNSET || (unsigned)p->model_name >= TRUING_TENSION_MODEL__COUNT) {
        set_field(field, "model_name");
        return TRUING_CFG_ERR_MODEL_UNSET;
    }
    const uint32_t required = truing_tension_model_required_fields(p->model_name);
    const uint32_t have = truing_tension_model_profile_established_fields(p);
    const uint32_t missing = required & ~have;
    if (missing != 0u) {
        if (missing_fields != NULL) {
            *missing_fields = missing;
        }
        set_field(field, "required model parameter unestablished");
        return TRUING_CFG_ERR_CALIBRATION_MISSING;
    }
    /* Established physical parameters must be positive. */
    if ((required & TRUING_TMP_FIELD_L_EFF) != 0u) {
        CHECK(check_pos(p->L_eff_m.value, "L_eff_m", field));
    }
    if ((required & TRUING_TMP_FIELD_LINEAR_DENSITY) != 0u) {
        CHECK(check_pos(p->linear_density_kg_per_m.value, "linear_density_kg_per_m", field));
    }
    if ((required & TRUING_TMP_FIELD_BENDING_STIFFNESS) != 0u) {
        CHECK(check_pos(p->bending_stiffness_n_m2.value, "bending_stiffness_n_m2", field));
    }
    if ((required & TRUING_TMP_FIELD_NOMINAL_FREE_SPAN) != 0u) {
        CHECK(check_pos(p->nominal_free_span_m.value, "nominal_free_span_m", field));
    }
    if ((required & TRUING_TMP_FIELD_EMPIRICAL_A) != 0u) {
        CHECK(check_pos(p->empirical_a.value, "empirical_a", field));
    }
    if ((required & TRUING_TMP_FIELD_EMPIRICAL_N) != 0u) {
        CHECK(check_pos(p->empirical_n.value, "empirical_n", field));
    }
    CHECK(check_assumptions(&p->assumptions, field));
    if (p->model_name == TRUING_TENSION_MODEL_HIGHER_MODE) {
        CHECK(check_pos(p->l_eff_bounds_lo, "l_eff_bounds_lo", field));
        CHECK(check_pos(p->l_eff_bounds_hi, "l_eff_bounds_hi", field));
        if (p->l_eff_bounds_lo >= p->l_eff_bounds_hi) {
            set_field(field, "l_eff_bounds");
            return TRUING_CFG_ERR_OUT_OF_RANGE;
        }
    }
    return TRUING_CFG_OK;
}

truing_cfg_check_t truing_tension_model_profile_compatible(const truing_tension_model_profile_t *p,
                                                           const truing_wheel_class_config_t *wheel,
                                                           const char **field)
{
    set_field(field, "");
    if (p == NULL || wheel == NULL) {
        return TRUING_CFG_ERR_NULL;
    }
    const truing_model_assumptions_t *a = &p->assumptions;
    const truing_model_assumptions_t *w = &wheel->compatible_model_assumptions;
    if (!declarations_match(a->spoke_diameter_mm, w->spoke_diameter_mm)) {
        set_field(field, "assumptions.spoke_diameter_mm");
        return TRUING_CFG_ERR_MODEL_INCOMPATIBLE;
    }
    if (!declarations_match(a->spoke_material_modulus_pa, w->spoke_material_modulus_pa)) {
        set_field(field, "assumptions.spoke_material_modulus_pa");
        return TRUING_CFG_ERR_MODEL_INCOMPATIBLE;
    }
    if (!declarations_match(a->spoke_density_kg_m3, w->spoke_density_kg_m3)) {
        set_field(field, "assumptions.spoke_density_kg_m3");
        return TRUING_CFG_ERR_MODEL_INCOMPATIBLE;
    }
    return TRUING_CFG_OK;
}

truing_cfg_check_t truing_config_check_pair(const truing_wheel_class_config_t *wheel,
                                            const truing_solver_config_t *solver, const char **field)
{
    set_field(field, "");
    if (wheel == NULL || solver == NULL) {
        return TRUING_CFG_ERR_NULL;
    }
    if (solver->n_rim_angles != wheel->n_spokes) {
        set_field(field, "n_rim_angles");
        return TRUING_CFG_ERR_RIM_ANGLES_NOT_EQUAL_SPOKES;
    }
    return TRUING_CFG_OK;
}

/* ---- machine profile (SPEC §10A, §11.6) ------------------------------------------- */
static const char *const k_station_str[TRUING_STATION__COUNT] = {
    "unset", "acoustic", "runout", "adjustment", "reference",
};

const char *truing_station_str(truing_station_id_t id)
{
    return (unsigned)id < TRUING_STATION__COUNT ? k_station_str[id] : "?";
}

const truing_station_geometry_t *truing_machine_profile_station(const truing_machine_profile_t *p, truing_station_id_t id)
{
    if (p == NULL || id == TRUING_STATION_UNSET || (unsigned)id >= TRUING_STATION__COUNT) {
        return NULL;
    }
    return p->stations[id].present ? &p->stations[id] : NULL;
}

truing_cfg_check_t truing_machine_profile_check(const truing_machine_profile_t *p, const char **field)
{
    set_field(field, "");
    if (p == NULL) {
        return TRUING_CFG_ERR_NULL;
    }
    if (p->profile_id == 0u) {
        set_field(field, "profile_id");
        return TRUING_CFG_ERR_UNSET;
    }
    /* The workflow positions features at these three stations in every cycle (SPEC §7.1);
     * they may share an angle, but each must be declared. */
    static const truing_station_id_t k_required[] = { TRUING_STATION_ACOUSTIC, TRUING_STATION_RUNOUT, TRUING_STATION_ADJUSTMENT };
    for (unsigned i = 0; i < sizeof(k_required) / sizeof(k_required[0]); ++i) {
        if (!p->stations[k_required[i]].present) {
            set_field(field, k_station_str[k_required[i]]);
            return TRUING_CFG_ERR_UNSET;
        }
    }
    for (unsigned id = 1u; id < TRUING_STATION__COUNT; ++id) {
        const truing_station_geometry_t *s = &p->stations[id];
        if (!s->present) {
            continue;
        }
        if (!isfinite(s->angle_rad) || !isfinite(s->positioning_tolerance_rad)) {
            set_field(field, k_station_str[id]);
            return TRUING_CFG_ERR_NOT_FINITE;
        }
        if (s->angle_rad < 0.0f || s->angle_rad >= TRUING_TWO_PI || s->positioning_tolerance_rad < 0.0f) {
            set_field(field, k_station_str[id]);
            return TRUING_CFG_ERR_OUT_OF_RANGE;
        }
    }
    if (truing_machine_profile_station(p, p->reference_station) == NULL) {
        set_field(field, "reference_station");
        return TRUING_CFG_ERR_UNSET;
    }
    return TRUING_CFG_OK;
}
