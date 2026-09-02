#include "truing/config_blob.h"

#include <string.h>

#include "truing/crc32.h"

static const char *const k_result_str[TRUING_BLOB__COUNT] = {
    "OK", "ERR_NULL", "ERR_CAPACITY", "ERR_TRUNCATED", "ERR_MAGIC",
    "ERR_VERSION", "ERR_KIND", "ERR_CRC", "ERR_PAYLOAD_LENGTH",
};
static const char *const k_kind_str[TRUING_BLOB_KIND__COUNT] = {
    "UNSET", "WHEEL_CLASS", "SOLVER", "CHAIN_PROFILE", "TENSION_MODEL_PROFILE", "MACHINE_PROFILE",
};

const char *truing_blob_result_str(truing_blob_result_t r)
{
    return (unsigned)r < TRUING_BLOB__COUNT ? k_result_str[r] : "?";
}

const char *truing_blob_kind_str(truing_blob_kind_t k)
{
    return (unsigned)k < TRUING_BLOB_KIND__COUNT ? k_kind_str[k] : "?";
}

/* ---- writer ------------------------------------------------------------------- */
typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   pos;
    bool     overflow;
} writer_t;

static void put_u8(writer_t *w, uint8_t v)
{
    if (w->pos < w->cap) {
        w->buf[w->pos] = v;
    } else {
        w->overflow = true;
    }
    w->pos++;
}

static void put_u16(writer_t *w, uint16_t v)
{
    put_u8(w, (uint8_t)(v & 0xFFu));
    put_u8(w, (uint8_t)(v >> 8));
}

static void put_u32(writer_t *w, uint32_t v)
{
    put_u16(w, (uint16_t)(v & 0xFFFFu));
    put_u16(w, (uint16_t)(v >> 16));
}

static void put_f32(writer_t *w, float f)
{
    uint32_t u;
    memcpy(&u, &f, sizeof(u));
    put_u32(w, u);
}

static void put_bool(writer_t *w, bool b)
{
    put_u8(w, b ? 1u : 0u);
}

static void put_bytes(writer_t *w, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        put_u8(w, p[i]);
    }
}

static void put_established(writer_t *w, const truing_established_param_t *p)
{
    put_f32(w, p->value);
    put_u8(w, (uint8_t)p->provenance);
}

static void put_assumptions(writer_t *w, const truing_model_assumptions_t *a)
{
    put_f32(w, a->spoke_diameter_mm);
    put_f32(w, a->spoke_material_modulus_pa);
    put_f32(w, a->spoke_density_kg_m3);
}

static bool writer_begin(writer_t *w, uint8_t *buf, size_t cap)
{
    if (buf == NULL) {
        return false;
    }
    w->buf = buf;
    w->cap = cap;
    w->pos = TRUING_BLOB_HEADER_BYTES;   /* header is patched in by writer_finish */
    w->overflow = cap < TRUING_BLOB_HEADER_BYTES;
    return true;
}

static size_t writer_finish(writer_t *w, truing_blob_kind_t kind)
{
    if (w->overflow) {
        return 0u;
    }
    const size_t payload_len = w->pos - TRUING_BLOB_HEADER_BYTES;
    if (payload_len > 0xFFFFu) {
        return 0u;
    }
    const uint32_t crc = truing_crc32(w->buf + TRUING_BLOB_HEADER_BYTES, payload_len);
    writer_t h = { w->buf, w->cap, 0u, false };
    put_u32(&h, TRUING_BLOB_MAGIC);
    put_u16(&h, (uint16_t)TRUING_BLOB_SCHEMA_VERSION);
    put_u8(&h, (uint8_t)kind);
    put_u8(&h, 0u);
    put_u16(&h, (uint16_t)payload_len);
    put_u32(&h, crc);
    return w->pos;
}

/* ---- reader ------------------------------------------------------------------- */
typedef struct {
    const uint8_t *buf;
    size_t         len;
    size_t         pos;
    bool           underflow;
} reader_t;

static uint8_t get_u8(reader_t *r)
{
    if (r->pos < r->len) {
        return r->buf[r->pos++];
    }
    r->underflow = true;
    return 0u;
}

static uint16_t get_u16(reader_t *r)
{
    const uint16_t lo = get_u8(r);
    const uint16_t hi = get_u8(r);
    return (uint16_t)(lo | (uint16_t)(hi << 8));
}

static uint32_t get_u32(reader_t *r)
{
    const uint32_t lo = get_u16(r);
    const uint32_t hi = get_u16(r);
    return lo | (hi << 16);
}

static float get_f32(reader_t *r)
{
    const uint32_t u = get_u32(r);
    float f;
    memcpy(&f, &u, sizeof(f));
    return f;
}

static bool get_bool(reader_t *r)
{
    return get_u8(r) != 0u;
}

static void get_bytes(reader_t *r, uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        p[i] = get_u8(r);
    }
}

static void get_established(reader_t *r, truing_established_param_t *p)
{
    p->value = get_f32(r);
    p->provenance = (truing_param_provenance_t)get_u8(r);
}

static void get_assumptions(reader_t *r, truing_model_assumptions_t *a)
{
    a->spoke_diameter_mm = get_f32(r);
    a->spoke_material_modulus_pa = get_f32(r);
    a->spoke_density_kg_m3 = get_f32(r);
}

truing_blob_result_t truing_blob_peek(const uint8_t *buf, size_t len, truing_blob_kind_t *kind_out,
                                      uint16_t *payload_len_out, uint32_t *crc_out)
{
    if (buf == NULL) {
        return TRUING_BLOB_ERR_NULL;
    }
    if (len < TRUING_BLOB_HEADER_BYTES) {
        return TRUING_BLOB_ERR_TRUNCATED;
    }
    reader_t h = { buf, TRUING_BLOB_HEADER_BYTES, 0u, false };
    const uint32_t magic = get_u32(&h);
    const uint16_t version = get_u16(&h);
    const uint8_t kind = get_u8(&h);
    (void)get_u8(&h);   /* reserved */
    const uint16_t payload_len = get_u16(&h);
    const uint32_t crc = get_u32(&h);
    if (magic != TRUING_BLOB_MAGIC) {
        return TRUING_BLOB_ERR_MAGIC;
    }
    if (version != TRUING_BLOB_SCHEMA_VERSION) {
        return TRUING_BLOB_ERR_VERSION;
    }
    if (kind == TRUING_BLOB_KIND_UNSET || kind >= TRUING_BLOB_KIND__COUNT) {
        return TRUING_BLOB_ERR_KIND;
    }
    if (len < TRUING_BLOB_HEADER_BYTES + (size_t)payload_len) {
        return TRUING_BLOB_ERR_TRUNCATED;
    }
    if (len > TRUING_BLOB_HEADER_BYTES + (size_t)payload_len) {
        return TRUING_BLOB_ERR_PAYLOAD_LENGTH;
    }
    if (truing_crc32(buf + TRUING_BLOB_HEADER_BYTES, payload_len) != crc) {
        return TRUING_BLOB_ERR_CRC;
    }
    if (kind_out != NULL) {
        *kind_out = (truing_blob_kind_t)kind;
    }
    if (payload_len_out != NULL) {
        *payload_len_out = payload_len;
    }
    if (crc_out != NULL) {
        *crc_out = crc;
    }
    return TRUING_BLOB_OK;
}

static truing_blob_result_t reader_begin(reader_t *r, const uint8_t *buf, size_t len, truing_blob_kind_t expected)
{
    truing_blob_kind_t kind = TRUING_BLOB_KIND_UNSET;
    uint16_t payload_len = 0u;
    const truing_blob_result_t res = truing_blob_peek(buf, len, &kind, &payload_len, NULL);
    if (res != TRUING_BLOB_OK) {
        return res;
    }
    if (kind != expected) {
        return TRUING_BLOB_ERR_KIND;
    }
    r->buf = buf + TRUING_BLOB_HEADER_BYTES;
    r->len = payload_len;
    r->pos = 0u;
    r->underflow = false;
    return TRUING_BLOB_OK;
}

static truing_blob_result_t reader_finish(const reader_t *r)
{
    if (r->underflow) {
        return TRUING_BLOB_ERR_TRUNCATED;
    }
    if (r->pos != r->len) {
        return TRUING_BLOB_ERR_PAYLOAD_LENGTH;
    }
    return TRUING_BLOB_OK;
}

/* ---- wheel class ---------------------------------------------------------------- */
size_t truing_blob_encode_wheel_class(const truing_wheel_class_config_t *cfg, uint8_t *buf, size_t cap)
{
    writer_t w;
    if (cfg == NULL || !writer_begin(&w, buf, cap)) {
        return 0u;
    }
    put_u8(&w, cfg->n_spokes);
    put_u8(&w, cfg->n_cross);
    put_f32(&w, cfg->rim_diameter_mm);
    put_f32(&w, cfg->spoke_diameter_mm);
    put_f32(&w, cfg->spoke_material_modulus_pa);
    put_bool(&w, cfg->asymmetric);
    put_u8(&w, (uint8_t)cfg->side_a_basis);
    put_f32(&w, cfg->hub_side_a.flange_diameter_mm);
    put_f32(&w, cfg->hub_side_a.flange_offset_mm);
    put_f32(&w, cfg->hub_side_b.flange_diameter_mm);
    put_f32(&w, cfg->hub_side_b.flange_offset_mm);
    put_f32(&w, cfg->c_side_a_n_per_rev);
    put_f32(&w, cfg->c_side_b_n_per_rev);
    put_u8(&w, (uint8_t)cfg->indexing_origin.side);
    put_u8(&w, (uint8_t)cfg->indexing_origin.lead_trail);
    put_f32(&w, cfg->symmetry_angle_tolerance_rad);
    put_bool(&w, cfg->expected_influence_fingerprint.set);
    put_bytes(&w, cfg->expected_influence_fingerprint.bytes, TRUING_FINGERPRINT_BYTES);
    put_assumptions(&w, &cfg->compatible_model_assumptions);
    return writer_finish(&w, TRUING_BLOB_KIND_WHEEL_CLASS);
}

truing_blob_result_t truing_blob_decode_wheel_class(const uint8_t *buf, size_t len, truing_wheel_class_config_t *out)
{
    if (out == NULL) {
        return TRUING_BLOB_ERR_NULL;
    }
    reader_t r;
    truing_blob_result_t res = reader_begin(&r, buf, len, TRUING_BLOB_KIND_WHEEL_CLASS);
    if (res != TRUING_BLOB_OK) {
        return res;
    }
    truing_wheel_class_config_t c;
    memset(&c, 0, sizeof(c));
    c.n_spokes = get_u8(&r);
    c.n_cross = get_u8(&r);
    c.rim_diameter_mm = get_f32(&r);
    c.spoke_diameter_mm = get_f32(&r);
    c.spoke_material_modulus_pa = get_f32(&r);
    c.asymmetric = get_bool(&r);
    c.side_a_basis = (truing_side_a_basis_t)get_u8(&r);
    c.hub_side_a.flange_diameter_mm = get_f32(&r);
    c.hub_side_a.flange_offset_mm = get_f32(&r);
    c.hub_side_b.flange_diameter_mm = get_f32(&r);
    c.hub_side_b.flange_offset_mm = get_f32(&r);
    c.c_side_a_n_per_rev = get_f32(&r);
    c.c_side_b_n_per_rev = get_f32(&r);
    c.indexing_origin.side = (truing_side_t)get_u8(&r);
    c.indexing_origin.lead_trail = (truing_lead_trail_t)get_u8(&r);
    c.symmetry_angle_tolerance_rad = get_f32(&r);
    c.expected_influence_fingerprint.set = get_bool(&r);
    get_bytes(&r, c.expected_influence_fingerprint.bytes, TRUING_FINGERPRINT_BYTES);
    get_assumptions(&r, &c.compatible_model_assumptions);
    res = reader_finish(&r);
    if (res == TRUING_BLOB_OK) {
        *out = c;
    }
    return res;
}

/* ---- solver ---------------------------------------------------------------------- */
size_t truing_blob_encode_solver(const truing_solver_config_t *cfg, uint8_t *buf, size_t cap)
{
    writer_t w;
    if (cfg == NULL || !writer_begin(&w, buf, cap)) {
        return 0u;
    }
    put_f32(&w, cfg->tol_lateral_mm);
    put_f32(&w, cfg->tol_radial_mm);
    put_f32(&w, cfg->tol_tension_n);
    put_f32(&w, cfg->tol_tension_cv);
    put_f32(&w, cfg->tol_mean_tension_error_n);
    put_f32(&w, cfg->tol_angular_rad);
    put_f32(&w, cfg->max_condition_number);
    put_f32(&w, cfg->rank_tolerance);
    put_f32(&w, cfg->n_mt_displacement_tolerance);
    put_f32(&w, cfg->n_mt_tension_tolerance);
    put_f32(&w, cfg->n_mt_uniqueness_threshold);
    put_f32(&w, cfg->max_adjustment_revolutions);
    put_u8(&w, cfg->partial_state_remeasure_attempts);
    put_f32(&w, cfg->trust_radial);
    put_f32(&w, cfg->trust_tension);
    put_u8(&w, cfg->N_lat);
    put_u8(&w, cfg->N_rad);
    put_u8(&w, cfg->n_rim_angles);
    put_f32(&w, cfg->target_tension_n);
    put_u32(&w, cfg->tension_model_profile_id);
    put_f32(&w, cfg->adjustment_deadband_rev);
    put_f32(&w, cfg->lateral_deadband_mm);
    put_u8(&w, cfg->max_cycles);
    put_f32(&w, cfg->min_relative_improvement);
    put_u8(&w, cfg->consecutive_non_improving_cycles);
    put_u8(&w, cfg->measurement_retry_count);
    put_u16(&w, cfg->excitation_settle_ms);
    return writer_finish(&w, TRUING_BLOB_KIND_SOLVER);
}

truing_blob_result_t truing_blob_decode_solver(const uint8_t *buf, size_t len, truing_solver_config_t *out)
{
    if (out == NULL) {
        return TRUING_BLOB_ERR_NULL;
    }
    reader_t r;
    truing_blob_result_t res = reader_begin(&r, buf, len, TRUING_BLOB_KIND_SOLVER);
    if (res != TRUING_BLOB_OK) {
        return res;
    }
    truing_solver_config_t c;
    memset(&c, 0, sizeof(c));
    c.tol_lateral_mm = get_f32(&r);
    c.tol_radial_mm = get_f32(&r);
    c.tol_tension_n = get_f32(&r);
    c.tol_tension_cv = get_f32(&r);
    c.tol_mean_tension_error_n = get_f32(&r);
    c.tol_angular_rad = get_f32(&r);
    c.max_condition_number = get_f32(&r);
    c.rank_tolerance = get_f32(&r);
    c.n_mt_displacement_tolerance = get_f32(&r);
    c.n_mt_tension_tolerance = get_f32(&r);
    c.n_mt_uniqueness_threshold = get_f32(&r);
    c.max_adjustment_revolutions = get_f32(&r);
    c.partial_state_remeasure_attempts = get_u8(&r);
    c.trust_radial = get_f32(&r);
    c.trust_tension = get_f32(&r);
    c.N_lat = get_u8(&r);
    c.N_rad = get_u8(&r);
    c.n_rim_angles = get_u8(&r);
    c.target_tension_n = get_f32(&r);
    c.tension_model_profile_id = get_u32(&r);
    c.adjustment_deadband_rev = get_f32(&r);
    c.lateral_deadband_mm = get_f32(&r);
    c.max_cycles = get_u8(&r);
    c.min_relative_improvement = get_f32(&r);
    c.consecutive_non_improving_cycles = get_u8(&r);
    c.measurement_retry_count = get_u8(&r);
    c.excitation_settle_ms = get_u16(&r);
    res = reader_finish(&r);
    if (res == TRUING_BLOB_OK) {
        *out = c;
    }
    return res;
}

/* ---- chain profile ---------------------------------------------------------------- */
size_t truing_blob_encode_chain_profile(const truing_chain_profile_t *p, uint8_t *buf, size_t cap)
{
    writer_t w;
    if (p == NULL || !writer_begin(&w, buf, cap)) {
        return 0u;
    }
    put_u32(&w, p->chain_id);
    put_u8(&w, (uint8_t)p->transducer);
    put_u32(&w, p->sample_rate_hz);
    put_u8(&w, p->bit_depth);
    put_f32(&w, p->noise_floor_dbfs);
    put_f32(&w, p->preflight_min_snr_db);
    put_f32(&w, p->preflight_max_noise_floor_dbfs);
    put_f32(&w, p->f1_band_lo_hz);
    put_f32(&w, p->f1_band_hi_hz);
    put_f32(&w, p->window_ms);
    put_f32(&w, p->gate_start_ms);
    return writer_finish(&w, TRUING_BLOB_KIND_CHAIN_PROFILE);
}

truing_blob_result_t truing_blob_decode_chain_profile(const uint8_t *buf, size_t len, truing_chain_profile_t *out)
{
    if (out == NULL) {
        return TRUING_BLOB_ERR_NULL;
    }
    reader_t r;
    truing_blob_result_t res = reader_begin(&r, buf, len, TRUING_BLOB_KIND_CHAIN_PROFILE);
    if (res != TRUING_BLOB_OK) {
        return res;
    }
    truing_chain_profile_t c;
    memset(&c, 0, sizeof(c));
    c.chain_id = get_u32(&r);
    c.transducer = (truing_transducer_t)get_u8(&r);
    c.sample_rate_hz = get_u32(&r);
    c.bit_depth = get_u8(&r);
    c.noise_floor_dbfs = get_f32(&r);
    c.preflight_min_snr_db = get_f32(&r);
    c.preflight_max_noise_floor_dbfs = get_f32(&r);
    c.f1_band_lo_hz = get_f32(&r);
    c.f1_band_hi_hz = get_f32(&r);
    c.window_ms = get_f32(&r);
    c.gate_start_ms = get_f32(&r);
    res = reader_finish(&r);
    if (res == TRUING_BLOB_OK) {
        *out = c;
    }
    return res;
}

/* ---- tension model profile ------------------------------------------------------- */
size_t truing_blob_encode_tension_model_profile(const truing_tension_model_profile_t *p, uint8_t *buf, size_t cap)
{
    writer_t w;
    if (p == NULL || !writer_begin(&w, buf, cap)) {
        return 0u;
    }
    put_u32(&w, p->profile_id);
    put_u8(&w, (uint8_t)p->model_name);
    put_u16(&w, p->model_version);
    put_established(&w, &p->L_eff_m);
    put_established(&w, &p->linear_density_kg_per_m);
    put_established(&w, &p->bending_stiffness_n_m2);
    put_established(&w, &p->nominal_free_span_m);
    put_established(&w, &p->empirical_a);
    put_established(&w, &p->empirical_n);
    put_assumptions(&w, &p->assumptions);
    return writer_finish(&w, TRUING_BLOB_KIND_TENSION_MODEL_PROFILE);
}

truing_blob_result_t truing_blob_decode_tension_model_profile(const uint8_t *buf, size_t len,
                                                              truing_tension_model_profile_t *out)
{
    if (out == NULL) {
        return TRUING_BLOB_ERR_NULL;
    }
    reader_t r;
    truing_blob_result_t res = reader_begin(&r, buf, len, TRUING_BLOB_KIND_TENSION_MODEL_PROFILE);
    if (res != TRUING_BLOB_OK) {
        return res;
    }
    truing_tension_model_profile_t c;
    memset(&c, 0, sizeof(c));
    c.profile_id = get_u32(&r);
    c.model_name = (truing_tension_model_t)get_u8(&r);
    c.model_version = get_u16(&r);
    get_established(&r, &c.L_eff_m);
    get_established(&r, &c.linear_density_kg_per_m);
    get_established(&r, &c.bending_stiffness_n_m2);
    get_established(&r, &c.nominal_free_span_m);
    get_established(&r, &c.empirical_a);
    get_established(&r, &c.empirical_n);
    get_assumptions(&r, &c.assumptions);
    res = reader_finish(&r);
    if (res == TRUING_BLOB_OK) {
        *out = c;
    }
    return res;
}

/* ---- machine profile (station geometry) ------------------------------------------ */
/* Payload: u32 profile_id; u8 reference_station; then for every station id 1..COUNT-1:
 *          u8 present, f32 angle_rad, f32 positioning_tolerance_rad. */
size_t truing_blob_encode_machine_profile(const truing_machine_profile_t *p, uint8_t *buf, size_t cap)
{
    writer_t w;
    if (p == NULL || !writer_begin(&w, buf, cap)) {
        return 0u;
    }
    put_u32(&w, p->profile_id);
    put_u8(&w, (uint8_t)p->reference_station);
    for (unsigned id = 1u; id < TRUING_STATION__COUNT; ++id) {
        put_bool(&w, p->stations[id].present);
        put_f32(&w, p->stations[id].angle_rad);
        put_f32(&w, p->stations[id].positioning_tolerance_rad);
    }
    return writer_finish(&w, TRUING_BLOB_KIND_MACHINE_PROFILE);
}

truing_blob_result_t truing_blob_decode_machine_profile(const uint8_t *buf, size_t len, truing_machine_profile_t *out)
{
    if (out == NULL) {
        return TRUING_BLOB_ERR_NULL;
    }
    reader_t r;
    truing_blob_result_t res = reader_begin(&r, buf, len, TRUING_BLOB_KIND_MACHINE_PROFILE);
    if (res != TRUING_BLOB_OK) {
        return res;
    }
    truing_machine_profile_t c;
    memset(&c, 0, sizeof(c));
    c.profile_id = get_u32(&r);
    c.reference_station = (truing_station_id_t)get_u8(&r);
    for (unsigned id = 1u; id < TRUING_STATION__COUNT; ++id) {
        c.stations[id].present = get_bool(&r);
        c.stations[id].angle_rad = get_f32(&r);
        c.stations[id].positioning_tolerance_rad = get_f32(&r);
    }
    res = reader_finish(&r);
    if (res == TRUING_BLOB_OK) {
        *out = c;
    }
    return res;
}
