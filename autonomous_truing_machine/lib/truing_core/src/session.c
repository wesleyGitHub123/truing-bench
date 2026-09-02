#include "truing/session.h"

#include <string.h>

bool truing_session_any_non_real(const truing_impl_descriptor_t *impls, size_t n_impls)
{
    if (impls == NULL) {
        return n_impls != 0u;   /* unknown implementations cannot be assumed real */
    }
    for (size_t i = 0; i < n_impls; ++i) {
        if (impls[i].source_impl != TRUING_SOURCE_REAL) {
            return true;
        }
    }
    return false;
}

bool truing_session_header_init(truing_session_header_t *h, uint32_t session_id, const char *firmware_version,
                                const truing_wheel_class_config_t *wheel, const truing_solver_config_t *solver,
                                uint32_t chain_profile_id, uint32_t machine_profile_id, uint32_t artifact_id,
                                const truing_fingerprint_t *artifact_fingerprint,
                                const truing_impl_descriptor_t *impls, size_t n_impls)
{
    if (h == NULL || firmware_version == NULL || wheel == NULL || solver == NULL) {
        return false;
    }
    memset(h, 0, sizeof(*h));
    h->session_id = session_id;
    strncpy(h->firmware_version, firmware_version, TRUING_FIRMWARE_VERSION_MAX - 1u);
    h->firmware_version[TRUING_FIRMWARE_VERSION_MAX - 1u] = '\0';
    h->influence_matrix_artifact_id = artifact_id;
    if (artifact_fingerprint != NULL) {
        h->influence_matrix_fingerprint = *artifact_fingerprint;
    }
    h->tension_model_profile_id = solver->tension_model_profile_id;
    h->chain_profile_id = chain_profile_id;
    h->machine_profile_id = machine_profile_id;
    h->target_tension_n = solver->target_tension_n;
    h->wheel_class_snapshot = *wheel;
    h->solver_snapshot = *solver;
    h->contains_non_real_implementations = truing_session_any_non_real(impls, n_impls);
    h->wall_clock_known = false;
    h->wall_clock_unix_ms_at_boot = 0;
    return true;
}

void truing_session_header_set_wall_clock(truing_session_header_t *h, int64_t unix_ms_now, uint32_t boot_ms_now)
{
    if (h == NULL) {
        return;
    }
    h->wall_clock_unix_ms_at_boot = unix_ms_now - (int64_t)boot_ms_now;
    h->wall_clock_known = true;
}

bool truing_session_resolve_timestamp(const truing_session_header_t *h, uint32_t timestamp_ms, int64_t *unix_ms_out)
{
    if (h == NULL || unix_ms_out == NULL || !h->wall_clock_known) {
        return false;
    }
    *unix_ms_out = h->wall_clock_unix_ms_at_boot + (int64_t)timestamp_ms;
    return true;
}
