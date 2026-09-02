/**
 * @file artifact.h
 * Influence-matrix artifact as consumed on the target (SPEC 8.7, 8.7.1, 8.11 R3, 11.4).
 *
 * The compact binary written by model_prep/truing_model_prep/export.py is parsed
 * into a fixed-capacity structure and the displacement blocks are expanded from
 * their Fourier coefficients at initialisation. Loading performs the three SPEC 8.7
 * checks in order: integrity (content_hash), compatibility (the INDEPENDENT
 * expected fingerprint from the wheel-class configuration), shape (dimensions and
 * layout row masks against the active configuration). Any failure is
 * ARTIFACT_INVALID: nothing is reshaped, padded or truncated.
 */
#ifndef TRUING_ARTIFACT_H
#define TRUING_ARTIFACT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "truing/config.h"
#include "truing/limits.h"
#include "truing/row_layout.h"
#include "truing/status.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TRUING_ARTIFACT_MAGIC          0x41495254u   /* 'TRIA' */
#define TRUING_ARTIFACT_SCHEMA_VERSION 1u
#define TRUING_ARTIFACT_HEADER_BYTES   120u   /* 12 + name 32 + fingerprint 32 + content hash 32 + dims 8 + payload_len 4 */
#define TRUING_MAX_FOURIER_ORDER       16u
#define TRUING_MAX_FOURIER_COEFFS      (2u * TRUING_MAX_FOURIER_ORDER + 1u)
#define TRUING_ARTIFACT_NAME_BYTES     32u

typedef struct {
    truing_layout_id_t layout;
    uint16_t           n_rows;
    uint8_t            effective_rank;
    uint8_t            expected_null_dim;
    float              effective_condition_number;
    truing_row_mask_t  row_mask;
    /* Phi-dagger(L): n_spokes x n_rows, row-major, float32 (SPEC 8.7: per-layout row count). */
    float              pinv[TRUING_MAX_SPOKES][TRUING_MAX_FULL_ROWS];
} truing_artifact_layout_t;

typedef struct {
    bool     loaded;
    uint32_t artifact_id;
    char     name[TRUING_ARTIFACT_NAME_BYTES];
    truing_fingerprint_t generating_fingerprint;
    uint8_t  content_hash[32];
    uint8_t  n_spokes, n_rim_angles, N_lat, N_rad;
    bool     asymmetric;
    /* weights and tolerances baked into every pseudoinverse (SPEC 8.7.1) */
    float    tol_lateral_mm, tol_radial_mm, tol_tension_n, trust_radial, trust_tension;
    float    c_side_a, c_side_b;
    /* common-mode direction (SPEC 8.4 Part 1, 11.4) */
    bool     n_mt_identified;
    float    n_mt[TRUING_MAX_SPOKES];
    bool     T_target_present;
    float    T_target_assumed[TRUING_MAX_SPOKES];    /* normalised to mean 1.0 */
    float    cond_i_residual, cond_i_tolerance, cond_ii_residual, cond_ii_tolerance;
    float    uniqueness_margin, uniqueness_threshold;
    uint8_t  displacement_null_dim;
    /* compact storage */
    float    coeffs_lat[TRUING_MAX_SPOKES][TRUING_MAX_FOURIER_COEFFS];
    float    coeffs_rad[TRUING_MAX_SPOKES][TRUING_MAX_FOURIER_COEFFS];
    float    phi_t[TRUING_MAX_SPOKES][TRUING_MAX_SPOKES];           /* [j][i]: dT_j per revolution of spoke i */
    uint8_t  class_map[TRUING_MAX_SPOKES];                          /* side<<4 | lead_trail */
    /* expanded at load (SPEC 11.4) */
    float    phi_u[TRUING_MAX_RIM_ANGLES][TRUING_MAX_SPOKES];
    float    phi_v[TRUING_MAX_RIM_ANGLES][TRUING_MAX_SPOKES];
    uint8_t  n_layouts;
    truing_artifact_layout_t layouts[2];
} truing_artifact_t;

typedef enum {
    TRUING_ART_OK = 0,
    TRUING_ART_ERR_NULL,
    TRUING_ART_ERR_TRUNCATED,
    TRUING_ART_ERR_MAGIC,
    TRUING_ART_ERR_VERSION,
    TRUING_ART_ERR_INTEGRITY,        /* content_hash mismatch */
    TRUING_ART_ERR_INCOMPATIBLE,     /* fingerprint != configured expectation */
    TRUING_ART_ERR_SHAPE,            /* dimensions / masks / orders / weights disagree with configuration */
    TRUING_ART_ERR_CONDITIONING,     /* recorded effective condition number beyond max_condition_number */
    TRUING_ART_ERR_STRUCTURE,        /* internal inconsistency (rank + nullity, normalisation, symmetric n_mt) */
    TRUING_ART__COUNT
} truing_artifact_result_t;

/* Parse, check (all three SPEC 8.7 checks plus R3 limits) and expand. On any failure `out->loaded`
 * stays false; `detail` names the failing check. */
truing_artifact_result_t truing_artifact_load(const uint8_t *buf, size_t len, const truing_wheel_class_config_t *wheel,
                                              const truing_solver_config_t *solver, truing_artifact_t *out,
                                              const char **detail);

const truing_artifact_layout_t *truing_artifact_layout(const truing_artifact_t *art, truing_layout_id_t layout);
/* SHA-256 hex of the fingerprint (65-byte buffer). */
void truing_fingerprint_hex(const truing_fingerprint_t *fp, char out[65]);
/* Parse a 64-char hex fingerprint into bytes; false on malformed input. */
bool truing_fingerprint_from_hex(const char *hex, truing_fingerprint_t *out);

const char *truing_artifact_result_str(truing_artifact_result_t r);

#ifdef __cplusplus
}
#endif
#endif /* TRUING_ARTIFACT_H */
