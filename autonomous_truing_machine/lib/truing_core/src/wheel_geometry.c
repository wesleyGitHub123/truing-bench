#include "truing/wheel_geometry.h"

#include <math.h>

#include "truing/limits.h"

float truing_wrap_angle(float angle_rad)
{
    if (!isfinite(angle_rad)) {
        return NAN;
    }
    float a = fmodf(angle_rad, TRUING_TWO_PI);
    if (a < 0.0f) {
        a += TRUING_TWO_PI;
    }
    if (a >= TRUING_TWO_PI) {   /* fmodf rounding can land exactly on 2π */
        a -= TRUING_TWO_PI;
    }
    return a;
}

float truing_shortest_delta(float from_rad, float to_rad)
{
    float d = truing_wrap_angle(to_rad - from_rad);
    if (!isfinite(d)) {
        return NAN;
    }
    if (d > TRUING_PI) {
        d -= TRUING_TWO_PI;
    }
    return d;
}

float truing_circular_distance(float a_rad, float b_rad)
{
    return fabsf(truing_shortest_delta(a_rad, b_rad));
}

float truing_spoke_angle(uint8_t n_spokes, uint8_t spoke_index)
{
    if (n_spokes == 0u || spoke_index >= n_spokes) {
        return NAN;
    }
    return TRUING_TWO_PI * (float)spoke_index / (float)n_spokes;
}

float truing_rim_index_angle(uint8_t n_rim_angles, uint8_t rim_index)
{
    if (n_rim_angles == 0u || rim_index >= n_rim_angles) {
        return NAN;
    }
    return TRUING_TWO_PI * (float)rim_index / (float)n_rim_angles;
}

float truing_machine_angle_of_feature(float feature_angle_rad, float rotation_rad)
{
    return truing_wrap_angle(feature_angle_rad + rotation_rad);
}

float truing_rotation_for_feature_at(float feature_angle_rad, float station_angle_rad)
{
    return truing_wrap_angle(station_angle_rad - feature_angle_rad);
}
