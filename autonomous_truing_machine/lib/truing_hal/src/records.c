#include "truing_hal/records.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

void truing_hal_fill_unavailable_estimate(truing_tension_estimate_t *out, truing_reason_t reason,
                                          uint8_t cycle_index, uint32_t timestamp_ms,
                                          truing_source_impl_t source_impl)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->meta.status = TRUING_STATUS_UNAVAILABLE;
    out->meta.reason_code = reason != TRUING_REASON_NONE ? reason : TRUING_REASON_NOT_IMPLEMENTED;
    out->meta.cycle_index = cycle_index;
    out->meta.timestamp_ms = timestamp_ms;
    out->meta.source_impl = source_impl;
    out->tension_n = NAN;   /* never 0.0 (P2) */
    out->sigma_n = NAN;
    out->frequency.selected_frequency_hz = NAN;
    out->frequency.snr_db = NAN;
}

void truing_hal_fill_unavailable_runout(truing_runout_measurement_t *out, truing_reason_t reason,
                                        float rim_angle_rad, uint8_t cycle_index, uint32_t timestamp_ms,
                                        truing_source_impl_t source_impl)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->meta.status = TRUING_STATUS_UNAVAILABLE;
    out->meta.reason_code = reason != TRUING_REASON_NONE ? reason : TRUING_REASON_NOT_IMPLEMENTED;
    out->meta.cycle_index = cycle_index;
    out->meta.timestamp_ms = timestamp_ms;
    out->meta.source_impl = source_impl;
    out->rim_angle_rad = rim_angle_rad;
    out->lateral_mm = NAN;
    out->radial_mm = NAN;
}
