/**
 * @file measurements.h
 * The measurement chain data types (SPEC §6.1, §6.3) and their validity checks.
 *
 *   FrequencyMeasurement -> TensionEstimate -> SpokeMeasurement
 *   RunoutMeasurement
 *
 * Units follow the canonical convention of SPEC §6.4: mm, N, radians, revolutions.
 */
#ifndef TRUING_MEASUREMENTS_H
#define TRUING_MEASUREMENTS_H

#include <stdint.h>

#include "truing/limits.h"
#include "truing/model_ids.h"
#include "truing/status.h"

#ifdef __cplusplus
extern "C" {
#endif

/* SPEC §6.3: the selected peak is a SELECTION, not an identification. */
typedef enum {
    TRUING_MODE_ID_UNSET = 0,
    TRUING_MODE_ID_CONFIRMED_FUNDAMENTAL,   /* independently corroborated */
    TRUING_MODE_ID_PRESUMED_FUNDAMENTAL,    /* selection rule output, unverified (default while §9.5 open) */
    TRUING_MODE_ID_UNKNOWN,
    TRUING_MODE_ID__COUNT
} truing_mode_identity_t;

typedef struct {
    float frequency_hz;
    float magnitude_db;
    float prominence_db;
} truing_peak_candidate_t;

typedef struct {
    truing_record_meta_t    meta;
    float                   selected_frequency_hz;   /* the peak the rule chose */
    truing_mode_identity_t  mode_identity;
    uint8_t                 n_candidates;
    truing_peak_candidate_t candidates[TRUING_MAX_CANDIDATE_PEAKS];
    float                   snr_db;
    uint16_t                selection_rule_version;
} truing_frequency_measurement_t;

typedef struct {
    truing_record_meta_t           meta;
    float                          tension_n;      /* tensile, always positive when valid/suspect */
    float                          sigma_n;        /* NAN when not estimated */
    truing_tension_model_t         model_name;     /* SPEC §6.3: mandatory provenance */
    uint16_t                       model_version;
    truing_frequency_measurement_t frequency;      /* the measurement this estimate was derived from */
} truing_tension_estimate_t;

typedef struct {
    uint8_t                   spoke_index;
    truing_tension_estimate_t tension;
} truing_spoke_measurement_t;

typedef struct {
    truing_record_meta_t meta;
    float rim_angle_rad;    /* SPEC §6.3: mandatory; CCW from Side A, valve stem = 0 */
    float lateral_mm;       /* positive toward Side B (SPEC §6.4) */
    float radial_mm;        /* positive outward from hub (SPEC §6.4) */
} truing_runout_measurement_t;

truing_check_t truing_frequency_measurement_check(const truing_frequency_measurement_t *m);
truing_check_t truing_tension_estimate_check(const truing_tension_estimate_t *e);
truing_check_t truing_runout_measurement_check(const truing_runout_measurement_t *m);

const char *truing_mode_identity_str(truing_mode_identity_t m);

#ifdef __cplusplus
}
#endif
#endif /* TRUING_MEASUREMENTS_H */
