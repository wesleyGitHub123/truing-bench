/* Shared record builders for host tests. Header-only; included by test sources. */
#ifndef TRUING_TEST_RECORDS_H
#define TRUING_TEST_RECORDS_H

#include <math.h>
#include <string.h>

#include "truing/measurements.h"

static inline truing_record_meta_t test_meta(truing_status_t s, truing_reason_t r, uint8_t cycle, truing_source_impl_t src)
{
    truing_record_meta_t m;
    memset(&m, 0, sizeof(m));
    m.status = s;
    m.reason_code = r;
    m.cycle_index = cycle;
    m.timestamp_ms = 1000u;
    m.source_impl = src;
    return m;
}

/* A Stage 1-style estimate: suspect / PROVISIONAL_MODE_ID / presumed_fundamental. */
static inline void test_make_suspect_estimate(truing_tension_estimate_t *e, uint8_t cycle, float tension_n, float freq_hz)
{
    memset(e, 0, sizeof(*e));
    e->meta = test_meta(TRUING_STATUS_SUSPECT, TRUING_REASON_PROVISIONAL_MODE_ID, cycle, TRUING_SOURCE_SYNTHETIC);
    e->tension_n = tension_n;
    e->sigma_n = NAN;
    e->model_name = TRUING_TENSION_MODEL_IDEAL_STRING;
    e->model_version = 1u;
    e->frequency.meta = e->meta;
    e->frequency.selected_frequency_hz = freq_hz;
    e->frequency.mode_identity = TRUING_MODE_ID_PRESUMED_FUNDAMENTAL;
    e->frequency.n_candidates = 1u;
    e->frequency.candidates[0].frequency_hz = freq_hz;
    e->frequency.candidates[0].magnitude_db = -10.0f;
    e->frequency.candidates[0].prominence_db = 20.0f;
    e->frequency.snr_db = 30.0f;
    e->frequency.selection_rule_version = 1u;
}

/* A verification-grade estimate: valid with an independently confirmed fundamental. */
static inline void test_make_valid_estimate(truing_tension_estimate_t *e, uint8_t cycle, float tension_n, float freq_hz)
{
    test_make_suspect_estimate(e, cycle, tension_n, freq_hz);
    e->meta.status = TRUING_STATUS_VALID;
    e->meta.reason_code = TRUING_REASON_NONE;
    e->frequency.meta.status = TRUING_STATUS_VALID;
    e->frequency.meta.reason_code = TRUING_REASON_NONE;
    e->frequency.mode_identity = TRUING_MODE_ID_CONFIRMED_FUNDAMENTAL;
}

static inline void test_make_failed_estimate(truing_tension_estimate_t *e, uint8_t cycle, truing_status_t s, truing_reason_t r)
{
    memset(e, 0, sizeof(*e));
    e->meta = test_meta(s, r, cycle, TRUING_SOURCE_SYNTHETIC);
    e->tension_n = NAN;
    e->sigma_n = NAN;
    e->model_name = TRUING_TENSION_MODEL_IDEAL_STRING;
    e->model_version = 1u;
}

static inline void test_make_runout(truing_runout_measurement_t *m, uint8_t cycle, float angle, float lat, float rad)
{
    memset(m, 0, sizeof(*m));
    m->meta = test_meta(TRUING_STATUS_VALID, TRUING_REASON_NONE, cycle, TRUING_SOURCE_SYNTHETIC);
    m->rim_angle_rad = angle;
    m->lateral_mm = lat;
    m->radial_mm = rad;
}

#endif /* TRUING_TEST_RECORDS_H */
