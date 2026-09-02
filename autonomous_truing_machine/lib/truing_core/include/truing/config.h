/**
 * @file config.h
 * Configuration and profile structures (SPEC §11.1–§11.3.1) with validation.
 *
 * SPEC P5: every value that could change with a different wheel, sensor, research
 * result or tolerance is configuration. These structures ARE the configuration
 * definition; no field's value may appear as a literal anywhere else.
 *
 * SPEC §11.3.1: "No defaults, ever." A required parameter that is unestablished
 * makes the profile incomplete (CALIBRATION_MISSING). Nothing here supplies a
 * nominal value in place of an established one.
 */
#ifndef TRUING_CONFIG_H
#define TRUING_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "truing/limits.h"
#include "truing/model_ids.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Side naming (SPEC §6.4.1) --------------------------------------------------- */
typedef enum {
    TRUING_SIDE_UNSET = 0,
    TRUING_SIDE_A,     /* cassette (rear) or brake rotor (front) side; maps to bike-wheel-calc DS */
    TRUING_SIDE_B,     /* opposite side; maps to NDS. Positive lateral is toward Side B. */
} truing_side_t;

typedef enum {
    TRUING_LEAD_TRAIL_UNSET = 0,
    TRUING_LEADING,
    TRUING_TRAILING,
} truing_lead_trail_t;

/* What physically defines Side A. Wheels with neither feature MUST declare the
 * assignment explicitly (SPEC §6.4.1) — hence DECLARED_ARBITRARY is a real value. */
typedef enum {
    TRUING_SIDE_A_BASIS_UNSET = 0,
    TRUING_SIDE_A_BASIS_CASSETTE,
    TRUING_SIDE_A_BASIS_ROTOR,
    TRUING_SIDE_A_BASIS_DECLARED_ARBITRARY,
} truing_side_a_basis_t;

typedef struct {
    float flange_diameter_mm;   /* spoke-hole pitch circle diameter */
    float flange_offset_mm;     /* hub centre-plane to flange centre-plane */
} truing_flange_geometry_t;

#define TRUING_FINGERPRINT_BYTES 32u
typedef struct {
    uint8_t bytes[TRUING_FINGERPRINT_BYTES];
    bool    set;
} truing_fingerprint_t;

/* Facts a tension-model profile must agree with (SPEC §11.1 compatible_model_assumptions). */
typedef struct {
    float spoke_diameter_mm;
    float spoke_material_modulus_pa;
    float spoke_density_kg_m3;
} truing_model_assumptions_t;

/* SPEC §6.5: which physical spoke is index 0 and its class. */
typedef struct {
    truing_side_t       side;
    truing_lead_trail_t lead_trail;
} truing_indexing_origin_t;

/* ---- §11.1 Wheel class configuration -------------------------------------------- */
typedef struct {
    uint8_t  n_spokes;                     /* 32 or 36 */
    uint8_t  n_cross;
    float    rim_diameter_mm;              /* e.g. 622 ISO */
    float    spoke_diameter_mm;            /* uniform round, operator-declared */
    float    spoke_material_modulus_pa;
    bool     asymmetric;                   /* from GEOMETRY, corroborated by §14.3 tests — never inferred from a rotor */
    truing_side_a_basis_t side_a_basis;
    truing_flange_geometry_t hub_side_a;
    truing_flange_geometry_t hub_side_b;
    float    c_side_a_n_per_rev;           /* measured response slope, per side (SPEC §8.4) */
    float    c_side_b_n_per_rev;
    truing_indexing_origin_t indexing_origin;
    float    symmetry_angle_tolerance_rad;
    truing_fingerprint_t expected_influence_fingerprint;   /* SOLE artifact compatibility authority (SPEC §11.4) */
    truing_model_assumptions_t compatible_model_assumptions;
} truing_wheel_class_config_t;

/* ---- §11.2 Solver configuration ------------------------------------------------- */
typedef struct truing_solver_config {
    float    tol_lateral_mm;               /* normalization AND acceptance */
    float    tol_radial_mm;
    float    tol_tension_n;                /* NORMALIZATION scale only (SPEC §8.6) */
    float    tol_tension_cv;               /* per-side non-uniformity acceptance */
    float    tol_mean_tension_error_n;     /* per-side mean-target acceptance */
    float    tol_angular_rad;              /* column localization test (SPEC §6.5) */
    float    max_condition_number;         /* SPEC §8.11 R3 */
    float    rank_tolerance;               /* relative to sigma_1 */
    float    n_mt_displacement_tolerance;
    float    n_mt_tension_tolerance;
    float    n_mt_uniqueness_threshold;
    float    max_adjustment_revolutions;   /* exceeding => ABORT_UNSAFE_ADJUSTMENT */
    uint8_t  partial_state_remeasure_attempts;
    float    trust_radial;                 /* dimensionless */
    float    trust_tension;                /* dimensionless; re-derive (SPEC §8.3.2) */
    uint8_t  N_lat;
    uint8_t  N_rad;
    uint8_t  n_rim_angles;                 /* = n_spokes in Stage 1 (SPEC §8.9) */
    float    target_tension_n;             /* overall MEAN scale; session-fixed */
    uint32_t tension_model_profile_id;     /* session-fixed selector (SPEC §11.3.1) */
    float    adjustment_deadband_rev;
    float    lateral_deadband_mm;          /* Stage 2 closed loop (SPEC §7.6) */
    uint8_t  max_cycles;
    float    min_relative_improvement;     /* SPEC §7.5.1 */
    uint8_t  consecutive_non_improving_cycles;
    uint8_t  measurement_retry_count;
    uint16_t excitation_settle_ms;
} truing_solver_config_t;

/* ---- §11.3 Acoustic chain profile ----------------------------------------------- */
typedef enum {
    TRUING_TRANSDUCER_UNSET = 0,
    TRUING_TRANSDUCER_INMP441_I2S,
    TRUING_TRANSDUCER_MAGNETIC_PICKUP_ANALOG,
} truing_transducer_t;

typedef struct {
    uint32_t chain_id;
    truing_transducer_t transducer;
    uint32_t sample_rate_hz;               /* must equal TRUING_AUDIO_SAMPLE_RATE_HZ */
    uint8_t  bit_depth;                    /* must equal TRUING_AUDIO_BIT_DEPTH */
    float    noise_floor_dbfs;             /* characterization */
    float    preflight_min_snr_db;         /* preflight thresholds */
    float    preflight_max_noise_floor_dbfs;
    float    f1_band_lo_hz;
    float    f1_band_hi_hz;
    float    window_ms;
    float    gate_start_ms;
    /* Phase 1f: every remaining DSP constant of the research pipeline (SPEC 9.5 "configuration,
     * never literals"; reference meaning in the research repo's config/dsp.yaml). */
    float    capture_ms;                   /* audio captured per measurement after the excitation trigger */
    float    pre_trigger_ms;               /* audio kept before the trigger so the onset detector sees silence */
    float    excitation_pulse_ms;          /* pluck actuator pulse (0 = no actuator commanded) */
    float    measurement_min_snr_db;       /* below this the estimate is REJECTED / LOW_SNR */
    float    onset_frame_ms;
    float    onset_hop_ms;
    float    onset_threshold_rel;
    float    onset_threshold_abs;          /* RMS, full scale 1.0; 0 disables */
    float    onset_refractory_s;
    float    decay_floor_db;
    float    decay_smoothing_ms;
    float    next_onset_margin_ms;
    float    min_window_ms;
    float    zero_pad_factor;
    float    search_band_lo_hz;
    float    search_band_hi_hz;
    float    prominence_db;
    float    max_peak_depth_db;            /* 0 disables the relative-depth gate */
    uint8_t  max_peaks;
    float    f2_ratio_lo;
    float    f2_ratio_hi;
    float    snr_noise_offset_lo_hz;
    float    snr_noise_offset_hi_hz;
} truing_chain_profile_t;

/* ---- §11.3.1 Tension model profile ---------------------------------------------- */
typedef enum {
    TRUING_PROVENANCE_UNESTABLISHED = 0,   /* field not usable */
    TRUING_PROVENANCE_RESEARCH_DERIVED,
    TRUING_PROVENANCE_BENCH_CALIBRATED,
} truing_param_provenance_t;

typedef struct {
    float                     value;
    truing_param_provenance_t provenance;
} truing_established_param_t;

typedef enum {
    TRUING_TMP_FIELD_L_EFF             = 1u << 0,
    TRUING_TMP_FIELD_LINEAR_DENSITY    = 1u << 1,
    TRUING_TMP_FIELD_BENDING_STIFFNESS = 1u << 2,
    TRUING_TMP_FIELD_NOMINAL_FREE_SPAN = 1u << 3,
    TRUING_TMP_FIELD_EMPIRICAL_A       = 1u << 4,
    TRUING_TMP_FIELD_EMPIRICAL_N       = 1u << 5,
} truing_tmp_field_t;

typedef struct {
    uint32_t               profile_id;
    truing_tension_model_t model_name;
    uint16_t               model_version;
    truing_established_param_t L_eff_m;                  /* research-derived, NOT operator-entered */
    truing_established_param_t linear_density_kg_per_m;  /* mu */
    truing_established_param_t bending_stiffness_n_m2;   /* EI */
    truing_established_param_t nominal_free_span_m;      /* bounds reference for two-mode inversion */
    truing_established_param_t empirical_a;
    truing_established_param_t empirical_n;
    truing_model_assumptions_t assumptions;
    /* HIGHER_MODE only: the recovered L_eff must lie within [lo, hi] x nominal_free_span
     * (research repo config/models.yaml m2.l_eff_bounds); a model parameter, not a literal. */
    float l_eff_bounds_lo;
    float l_eff_bounds_hi;
} truing_tension_model_profile_t;

/* ---- §11.6 Machine profile: physical station geometry (SPEC §10A) ----------------- */
/* The physical stations of the machine. Their placement is DATA (this profile), never a
 * compiled-in assumption: a mechanical revision that relocates a station changes this
 * profile, not the workflow. Stations may share an angle. */
typedef enum {
    TRUING_STATION_UNSET = 0,
    TRUING_STATION_ACOUSTIC,     /* excitation + capture */
    TRUING_STATION_RUNOUT,       /* lateral / radial gauges */
    TRUING_STATION_ADJUSTMENT,   /* nipple adjustment (operator in C2, actuator in C3) */
    TRUING_STATION_REFERENCE,    /* index / reference sensor, if the machine has one */
    TRUING_STATION__COUNT
} truing_station_id_t;

typedef struct {
    bool  present;
    float angle_rad;                     /* machine-frame angle of the station, [0, 2π) */
    float positioning_tolerance_rad;     /* C3: acceptable feature-to-station error (OPEN, hardware) */
} truing_station_geometry_t;

typedef struct {
    uint32_t                  profile_id;
    truing_station_geometry_t stations[TRUING_STATION__COUNT];   /* indexed by truing_station_id_t */
    truing_station_id_t       reference_station;                 /* where the wheel reference is established (SPEC §6.5) */
} truing_machine_profile_t;

/* ---- Validation ----------------------------------------------------------------- */
typedef enum {
    TRUING_CFG_OK = 0,
    TRUING_CFG_ERR_NULL,
    TRUING_CFG_ERR_UNSET,
    TRUING_CFG_ERR_NOT_FINITE,
    TRUING_CFG_ERR_NOT_POSITIVE,
    TRUING_CFG_ERR_OUT_OF_RANGE,
    TRUING_CFG_ERR_N_SPOKES_UNSUPPORTED,
    TRUING_CFG_ERR_SYMMETRY_CLAIM_CONTRADICTS_GEOMETRY,  /* asymmetric=false but bracing angles differ */
    TRUING_CFG_ERR_AUDIO_FORMAT,                         /* SPEC §9.3 / §14.4 */
    TRUING_CFG_ERR_BAND,
    TRUING_CFG_ERR_MODEL_UNSET,
    TRUING_CFG_ERR_CALIBRATION_MISSING,                  /* SPEC §11.3.1 */
    TRUING_CFG_ERR_RIM_ANGLES_NOT_EQUAL_SPOKES,          /* SPEC §8.9 Stage 1 policy */
    TRUING_CFG_ERR_MODEL_INCOMPATIBLE,                   /* profile assumptions disagree with wheel */
    TRUING_CFG__COUNT
} truing_cfg_check_t;

/* Relative float-equality guard for comparing two DECLARATIONS of the same fact
 * (e.g. profile assumptions vs. wheel configuration). Not a physical tolerance. */
#define TRUING_DECLARATION_MATCH_REL_EPS 1e-6f

/* `field` (optional) receives the name of the offending field. */
truing_cfg_check_t truing_wheel_class_config_check(const truing_wheel_class_config_t *cfg, const char **field);
truing_cfg_check_t truing_solver_config_check(const truing_solver_config_t *cfg, const char **field);
truing_cfg_check_t truing_chain_profile_check(const truing_chain_profile_t *p, const char **field);
truing_cfg_check_t truing_tension_model_profile_check(const truing_tension_model_profile_t *p,
                                                      uint32_t *missing_fields, const char **field);
truing_cfg_check_t truing_tension_model_profile_compatible(const truing_tension_model_profile_t *p,
                                                           const truing_wheel_class_config_t *wheel,
                                                           const char **field);
/* Cross-structure rules (SPEC §8.9: n_rim_angles = n_spokes in Stage 1). */
truing_cfg_check_t truing_config_check_pair(const truing_wheel_class_config_t *wheel,
                                            const truing_solver_config_t *solver, const char **field);

truing_cfg_check_t truing_machine_profile_check(const truing_machine_profile_t *p, const char **field);
/* NULL when the station is absent from the profile. */
const truing_station_geometry_t *truing_machine_profile_station(const truing_machine_profile_t *p, truing_station_id_t id);
const char *truing_station_str(truing_station_id_t id);

/* Bracing angle per side from configured hub geometry (SPEC §11.1 geometry classification):
 * alpha = atan2(flange_offset, rim_radius - flange_radius). */
bool truing_wheel_class_bracing_angles(const truing_wheel_class_config_t *cfg, float *alpha_a_rad, float *alpha_b_rad);

uint32_t truing_tension_model_required_fields(truing_tension_model_t model);
uint32_t truing_tension_model_profile_established_fields(const truing_tension_model_profile_t *p);

bool truing_fingerprint_equals(const truing_fingerprint_t *a, const truing_fingerprint_t *b);

const char *truing_cfg_check_str(truing_cfg_check_t c);

#ifdef __cplusplus
}
#endif
#endif /* TRUING_CONFIG_H */
