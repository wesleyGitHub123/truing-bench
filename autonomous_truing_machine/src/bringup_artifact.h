/* Bring-up section for the influence artifact and the real truing calculation (Phase 1c). */
#ifndef TRUING_BRINGUP_ARTIFACT_H
#define TRUING_BRINGUP_ARTIFACT_H

#include <stdbool.h>

/* Runs the artifact checks and the host/firmware parity cases ON THE TARGET. Adds to the
 * caller's pass/fail tallies; `ok_out` is true when every check in the section passed. */
void truing_bringup_artifact_section(int *pass, int *fail, bool *ok_out);

#endif /* TRUING_BRINGUP_ARTIFACT_H */
