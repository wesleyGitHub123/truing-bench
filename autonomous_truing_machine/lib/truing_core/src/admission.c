#include "truing/admission.h"

#include <stddef.h>
#include <string.h>

static void mask_and(const truing_row_mask_t *a, const truing_row_mask_t *b, truing_row_mask_t *out)
{
    for (unsigned w = 0; w < TRUING_ROW_MASK_WORDS; ++w) {
        out->words[w] = a->words[w] & b->words[w];
    }
}

static void mask_and_not(const truing_row_mask_t *a, const truing_row_mask_t *b, truing_row_mask_t *out)
{
    for (unsigned w = 0; w < TRUING_ROW_MASK_WORDS; ++w) {
        out->words[w] = a->words[w] & ~b->words[w];
    }
}

static void count_row_statuses(const truing_wheel_state_t *ws, const truing_row_dims_t *d, const truing_row_mask_t *active,
                               uint8_t *n_valid, uint8_t *n_suspect)
{
    unsigned valid = 0u, suspect = 0u;
    for (uint8_t k = 0; k < d->n_rim_angles; ++k) {
        const truing_status_t s = ws->runout[k].meta.status;
        const unsigned present = (truing_row_mask_test(active, truing_row_lateral(d, k)) ? 1u : 0u) +
                                 (truing_row_mask_test(active, truing_row_radial(d, k)) ? 1u : 0u);
        if (s == TRUING_STATUS_VALID) {
            valid += present;
        } else if (s == TRUING_STATUS_SUSPECT) {
            suspect += present;
        }
    }
    for (uint8_t i = 0; i < d->n_spokes; ++i) {
        if (!truing_row_mask_test(active, truing_row_tension(d, i))) {
            continue;
        }
        const truing_status_t s = ws->spokes[i].tension.meta.status;
        if (s == TRUING_STATUS_VALID) {
            ++valid;
        } else if (s == TRUING_STATUS_SUSPECT) {
            ++suspect;
        }
    }
    *n_valid = valid > 255u ? 255u : (uint8_t)valid;
    *n_suspect = suspect > 255u ? 255u : (uint8_t)suspect;
}

void truing_admission_evaluate(const truing_wheel_state_t *ws, const truing_row_dims_t *dims,
                               truing_layout_id_t requested_layout, truing_reason_t policy_reason,
                               truing_admission_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->reason = TRUING_REASON_PARTIAL_WHEEL_STATE;
    if (ws == NULL || dims == NULL) {
        return;
    }
    if (!truing_row_mask_available(ws, dims, &out->available_rows)) {
        return;   /* dimension mismatch: nothing is admissible */
    }
    truing_row_channel_summary_t avail;
    truing_row_mask_summarize(&out->available_rows, dims, &avail);

    /* Layout selection: policy first, then the R4 all-absent case. */
    truing_layout_id_t layout = requested_layout;
    if (layout == TRUING_LAYOUT_TENSION_ABSENT) {
        out->tension_excluded_by_policy = !avail.tension_empty;   /* measurements existed but are excluded */
        out->policy_reason = policy_reason;
    } else {
        layout = avail.tension_empty ? TRUING_LAYOUT_TENSION_ABSENT : TRUING_LAYOUT_FULL;
        out->policy_reason = TRUING_REASON_NONE;
    }
    out->layout = layout;

    truing_row_mask_t layout_mask;
    if (!truing_row_mask_for_layout(layout, dims, &layout_mask)) {
        return;
    }
    /* Active row set: what is available AND what the layout uses (a policy exclusion drops
     * the tension rows here; the measurements themselves are untouched). */
    mask_and(&out->available_rows, &layout_mask, &out->active_row_set);
    mask_and_not(&layout_mask, &out->active_row_set, &out->missing_rows);
    out->n_active_rows = truing_row_mask_popcount(&out->active_row_set);
    count_row_statuses(ws, dims, &out->active_row_set, &out->n_valid_rows, &out->n_suspect_rows);

    /* R1: exact match with the shipped layout. */
    if (!truing_row_mask_equals(&out->active_row_set, &layout_mask)) {
        out->admissible = false;
        out->reason = TRUING_REASON_PARTIAL_WHEEL_STATE;
        return;
    }
    /* R2: determinacy. */
    if (out->n_active_rows < dims->n_spokes) {
        out->admissible = false;
        out->reason = TRUING_REASON_PARTIAL_WHEEL_STATE;
        return;
    }
    out->admissible = true;
    out->reason = TRUING_REASON_NONE;
}

bool truing_admission_missing_spoke(const truing_admission_t *a, const truing_row_dims_t *dims, uint8_t spoke_index)
{
    if (a == NULL || dims == NULL || spoke_index >= dims->n_spokes) {
        return false;
    }
    return truing_row_mask_test(&a->missing_rows, truing_row_tension(dims, spoke_index));
}

bool truing_admission_missing_rim_index(const truing_admission_t *a, const truing_row_dims_t *dims, uint8_t rim_index)
{
    if (a == NULL || dims == NULL || rim_index >= dims->n_rim_angles) {
        return false;
    }
    return truing_row_mask_test(&a->missing_rows, truing_row_lateral(dims, rim_index)) ||
           truing_row_mask_test(&a->missing_rows, truing_row_radial(dims, rim_index));
}
