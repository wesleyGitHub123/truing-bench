/**
 * @file fixtures.h
 * SYNTHETIC configuration fixtures for tests and the on-device self-test.
 *
 * NOT DEFAULTS. NOT A REAL WHEEL. Every value here is test data. The firmware
 * never loads these unless a build is explicitly compiled to provision them for
 * a persistence test, and then it logs loudly that fixture data is in use.
 *
 * Values quoted from the reference work (e.g. c = 473 N/rev, N_lat = 6,
 * N_rad = 13, tolerances) appear here ONLY as fixture content; SPEC P5 forbids
 * treating them as project defaults.
 */
#ifndef TRUING_FIXTURES_H
#define TRUING_FIXTURES_H

#include <stddef.h>
#include <stdint.h>

#include "truing/config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Symmetric 32-spoke, 3-cross fixture wheel (Side A declared arbitrarily). */
/* GENERATED golden influence artifact for the sym32 fixture wheel (model_prep export-c-fixtures):
 * the compact binary blob, and its fingerprint / content hash as independent hex strings. */
extern const uint8_t fixture_sym32_artifact_blob[];
extern const size_t  fixture_sym32_artifact_blob_len;
extern const char    fixture_sym32_artifact_fingerprint_hex[65];
extern const char    fixture_sym32_artifact_content_hash_hex[65];

void truing_fixture_wheel_class_sym32(truing_wheel_class_config_t *out);
/* Asymmetric 36-spoke fixture wheel (rotor side flange inboard). */
void truing_fixture_wheel_class_asym36(truing_wheel_class_config_t *out);

void truing_fixture_solver_config(truing_solver_config_t *out, uint8_t n_rim_angles);
void truing_fixture_chain_profile_inmp441(truing_chain_profile_t *out);
/* Both acoustic stations' actuators at the pulse the chain profile carried until the split. */
void truing_fixture_excitation_profile(truing_excitation_profile_t *out);

/* Ideal-string profile with every required parameter established. */
void truing_fixture_tension_model_profile_complete(truing_tension_model_profile_t *out);
/* Same, but L_eff unestablished -> CALIBRATION_MISSING (SPEC §11.3.1). */
void truing_fixture_tension_model_profile_incomplete(truing_tension_model_profile_t *out);
/* Complete, but declares a different spoke gauge -> incompatible with sym32. */
void truing_fixture_tension_model_profile_incompatible(truing_tension_model_profile_t *out);

/* Fixture machine: acoustic station is the reference at 0 rad, runout at +90°,
 * adjustment at +180°, no separate reference sensor. Angles are fixture data only. */
void truing_fixture_machine_profile(truing_machine_profile_t *out);

#ifdef __cplusplus
}
#endif
#endif /* TRUING_FIXTURES_H */
