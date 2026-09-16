/**
 * @file debug_code.h
 * Debug-channel command codes (SPEC §12.5): what TRUING_INTENT_DEBUG's `payload.debug.code`
 * selects. Development only, never in the demonstration script -- admission gates the intent
 * on debug_channel_enabled alone (operator_intent.c), never on session or state; this registry
 * is consulted only once an intent has already been admitted.
 *
 * A registry, not a dispatcher: adding a code here does not make it do anything by itself.
 * The orchestrator's intent dispatch (truing_orch_submit_intent(), orchestrator.c) is the only
 * place a code is interpreted, and it is the only place that may know what a code does.
 */
#ifndef TRUING_DEBUG_CODE_H
#define TRUING_DEBUG_CODE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TRUING_DEBUG_CODE_UNSET = 0,
    /* Fire exactly one acoustic measurement on demand, with no orchestrator session, for
     * bench-driven campaign scripts. `arg` is the spoke_id. The estimate is discarded by the
     * dispatcher -- MEASURE_ONCE never becomes wheel state and never reaches the solver or
     * admission path (rule 4, docs/DEV_NOTES.md); its result is read back only through the
     * existing /debug/capture.json /.pcm evidence seam (SPEC §12.5), never a return value here. */
    TRUING_DEBUG_CODE_MEASURE_ONCE = 1,
    TRUING_DEBUG_CODE__COUNT
} truing_debug_code_t;

const char *truing_debug_code_str(truing_debug_code_t c);

#ifdef __cplusplus
}
#endif
#endif /* TRUING_DEBUG_CODE_H */
