/**
 * @file model_ids.h
 * Tension-model candidate identifiers (SPEC §9.2 layer 4).
 *
 * Supported candidates exist; a project-wide validated default does NOT. The
 * session-selected model comes from the tension-model profile chosen in
 * configuration (SPEC §11.3.1) and is recorded on every estimate (SPEC §6.3).
 * UNSET is a configuration error, never a fallback.
 */
#ifndef TRUING_MODEL_IDS_H
#define TRUING_MODEL_IDS_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TRUING_TENSION_MODEL_UNSET = 0,
    TRUING_TENSION_MODEL_IDEAL_STRING,          /* research repo M0 */
    TRUING_TENSION_MODEL_STIFFNESS_CORRECTED,   /* research repo M1 */
    TRUING_TENSION_MODEL_HIGHER_MODE,           /* research repo M2 (two-mode) */
    TRUING_TENSION_MODEL_EMPIRICAL,             /* research repo M_emp */
    TRUING_TENSION_MODEL__COUNT
} truing_tension_model_t;

const char *truing_tension_model_str(truing_tension_model_t m);

#ifdef __cplusplus
}
#endif
#endif /* TRUING_MODEL_IDS_H */
