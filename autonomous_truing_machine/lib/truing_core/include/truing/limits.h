/**
 * @file limits.h
 * Capacity bounds and specification-fixed constants for the framework-free core.
 *
 * Only quantities determined by physics or by the specification itself live
 * here (SPEC P5). Everything wheel-, sensor-, or tolerance-dependent is
 * configuration (truing/config.h) and MUST NOT appear as a literal elsewhere.
 */
#ifndef TRUING_LIMITS_H
#define TRUING_LIMITS_H

/* Wheel-class scope supported by this project (SPEC §11.1: n_spokes is 32 or 36). */
#define TRUING_MAX_SPOKES        36u
/* SPEC §8.9 Stage 1 policy: n_rim_angles = n_spokes, so the same bound applies. */
#define TRUING_MAX_RIM_ANGLES    36u
/* SPEC §8.7: n_full_rows = 2*n_rim_angles + n_spokes (lateral + radial + tension). */
#define TRUING_MAX_FULL_ROWS     (2u * TRUING_MAX_RIM_ANGLES + TRUING_MAX_SPOKES)
/* Row masks are fixed-width bitsets covering TRUING_MAX_FULL_ROWS (108 -> 4 words). */
#define TRUING_ROW_MASK_WORDS    4u
#define TRUING_ROW_MASK_BITS     (32u * TRUING_ROW_MASK_WORDS)

/* Retained in-band peak candidates per FrequencyMeasurement (SPEC §6.3).
 * Capacity bound matching the research pipeline's reported peak count. */
#define TRUING_MAX_CANDIDATE_PEAKS 8u

/* SPEC §9.3 fixed system audio format. Sources adapt to it; it never adapts. */
#define TRUING_AUDIO_SAMPLE_RATE_HZ 48000u
#define TRUING_AUDIO_BIT_DEPTH      24u
#define TRUING_AUDIO_SLOT_BITS      32u

#define TRUING_PI      3.14159265358979323846f
#define TRUING_TWO_PI  6.28318530717958647692f

#endif /* TRUING_LIMITS_H */
