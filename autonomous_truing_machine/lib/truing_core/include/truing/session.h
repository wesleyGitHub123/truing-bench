/**
 * @file session.h
 * Per-session provenance header (SPEC §6.2, §6.6). Written once at session start.
 *
 * Session-constant provenance lives here and is NOT replicated onto every record.
 * Together with per-record fields it answers "why did the system calculate this
 * adjustment" (SPEC P6).
 */
#ifndef TRUING_SESSION_H
#define TRUING_SESSION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "truing/config.h"
#include "truing/status.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TRUING_FIRMWARE_VERSION_MAX 24u

/* Describes one implementation satisfying a hardware-facing interface in this session. */
typedef struct {
    const char          *name;
    truing_source_impl_t source_impl;
} truing_impl_descriptor_t;

typedef struct {
    uint32_t session_id;
    char     firmware_version[TRUING_FIRMWARE_VERSION_MAX];
    uint32_t influence_matrix_artifact_id;         /* 0 until an artifact is loaded */
    truing_fingerprint_t influence_matrix_fingerprint;
    uint32_t tension_model_profile_id;             /* the session-fixed model (SPEC §11.3.1) */
    uint32_t chain_profile_id;
    uint32_t machine_profile_id;                   /* station geometry the session positioned against (SPEC §11.6) */
    float    target_tension_n;                     /* session-fixed value actually used */
    truing_wheel_class_config_t wheel_class_snapshot;
    truing_solver_config_t      solver_snapshot;
    bool     contains_non_real_implementations;    /* SPEC §6.2 data-integrity flag */
    bool     wall_clock_known;                     /* SPEC §6.6: host-supplied offset, recorded once */
    int64_t  wall_clock_unix_ms_at_boot;           /* unix ms corresponding to timestamp_ms == 0 */
} truing_session_header_t;

/* SPEC §6.2: true if ANY interface is satisfied by a recorded, synthetic, or host-service impl. */
bool truing_session_any_non_real(const truing_impl_descriptor_t *impls, size_t n_impls);

bool truing_session_header_init(truing_session_header_t *h, uint32_t session_id, const char *firmware_version,
                                const truing_wheel_class_config_t *wheel, const truing_solver_config_t *solver,
                                uint32_t chain_profile_id, uint32_t machine_profile_id, uint32_t artifact_id,
                                const truing_fingerprint_t *artifact_fingerprint,
                                const truing_impl_descriptor_t *impls, size_t n_impls);

/* Records the host wall clock once: unix_ms_now corresponds to boot-relative boot_ms_now. */
void truing_session_header_set_wall_clock(truing_session_header_t *h, int64_t unix_ms_now, uint32_t boot_ms_now);

/* Resolve a boot-relative timestamp to unix ms; false if the wall clock is unknown. */
bool truing_session_resolve_timestamp(const truing_session_header_t *h, uint32_t timestamp_ms, int64_t *unix_ms_out);

#ifdef __cplusplus
}
#endif
#endif /* TRUING_SESSION_H */
