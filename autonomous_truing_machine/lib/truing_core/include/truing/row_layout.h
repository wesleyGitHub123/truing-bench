/**
 * @file row_layout.h
 * Row indexing, row masks and solver layouts (SPEC §8.7, §8.7.1, §8.11).
 *
 * Row convention for the FULL influence matrix Φ = [Φ_u ; Φ_v ; Φ_t]:
 *   rows [0, n_rim_angles)                    lateral, by rim index
 *   rows [n_rim_angles, 2*n_rim_angles)       radial,  by rim index
 *   rows [2*n_rim_angles, n_full_rows)        tension, by spoke index
 *
 * Dimensions are DERIVED from configuration, never hardcoded (SPEC §8.7).
 *
 * `available_rows` (what data exists: valid or suspect) and the `active_row_set`
 * (what problem is solved) are different things (SPEC §8.11). This module supplies
 * the primitives; policy selection of the active set belongs to the solver.
 */
#ifndef TRUING_ROW_LAYOUT_H
#define TRUING_ROW_LAYOUT_H

#include <stdbool.h>
#include <stdint.h>

#include "truing/limits.h"
#include "truing/wheel_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Shipped layouts (SPEC §8.7.1). Any other row pattern is refused with PARTIAL_WHEEL_STATE. */
typedef enum {
    TRUING_LAYOUT_NONE = 0,
    TRUING_LAYOUT_FULL,            /* lateral + radial + tension */
    TRUING_LAYOUT_TENSION_ABSENT,  /* lateral + radial only (degraded mode, SPEC §8.5) */
    TRUING_LAYOUT__COUNT
} truing_layout_id_t;

typedef struct {
    uint32_t words[TRUING_ROW_MASK_WORDS];
} truing_row_mask_t;

typedef struct {
    uint8_t  n_spokes;
    uint8_t  n_rim_angles;
    uint16_t n_full_rows;    /* 2*n_rim_angles + n_spokes */
} truing_row_dims_t;

typedef enum {
    TRUING_ROW_CHANNEL_LATERAL = 0,
    TRUING_ROW_CHANNEL_RADIAL,
    TRUING_ROW_CHANNEL_TENSION,
    TRUING_ROW_CHANNEL_INVALID
} truing_row_channel_t;

typedef struct {
    uint16_t n_lateral;
    uint16_t n_radial;
    uint16_t n_tension;
    bool     lateral_complete;
    bool     radial_complete;
    bool     tension_complete;   /* all tension rows present */
    bool     tension_empty;      /* no tension rows present */
} truing_row_channel_summary_t;

bool truing_row_dims_init(truing_row_dims_t *d, uint8_t n_spokes, uint8_t n_rim_angles);

uint16_t truing_row_lateral(const truing_row_dims_t *d, uint8_t rim_index);
uint16_t truing_row_radial(const truing_row_dims_t *d, uint8_t rim_index);
uint16_t truing_row_tension(const truing_row_dims_t *d, uint8_t spoke_index);
truing_row_channel_t truing_row_channel(const truing_row_dims_t *d, uint16_t row, uint8_t *index_out);

void     truing_row_mask_clear(truing_row_mask_t *m);
void     truing_row_mask_set(truing_row_mask_t *m, uint16_t row);
bool     truing_row_mask_test(const truing_row_mask_t *m, uint16_t row);
uint16_t truing_row_mask_popcount(const truing_row_mask_t *m);
bool     truing_row_mask_equals(const truing_row_mask_t *a, const truing_row_mask_t *b);

/* row_mask(L): the rows a layout's pseudoinverse was computed for (SPEC §8.7.1). */
bool     truing_row_mask_for_layout(truing_layout_id_t layout, const truing_row_dims_t *d, truing_row_mask_t *out);
/* n_layout_rows(L) = popcount(row_mask(L)) (SPEC §8.7). */
uint16_t truing_layout_row_count(truing_layout_id_t layout, const truing_row_dims_t *d);

/* available_rows: rows whose measurement is valid OR suspect (SPEC §8.11). */
bool truing_row_mask_available(const truing_wheel_state_t *ws, const truing_row_dims_t *d, truing_row_mask_t *out);

/* SPEC §8.11 R1: exact match of an active row set against a shipped layout, else NONE. */
truing_layout_id_t truing_layout_match(const truing_row_mask_t *active, const truing_row_dims_t *d);

void truing_row_mask_summarize(const truing_row_mask_t *m, const truing_row_dims_t *d, truing_row_channel_summary_t *out);

const char *truing_layout_str(truing_layout_id_t layout);

#ifdef __cplusplus
}
#endif
#endif /* TRUING_ROW_LAYOUT_H */
