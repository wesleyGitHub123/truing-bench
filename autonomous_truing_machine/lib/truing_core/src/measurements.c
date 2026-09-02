#include "truing/measurements.h"

#include <math.h>
#include <stddef.h>

static const char *const k_mode_str[TRUING_MODE_ID__COUNT] = {
    "unset", "confirmed_fundamental", "presumed_fundamental", "unknown",
};
static const char *const k_model_str[TRUING_TENSION_MODEL__COUNT] = {
    "unset", "ideal_string", "stiffness_corrected", "higher_mode", "empirical",
};

const char *truing_mode_identity_str(truing_mode_identity_t m)
{
    return (unsigned)m < TRUING_MODE_ID__COUNT ? k_mode_str[m] : "?";
}

const char *truing_tension_model_str(truing_tension_model_t m)
{
    return (unsigned)m < TRUING_TENSION_MODEL__COUNT ? k_model_str[m] : "?";
}

static bool has_value(truing_status_t s)
{
    /* Records in these states carry a numeric value that must be well-formed. */
    return s == TRUING_STATUS_VALID || s == TRUING_STATUS_SUSPECT;
}

truing_check_t truing_frequency_measurement_check(const truing_frequency_measurement_t *m)
{
    if (m == NULL) {
        return TRUING_CHECK_ERR_NULL;
    }
    truing_check_t c = truing_record_meta_check(&m->meta);
    if (c != TRUING_CHECK_OK) {
        return c;
    }
    if (m->n_candidates > TRUING_MAX_CANDIDATE_PEAKS) {
        return TRUING_CHECK_ERR_TOO_MANY_CANDIDATES;
    }
    if (has_value(m->meta.status)) {
        if (m->mode_identity == TRUING_MODE_ID_UNSET || (unsigned)m->mode_identity >= TRUING_MODE_ID__COUNT) {
            return TRUING_CHECK_ERR_MODE_IDENTITY_UNSET;
        }
        if (!isfinite(m->selected_frequency_hz) || !isfinite(m->snr_db)) {
            return TRUING_CHECK_ERR_NOT_FINITE;
        }
        if (m->selected_frequency_hz <= 0.0f) {
            return TRUING_CHECK_ERR_NOT_POSITIVE;
        }
        /* SPEC §4.4.1 / §6.3: a measurement whose mode identity is not independently
         * confirmed cannot be `valid`. */
        if (m->meta.status == TRUING_STATUS_VALID && m->mode_identity != TRUING_MODE_ID_CONFIRMED_FUNDAMENTAL) {
            return TRUING_CHECK_ERR_VALID_WITHOUT_CONFIRMED_MODE;
        }
        for (uint8_t i = 0; i < m->n_candidates; ++i) {
            if (!isfinite(m->candidates[i].frequency_hz) || m->candidates[i].frequency_hz <= 0.0f) {
                return TRUING_CHECK_ERR_NOT_FINITE;
            }
        }
    }
    return TRUING_CHECK_OK;
}

truing_check_t truing_tension_estimate_check(const truing_tension_estimate_t *e)
{
    if (e == NULL) {
        return TRUING_CHECK_ERR_NULL;
    }
    truing_check_t c = truing_record_meta_check(&e->meta);
    if (c != TRUING_CHECK_OK) {
        return c;
    }
    const truing_status_t s = e->meta.status;
    /* SPEC §6.3: model_name is mandatory wherever a value was produced or attempted. */
    if (s == TRUING_STATUS_VALID || s == TRUING_STATUS_SUSPECT || s == TRUING_STATUS_REJECTED) {
        if (e->model_name == TRUING_TENSION_MODEL_UNSET || (unsigned)e->model_name >= TRUING_TENSION_MODEL__COUNT) {
            return TRUING_CHECK_ERR_MODEL_UNSET;
        }
    }
    if (has_value(s)) {
        if (!isfinite(e->tension_n)) {
            return TRUING_CHECK_ERR_NOT_FINITE;
        }
        if (e->tension_n <= 0.0f) {
            /* SPEC §6.4: tension is tensile, always positive. A zero here is exactly the
             * "valid-looking number" §7.4 forbids. */
            return TRUING_CHECK_ERR_NOT_POSITIVE;
        }
        /* The frequency the estimate rests on must itself be a well-formed, value-bearing record. */
        c = truing_frequency_measurement_check(&e->frequency);
        if (c != TRUING_CHECK_OK || !has_value(e->frequency.meta.status)) {
            return TRUING_CHECK_ERR_EMBEDDED_RECORD;
        }
        /* SPEC §4.4.1: a valid estimate requires a valid, confirmed-mode frequency. */
        if (s == TRUING_STATUS_VALID && e->frequency.meta.status != TRUING_STATUS_VALID) {
            return TRUING_CHECK_ERR_VALID_WITHOUT_CONFIRMED_MODE;
        }
    } else if (e->frequency.meta.status != TRUING_STATUS_UNSET) {
        /* An embedded record, if present at all, must be self-consistent. */
        c = truing_frequency_measurement_check(&e->frequency);
        if (c != TRUING_CHECK_OK) {
            return TRUING_CHECK_ERR_EMBEDDED_RECORD;
        }
    }
    return TRUING_CHECK_OK;
}

truing_check_t truing_runout_measurement_check(const truing_runout_measurement_t *m)
{
    if (m == NULL) {
        return TRUING_CHECK_ERR_NULL;
    }
    truing_check_t c = truing_record_meta_check(&m->meta);
    if (c != TRUING_CHECK_OK) {
        return c;
    }
    /* SPEC §6.3: rim_angle is mandatory regardless of status. */
    if (!isfinite(m->rim_angle_rad)) {
        return TRUING_CHECK_ERR_NOT_FINITE;
    }
    if (m->rim_angle_rad < 0.0f || m->rim_angle_rad >= TRUING_TWO_PI) {
        return TRUING_CHECK_ERR_ANGLE_RANGE;
    }
    if (has_value(m->meta.status)) {
        if (!isfinite(m->lateral_mm) || !isfinite(m->radial_mm)) {
            return TRUING_CHECK_ERR_NOT_FINITE;
        }
    }
    return TRUING_CHECK_OK;
}
