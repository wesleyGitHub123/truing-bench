/**
 * @file wheel_geometry.h
 * The ONE place that relates the three angular coordinate domains of the machine
 * (SPEC §10A). Nothing else may re-derive these relationships.
 *
 *   WHEEL coordinates    rim angle θ of a wheel feature: radians, CCW viewed from Side A,
 *                        origin at spoke 0 / valve-stem reference (SPEC §6.4, §6.5).
 *   MACHINE coordinates  fixed to the frame; each physical STATION sits at a machine angle
 *                        φ_S recorded in the machine profile (SPEC §11.6).
 *   ACTUATOR coordinates steps / encoder counts of whatever drives the wheel; owned by
 *                        the navigation implementation and never exposed upward.
 *
 * The wheel's rotation state R links the first two:  machine_angle = wrap(θ + R).
 * Bringing feature θ to station φ_S therefore requires  R' = wrap(φ_S − θ).
 */
#ifndef TRUING_WHEEL_GEOMETRY_H
#define TRUING_WHEEL_GEOMETRY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Wrap any angle into [0, 2π). NaN stays NaN. */
float truing_wrap_angle(float angle_rad);
/* Signed shortest arc from `from` to `to`, in (−π, π]. */
float truing_shortest_delta(float from_rad, float to_rad);
/* Unsigned circular distance in [0, π]. */
float truing_circular_distance(float a_rad, float b_rad);

/* Wheel angle of spoke i: 2π·i / n_spokes (spoke 0 is the origin, SPEC §6.5). NaN if out of range. */
float truing_spoke_angle(uint8_t n_spokes, uint8_t spoke_index);
/* Wheel angle of rim index k: 2π·k / n_rim_angles. NaN if out of range. */
float truing_rim_index_angle(uint8_t n_rim_angles, uint8_t rim_index);

/* Machine angle at which wheel feature θ currently sits, given rotation state R. */
float truing_machine_angle_of_feature(float feature_angle_rad, float rotation_rad);
/* Rotation state R' that places wheel feature θ at station angle φ. */
float truing_rotation_for_feature_at(float feature_angle_rad, float station_angle_rad);

#ifdef __cplusplus
}
#endif
#endif /* TRUING_WHEEL_GEOMETRY_H */
