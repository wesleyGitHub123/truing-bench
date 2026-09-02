/**
 * @file records.h
 * Helpers that fill status-bearing records for implementations that cannot
 * produce a value. SPEC P2: placeholders return STATUS, never neutral values —
 * these helpers exist so no implementation is ever tempted to write 0.0.
 */
#ifndef TRUING_HAL_RECORDS_H
#define TRUING_HAL_RECORDS_H

#include "truing/measurements.h"

#ifdef __cplusplus
extern "C" {
#endif

void truing_hal_fill_unavailable_estimate(truing_tension_estimate_t *out, truing_reason_t reason,
                                          uint8_t cycle_index, uint32_t timestamp_ms,
                                          truing_source_impl_t source_impl);

void truing_hal_fill_unavailable_runout(truing_runout_measurement_t *out, truing_reason_t reason,
                                        float rim_angle_rad, uint8_t cycle_index, uint32_t timestamp_ms,
                                        truing_source_impl_t source_impl);

#ifdef __cplusplus
}
#endif
#endif /* TRUING_HAL_RECORDS_H */
