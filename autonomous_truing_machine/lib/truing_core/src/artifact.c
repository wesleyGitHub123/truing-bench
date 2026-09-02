#include "truing/artifact.h"

#include <math.h>
#include <string.h>

#include "truing/sha256.h"
#include "truing/wheel_geometry.h"

static const char *const k_result_str[TRUING_ART__COUNT] = {
    "OK", "ERR_NULL", "ERR_TRUNCATED", "ERR_MAGIC", "ERR_VERSION", "ERR_INTEGRITY",
    "ERR_INCOMPATIBLE", "ERR_SHAPE", "ERR_CONDITIONING", "ERR_STRUCTURE",
};

const char *truing_artifact_result_str(truing_artifact_result_t r)
{
    return (unsigned)r < TRUING_ART__COUNT ? k_result_str[r] : "?";
}

/* ---- little-endian reader ---------------------------------------------------------- */
typedef struct {
    const uint8_t *p;
    size_t         len;
    size_t         pos;
    bool           underflow;
} reader_t;

static uint8_t rd_u8(reader_t *r)
{
    if (r->pos < r->len) {
        return r->p[r->pos++];
    }
    r->underflow = true;
    return 0u;
}

static uint16_t rd_u16(reader_t *r)
{
    const uint16_t lo = rd_u8(r);
    const uint16_t hi = rd_u8(r);
    return (uint16_t)(lo | (uint16_t)(hi << 8));
}

static uint32_t rd_u32(reader_t *r)
{
    const uint32_t lo = rd_u16(r);
    const uint32_t hi = rd_u16(r);
    return lo | (hi << 16);
}

static float rd_f32(reader_t *r)
{
    const uint32_t u = rd_u32(r);
    float f;
    memcpy(&f, &u, sizeof(f));
    return f;
}

static void rd_bytes(reader_t *r, uint8_t *dst, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        dst[i] = rd_u8(r);
    }
}

static bool weights_match(float artifact_value, float config_value)
{
    /* Both originate from the same decimal declaration; float32 equality is the contract. */
    return artifact_value == config_value;
}

const truing_artifact_layout_t *truing_artifact_layout(const truing_artifact_t *art, truing_layout_id_t layout)
{
    if (art == NULL || !art->loaded) {
        return NULL;
    }
    for (unsigned i = 0; i < art->n_layouts; ++i) {
        if (art->layouts[i].layout == layout) {
            return &art->layouts[i];
        }
    }
    return NULL;
}

void truing_fingerprint_hex(const truing_fingerprint_t *fp, char out[65])
{
    static const char digits[] = "0123456789abcdef";
    for (unsigned i = 0; i < TRUING_FINGERPRINT_BYTES; ++i) {
        out[2u * i] = digits[fp->bytes[i] >> 4];
        out[2u * i + 1u] = digits[fp->bytes[i] & 0x0Fu];
    }
    out[64] = '\0';
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool truing_fingerprint_from_hex(const char *hex, truing_fingerprint_t *out)
{
    if (hex == NULL || out == NULL) {
        return false;
    }
    for (unsigned i = 0; i < TRUING_FINGERPRINT_BYTES; ++i) {
        const int hi = hexval(hex[2u * i]);
        const int lo = (hi < 0) ? -1 : hexval(hex[2u * i + 1u]);
        if (hi < 0 || lo < 0) {
            out->set = false;
            return false;
        }
        out->bytes[i] = (uint8_t)((hi << 4) | lo);
    }
    if (hex[64] != '\0') {
        out->set = false;
        return false;
    }
    out->set = true;
    return true;
}

#define FAIL(code, what)        \
    do {                        \
        if (detail != NULL) {   \
            *detail = (what);   \
        }                       \
        out->loaded = false;    \
        return (code);          \
    } while (0)

truing_artifact_result_t truing_artifact_load(const uint8_t *buf, size_t len, const truing_wheel_class_config_t *wheel,
                                              const truing_solver_config_t *solver, truing_artifact_t *out,
                                              const char **detail)
{
    if (detail != NULL) {
        *detail = "";
    }
    if (out == NULL) {
        return TRUING_ART_ERR_NULL;
    }
    memset(out, 0, sizeof(*out));
    if (buf == NULL || wheel == NULL || solver == NULL) {
        FAIL(TRUING_ART_ERR_NULL, "null input");
    }
    if (len < TRUING_ARTIFACT_HEADER_BYTES) {
        FAIL(TRUING_ART_ERR_TRUNCATED, "shorter than the header");
    }
    reader_t r = { buf, len, 0u, false };
    if (rd_u32(&r) != TRUING_ARTIFACT_MAGIC) {
        FAIL(TRUING_ART_ERR_MAGIC, "magic");
    }
    if (rd_u16(&r) != TRUING_ARTIFACT_SCHEMA_VERSION) {
        FAIL(TRUING_ART_ERR_VERSION, "schema_version");
    }
    (void)rd_u16(&r);
    out->artifact_id = rd_u32(&r);
    rd_bytes(&r, (uint8_t *)out->name, TRUING_ARTIFACT_NAME_BYTES);
    out->name[TRUING_ARTIFACT_NAME_BYTES - 1u] = '\0';
    rd_bytes(&r, out->generating_fingerprint.bytes, TRUING_FINGERPRINT_BYTES);
    out->generating_fingerprint.set = true;
    rd_bytes(&r, out->content_hash, 32u);
    out->n_spokes = rd_u8(&r);
    out->n_rim_angles = rd_u8(&r);
    out->N_lat = rd_u8(&r);
    out->N_rad = rd_u8(&r);
    out->asymmetric = rd_u8(&r) != 0u;
    out->n_mt_identified = rd_u8(&r) != 0u;
    out->n_layouts = rd_u8(&r);
    out->T_target_present = rd_u8(&r) != 0u;
    const uint32_t payload_len = rd_u32(&r);
    if (r.pos != TRUING_ARTIFACT_HEADER_BYTES || (size_t)payload_len != len - TRUING_ARTIFACT_HEADER_BYTES) {
        FAIL(TRUING_ART_ERR_TRUNCATED, "payload length");
    }

    /* Check 1 — integrity (SPEC 8.7): the stored numbers are intact. */
    uint8_t digest[32];
    truing_sha256(buf + TRUING_ARTIFACT_HEADER_BYTES, payload_len, digest);
    if (memcmp(digest, out->content_hash, 32u) != 0) {
        FAIL(TRUING_ART_ERR_INTEGRITY, "content_hash");
    }
    /* Check 2 — compatibility against the INDEPENDENT expectation in the wheel-class config. */
    if (!wheel->expected_influence_fingerprint.set ||
        memcmp(wheel->expected_influence_fingerprint.bytes, out->generating_fingerprint.bytes, TRUING_FINGERPRINT_BYTES) != 0) {
        FAIL(TRUING_ART_ERR_INCOMPATIBLE, "generating_fingerprint != expected_influence_fingerprint");
    }
    /* Check 3 — shape against the active configuration; never reshape. */
    if (out->n_spokes != wheel->n_spokes || out->n_rim_angles != solver->n_rim_angles ||
        out->N_lat != solver->N_lat || out->N_rad != solver->N_rad) {
        FAIL(TRUING_ART_ERR_SHAPE, "n_spokes / n_rim_angles / N_lat / N_rad");
    }
    if (out->n_spokes > TRUING_MAX_SPOKES || out->n_rim_angles > TRUING_MAX_RIM_ANGLES ||
        out->N_lat > TRUING_MAX_FOURIER_ORDER || out->N_rad > TRUING_MAX_FOURIER_ORDER || out->n_layouts != 2u) {
        FAIL(TRUING_ART_ERR_SHAPE, "capacity bounds / layout count");
    }
    if (out->asymmetric != wheel->asymmetric) {
        FAIL(TRUING_ART_ERR_SHAPE, "asymmetric flag");
    }
    const uint8_t n = out->n_spokes;
    const uint8_t nra = out->n_rim_angles;

    /* ---- payload ---- */
    out->tol_lateral_mm = rd_f32(&r);
    out->tol_radial_mm = rd_f32(&r);
    out->tol_tension_n = rd_f32(&r);
    out->trust_radial = rd_f32(&r);
    out->trust_tension = rd_f32(&r);
    out->c_side_a = rd_f32(&r);
    out->c_side_b = rd_f32(&r);
    out->cond_i_residual = rd_f32(&r);
    out->cond_i_tolerance = rd_f32(&r);
    out->cond_ii_residual = rd_f32(&r);
    out->cond_ii_tolerance = rd_f32(&r);
    out->uniqueness_margin = rd_f32(&r);
    out->uniqueness_threshold = rd_f32(&r);
    out->displacement_null_dim = rd_u8(&r);
    if (out->T_target_present) {
        for (uint8_t i = 0; i < n; ++i) {
            out->T_target_assumed[i] = rd_f32(&r);
        }
    }
    if (out->n_mt_identified) {
        for (uint8_t i = 0; i < n; ++i) {
            out->n_mt[i] = rd_f32(&r);
        }
    }
    const unsigned nc_lat = 2u * out->N_lat + 1u, nc_rad = 2u * out->N_rad + 1u;
    for (uint8_t i = 0; i < n; ++i) {
        for (unsigned k = 0; k < nc_lat; ++k) {
            out->coeffs_lat[i][k] = rd_f32(&r);
        }
    }
    for (uint8_t i = 0; i < n; ++i) {
        for (unsigned k = 0; k < nc_rad; ++k) {
            out->coeffs_rad[i][k] = rd_f32(&r);
        }
    }
    for (uint8_t j = 0; j < n; ++j) {
        for (uint8_t i = 0; i < n; ++i) {
            out->phi_t[j][i] = rd_f32(&r);
        }
    }
    rd_bytes(&r, out->class_map, n);
    truing_row_dims_t dims;
    if (!truing_row_dims_init(&dims, n, nra)) {
        FAIL(TRUING_ART_ERR_SHAPE, "row dims");
    }
    for (unsigned li = 0; li < out->n_layouts; ++li) {
        truing_artifact_layout_t *L = &out->layouts[li];
        const uint8_t code = rd_u8(&r);
        L->layout = code == 1u ? TRUING_LAYOUT_FULL : (code == 2u ? TRUING_LAYOUT_TENSION_ABSENT : TRUING_LAYOUT_NONE);
        L->effective_rank = rd_u8(&r);
        L->expected_null_dim = rd_u8(&r);
        (void)rd_u8(&r);
        L->n_rows = rd_u16(&r);
        (void)rd_u16(&r);
        L->effective_condition_number = rd_f32(&r);
        for (unsigned w = 0; w < TRUING_ROW_MASK_WORDS; ++w) {
            L->row_mask.words[w] = rd_u32(&r);
        }
        if (L->n_rows > TRUING_MAX_FULL_ROWS) {
            FAIL(TRUING_ART_ERR_SHAPE, "layout rows beyond capacity");
        }
        for (uint8_t i = 0; i < n; ++i) {
            for (uint16_t c = 0; c < L->n_rows; ++c) {
                L->pinv[i][c] = rd_f32(&r);
            }
        }
    }
    if (r.underflow || r.pos != len) {
        FAIL(TRUING_ART_ERR_TRUNCATED, "payload does not match the schema");
    }

    /* Shape check continued: layouts and their masks (SPEC 8.7.1 Phi-dagger <-> row set binding). */
    for (unsigned li = 0; li < out->n_layouts; ++li) {
        const truing_artifact_layout_t *L = &out->layouts[li];
        truing_row_mask_t expected;
        if (L->layout == TRUING_LAYOUT_NONE || !truing_row_mask_for_layout(L->layout, &dims, &expected) ||
            !truing_row_mask_equals(&expected, &L->row_mask) || truing_row_mask_popcount(&L->row_mask) != L->n_rows) {
            FAIL(TRUING_ART_ERR_SHAPE, "layout row mask");
        }
        if ((unsigned)L->effective_rank + (unsigned)L->expected_null_dim != n) {
            FAIL(TRUING_ART_ERR_STRUCTURE, "effective_rank + expected_null_dim != n_spokes");
        }
        /* R3 (SPEC 8.11): recorded conditioning against the configured limit. */
        if (!(L->effective_condition_number <= solver->max_condition_number)) {
            FAIL(TRUING_ART_ERR_CONDITIONING, "effective_condition_number > max_condition_number");
        }
    }
    if (truing_artifact_layout(out, TRUING_LAYOUT_FULL) == NULL && out->layouts[0].layout != TRUING_LAYOUT_FULL) {
        /* truing_artifact_layout requires loaded=true; check codes directly. */
    }
    if (!((out->layouts[0].layout == TRUING_LAYOUT_FULL && out->layouts[1].layout == TRUING_LAYOUT_TENSION_ABSENT) ||
          (out->layouts[1].layout == TRUING_LAYOUT_FULL && out->layouts[0].layout == TRUING_LAYOUT_TENSION_ABSENT))) {
        FAIL(TRUING_ART_ERR_SHAPE, "FULL and TENSION_ABSENT layouts required");
    }
    /* Weights and tolerances are baked into every Phi-dagger: they must equal the configuration. */
    if (!weights_match(out->tol_lateral_mm, solver->tol_lateral_mm) || !weights_match(out->tol_radial_mm, solver->tol_radial_mm) ||
        !weights_match(out->tol_tension_n, solver->tol_tension_n) || !weights_match(out->trust_radial, solver->trust_radial) ||
        !weights_match(out->trust_tension, solver->trust_tension)) {
        FAIL(TRUING_ART_ERR_SHAPE, "weights/tolerances differ from the solver configuration");
    }
    if (!weights_match(out->c_side_a, wheel->c_side_a_n_per_rev) || !weights_match(out->c_side_b, wheel->c_side_b_n_per_rev)) {
        FAIL(TRUING_ART_ERR_SHAPE, "c_side values differ from the wheel-class configuration");
    }
    /* Structure: normalisation and the symmetric common-mode direction (SPEC 11.2, 8.4). */
    if (out->T_target_present) {
        float sum = 0.0f;
        for (uint8_t i = 0; i < n; ++i) {
            sum += out->T_target_assumed[i];
        }
        if (fabsf(sum / (float)n - 1.0f) > 1e-5f) {
            FAIL(TRUING_ART_ERR_STRUCTURE, "T_target_assumed not normalised to mean 1.0");
        }
    }
    if (out->n_mt_identified) {
        if (!out->T_target_present) {
            FAIL(TRUING_ART_ERR_STRUCTURE, "n_mt identified without T_target_assumed");
        }
        if (!out->asymmetric) {
            const float ref = 1.0f / sqrtf((float)n);
            for (uint8_t i = 0; i < n; ++i) {
                if (fabsf(out->n_mt[i] - ref) > 1e-6f) {
                    FAIL(TRUING_ART_ERR_STRUCTURE, "symmetric wheel: n_mt is not [1..1]/sqrt(n)");
                }
            }
        }
    }

    /* Expand the displacement blocks on the rim grid (SPEC 11.4). */
    for (uint8_t k = 0; k < nra; ++k) {
        const float theta = truing_rim_index_angle(nra, k);
        for (uint8_t i = 0; i < n; ++i) {
            float u = out->coeffs_lat[i][0];
            for (unsigned m = 1; m <= out->N_lat; ++m) {
                u += out->coeffs_lat[i][2u * m - 1u] * cosf((float)m * theta) + out->coeffs_lat[i][2u * m] * sinf((float)m * theta);
            }
            float v = out->coeffs_rad[i][0];
            for (unsigned m = 1; m <= out->N_rad; ++m) {
                v += out->coeffs_rad[i][2u * m - 1u] * cosf((float)m * theta) + out->coeffs_rad[i][2u * m] * sinf((float)m * theta);
            }
            out->phi_u[k][i] = u;
            out->phi_v[k][i] = v;
        }
    }
    out->loaded = true;
    return TRUING_ART_OK;
}
