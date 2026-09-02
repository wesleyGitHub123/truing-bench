#include "truing_dsp/tension_model.h"

#include <math.h>
#include <string.h>

#define PI_F 3.14159265358979323846f

static bool established(const truing_established_param_t *p)
{
    return p->provenance != TRUING_PROVENANCE_UNESTABLISHED && isfinite(p->value);
}

static truing_tension_result_t refuse(truing_reason_t r)
{
    truing_tension_result_t out;
    memset(&out, 0, sizeof(out));
    out.ok = false;
    out.reason = r;
    out.tension_n = NAN;
    out.l_eff_m = NAN;
    out.correction_n = NAN;
    return out;
}

truing_tension_result_t truing_tension_model_apply(const truing_tension_model_profile_t *p, float f1_hz, float f2_hz)
{
    if (p == NULL) {
        return refuse(TRUING_REASON_CALIBRATION_MISSING);
    }
    uint32_t missing = 0u;
    const char *field = NULL;
    if (truing_tension_model_profile_check(p, &missing, &field) != TRUING_CFG_OK) {
        return refuse(TRUING_REASON_CALIBRATION_MISSING);
    }
    if (!isfinite(f1_hz) || f1_hz <= 0.0f) {
        return refuse(TRUING_REASON_FREQ_OUT_OF_RANGE);
    }
    truing_tension_result_t out;
    memset(&out, 0, sizeof(out));
    out.l_eff_m = NAN;
    out.correction_n = NAN;
    switch (p->model_name) {
    case TRUING_TENSION_MODEL_IDEAL_STRING: {
        const float mu = p->linear_density_kg_per_m.value, L = p->L_eff_m.value;
        if (!established(&p->linear_density_kg_per_m) || !established(&p->L_eff_m) || mu <= 0.0f || L <= 0.0f) {
            return refuse(TRUING_REASON_CALIBRATION_MISSING);
        }
        out.tension_n = 4.0f * mu * L * L * f1_hz * f1_hz;
        out.l_eff_m = L;
        break;
    }
    case TRUING_TENSION_MODEL_STIFFNESS_CORRECTED: {
        const float mu = p->linear_density_kg_per_m.value, L = p->L_eff_m.value, ei = p->bending_stiffness_n_m2.value;
        if (!established(&p->linear_density_kg_per_m) || !established(&p->L_eff_m) || !established(&p->bending_stiffness_n_m2) ||
            mu <= 0.0f || L <= 0.0f || ei <= 0.0f) {
            return refuse(TRUING_REASON_CALIBRATION_MISSING);
        }
        const float ideal = 4.0f * mu * L * L * f1_hz * f1_hz;
        const float corr = PI_F * PI_F * ei / (L * L);
        out.correction_n = corr;
        out.l_eff_m = L;
        out.tension_n = ideal - corr;
        if (out.tension_n <= 0.0f) {
            truing_tension_result_t r = refuse(TRUING_REASON_MODEL_REJECTED);
            r.correction_n = corr;
            return r;
        }
        break;
    }
    case TRUING_TENSION_MODEL_HIGHER_MODE: {
        const float mu = p->linear_density_kg_per_m.value, ei = p->bending_stiffness_n_m2.value, nominal = p->nominal_free_span_m.value;
        if (!established(&p->linear_density_kg_per_m) || !established(&p->bending_stiffness_n_m2) || !established(&p->nominal_free_span_m) ||
            mu <= 0.0f || ei <= 0.0f || nominal <= 0.0f) {
            return refuse(TRUING_REASON_CALIBRATION_MISSING);
        }
        if (!isfinite(f2_hz) || f2_hz <= 0.0f) {
            return refuse(TRUING_REASON_NO_F2_PARTNER);
        }
        const float b = ((f2_hz / 2.0f) * (f2_hz / 2.0f) - f1_hz * f1_hz) / 3.0f;
        if (!isfinite(b) || b <= 0.0f) {
            return refuse(TRUING_REASON_MODEL_REJECTED);        /* sub-harmonic f2: no physical stiffness */
        }
        const float k1 = 4.0f * mu * b / (PI_F * PI_F);
        if (k1 <= 0.0f) {
            return refuse(TRUING_REASON_MODEL_REJECTED);
        }
        const float l_eff = powf(ei / k1, 0.25f);
        const float a = f1_hz * f1_hz - b;
        if (!isfinite(l_eff) || l_eff <= 0.0f || a <= 0.0f) {
            return refuse(TRUING_REASON_MODEL_REJECTED);
        }
        const float t = 4.0f * mu * a * l_eff * l_eff;
        if (!isfinite(t) || t <= 0.0f) {
            return refuse(TRUING_REASON_MODEL_REJECTED);
        }
        if (l_eff < p->l_eff_bounds_lo * nominal || l_eff > p->l_eff_bounds_hi * nominal) {
            truing_tension_result_t r = refuse(TRUING_REASON_MODEL_REJECTED);   /* l_eff_out_of_bounds */
            r.l_eff_m = l_eff;
            return r;
        }
        out.tension_n = t;
        out.l_eff_m = l_eff;
        break;
    }
    case TRUING_TENSION_MODEL_EMPIRICAL: {
        const float a = p->empirical_a.value, nn = p->empirical_n.value;
        if (!established(&p->empirical_a) || !established(&p->empirical_n) || a <= 0.0f) {
            return refuse(TRUING_REASON_CALIBRATION_MISSING);
        }
        out.tension_n = a * powf(f1_hz, nn);
        break;
    }
    default:
        return refuse(TRUING_REASON_CALIBRATION_MISSING);
    }
    if (!isfinite(out.tension_n) || out.tension_n <= 0.0f) {
        return refuse(TRUING_REASON_MODEL_REJECTED);
    }
    out.ok = true;
    out.reason = TRUING_REASON_NONE;
    return out;
}
