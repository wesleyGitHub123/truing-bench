#include "truing/row_layout.h"

#include <stddef.h>
#include <string.h>

static const char *const k_layout_str[TRUING_LAYOUT__COUNT] = { "NONE", "FULL", "TENSION_ABSENT" };

const char *truing_layout_str(truing_layout_id_t layout)
{
    return (unsigned)layout < TRUING_LAYOUT__COUNT ? k_layout_str[layout] : "?";
}

bool truing_row_dims_init(truing_row_dims_t *d, uint8_t n_spokes, uint8_t n_rim_angles)
{
    if (d == NULL) {
        return false;
    }
    if (!truing_wheel_dims_supported(n_spokes, n_rim_angles)) {
        memset(d, 0, sizeof(*d));
        return false;
    }
    d->n_spokes = n_spokes;
    d->n_rim_angles = n_rim_angles;
    d->n_full_rows = (uint16_t)(2u * n_rim_angles + n_spokes);
    return d->n_full_rows <= TRUING_MAX_FULL_ROWS;
}

uint16_t truing_row_lateral(const truing_row_dims_t *d, uint8_t rim_index)
{
    (void)d;
    return rim_index;
}

uint16_t truing_row_radial(const truing_row_dims_t *d, uint8_t rim_index)
{
    return (uint16_t)(d->n_rim_angles + rim_index);
}

uint16_t truing_row_tension(const truing_row_dims_t *d, uint8_t spoke_index)
{
    return (uint16_t)(2u * d->n_rim_angles + spoke_index);
}

truing_row_channel_t truing_row_channel(const truing_row_dims_t *d, uint16_t row, uint8_t *index_out)
{
    if (d == NULL || row >= d->n_full_rows) {
        return TRUING_ROW_CHANNEL_INVALID;
    }
    truing_row_channel_t ch;
    uint16_t idx;
    if (row < d->n_rim_angles) {
        ch = TRUING_ROW_CHANNEL_LATERAL;
        idx = row;
    } else if (row < (uint16_t)(2u * d->n_rim_angles)) {
        ch = TRUING_ROW_CHANNEL_RADIAL;
        idx = (uint16_t)(row - d->n_rim_angles);
    } else {
        ch = TRUING_ROW_CHANNEL_TENSION;
        idx = (uint16_t)(row - 2u * d->n_rim_angles);
    }
    if (index_out != NULL) {
        *index_out = (uint8_t)idx;
    }
    return ch;
}

void truing_row_mask_clear(truing_row_mask_t *m)
{
    if (m != NULL) {
        memset(m, 0, sizeof(*m));
    }
}

void truing_row_mask_set(truing_row_mask_t *m, uint16_t row)
{
    if (m == NULL || row >= TRUING_ROW_MASK_BITS) {
        return;
    }
    m->words[row / 32u] |= (uint32_t)1u << (row % 32u);
}

bool truing_row_mask_test(const truing_row_mask_t *m, uint16_t row)
{
    if (m == NULL || row >= TRUING_ROW_MASK_BITS) {
        return false;
    }
    return (m->words[row / 32u] >> (row % 32u)) & 1u;
}

uint16_t truing_row_mask_popcount(const truing_row_mask_t *m)
{
    if (m == NULL) {
        return 0;
    }
    uint16_t n = 0;
    for (unsigned w = 0; w < TRUING_ROW_MASK_WORDS; ++w) {
        uint32_t v = m->words[w];
        while (v != 0u) {
            v &= v - 1u;
            ++n;
        }
    }
    return n;
}

bool truing_row_mask_equals(const truing_row_mask_t *a, const truing_row_mask_t *b)
{
    if (a == NULL || b == NULL) {
        return false;
    }
    return memcmp(a->words, b->words, sizeof(a->words)) == 0;
}

bool truing_row_mask_for_layout(truing_layout_id_t layout, const truing_row_dims_t *d, truing_row_mask_t *out)
{
    if (d == NULL || out == NULL) {
        return false;
    }
    truing_row_mask_clear(out);
    if (layout != TRUING_LAYOUT_FULL && layout != TRUING_LAYOUT_TENSION_ABSENT) {
        return false;
    }
    for (uint8_t k = 0; k < d->n_rim_angles; ++k) {
        truing_row_mask_set(out, truing_row_lateral(d, k));
        truing_row_mask_set(out, truing_row_radial(d, k));
    }
    if (layout == TRUING_LAYOUT_FULL) {
        for (uint8_t i = 0; i < d->n_spokes; ++i) {
            truing_row_mask_set(out, truing_row_tension(d, i));
        }
    }
    return true;
}

uint16_t truing_layout_row_count(truing_layout_id_t layout, const truing_row_dims_t *d)
{
    truing_row_mask_t m;
    if (!truing_row_mask_for_layout(layout, d, &m)) {
        return 0;
    }
    return truing_row_mask_popcount(&m);
}

bool truing_row_mask_available(const truing_wheel_state_t *ws, const truing_row_dims_t *d, truing_row_mask_t *out)
{
    if (out == NULL) {
        return false;
    }
    truing_row_mask_clear(out);
    if (ws == NULL || d == NULL) {
        return false;
    }
    if (ws->n_spokes != d->n_spokes || ws->n_rim_angles != d->n_rim_angles) {
        return false;
    }
    for (uint8_t k = 0; k < d->n_rim_angles; ++k) {
        if (truing_status_is_solver_admissible(ws->runout[k].meta.status)) {
            truing_row_mask_set(out, truing_row_lateral(d, k));
            truing_row_mask_set(out, truing_row_radial(d, k));
        }
    }
    for (uint8_t i = 0; i < d->n_spokes; ++i) {
        if (truing_status_is_solver_admissible(ws->spokes[i].tension.meta.status)) {
            truing_row_mask_set(out, truing_row_tension(d, i));
        }
    }
    return true;
}

truing_layout_id_t truing_layout_match(const truing_row_mask_t *active, const truing_row_dims_t *d)
{
    if (active == NULL || d == NULL) {
        return TRUING_LAYOUT_NONE;
    }
    truing_row_mask_t m;
    static const truing_layout_id_t k_shipped[] = { TRUING_LAYOUT_FULL, TRUING_LAYOUT_TENSION_ABSENT };
    for (unsigned i = 0; i < sizeof(k_shipped) / sizeof(k_shipped[0]); ++i) {
        if (truing_row_mask_for_layout(k_shipped[i], d, &m) && truing_row_mask_equals(active, &m)) {
            return k_shipped[i];
        }
    }
    return TRUING_LAYOUT_NONE;
}

void truing_row_mask_summarize(const truing_row_mask_t *m, const truing_row_dims_t *d, truing_row_channel_summary_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (m == NULL || d == NULL) {
        return;
    }
    for (uint8_t k = 0; k < d->n_rim_angles; ++k) {
        if (truing_row_mask_test(m, truing_row_lateral(d, k))) {
            out->n_lateral++;
        }
        if (truing_row_mask_test(m, truing_row_radial(d, k))) {
            out->n_radial++;
        }
    }
    for (uint8_t i = 0; i < d->n_spokes; ++i) {
        if (truing_row_mask_test(m, truing_row_tension(d, i))) {
            out->n_tension++;
        }
    }
    out->lateral_complete = out->n_lateral == d->n_rim_angles;
    out->radial_complete = out->n_radial == d->n_rim_angles;
    out->tension_complete = out->n_tension == d->n_spokes;
    out->tension_empty = out->n_tension == 0u;
}
