/* Influence-artifact store: the single expanded artifact held in RAM (SPEC 11.4).
 *
 * Phase 1c loads the GOLDEN FIXTURE artifact compiled into flash (the sym32 fixture
 * wheel's model, SYNTHETIC). It is the only artifact this build knows; a real wheel's
 * artifact would arrive over the SPEC 12 protocol into the artifacts partition and load
 * through the same checks. Nothing here is a default: the caller passes the active
 * wheel-class and solver configuration and the three SPEC 8.7 checks decide. */
#ifndef TRUING_ARTIFACT_STORE_H
#define TRUING_ARTIFACT_STORE_H

#include <stdbool.h>
#include <stdint.h>

#include "truing/artifact.h"
#include "truing/config.h"

typedef struct {
    truing_artifact_result_t result;
    const char              *detail;
    uint32_t                 load_us;        /* parse + checks + Fourier expansion on this target */
    uint32_t                 blob_bytes;     /* compact form in flash */
    uint32_t                 expanded_bytes; /* sizeof(truing_artifact_t) held in RAM */
} truing_artifact_store_status_t;

/* Loads the compiled-in golden fixture artifact against the given configuration. NULL on any
 * failed check (the store then holds nothing). */
const truing_artifact_t *truing_artifact_store_load_fixture(const truing_wheel_class_config_t *wheel,
                                                            const truing_solver_config_t *solver,
                                                            truing_artifact_store_status_t *status);
/* The loaded artifact, or NULL. */
const truing_artifact_t *truing_artifact_store_get(void);

#endif /* TRUING_ARTIFACT_STORE_H */
