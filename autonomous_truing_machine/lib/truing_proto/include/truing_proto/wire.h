/**
 * @file wire.h
 * The SPEC §12 wire protocol: JSON frames in both directions, transport-independent.
 *
 * SPEC §12.1 makes the transport interchangeable on purpose — "the UI is a web
 * application talking to a JSON/websocket endpoint either way. Only the transport
 * changes; the frontend does not." This header is that endpoint's vocabulary, with
 * no socket, no WiFi and no framework anywhere in it, so the same frames are produced
 * for WiFi, for the USB CDC fallback, and for a host test with no hardware at all.
 *
 * Four outbound frame types:
 *   "event"       telemetry, one-way and best-effort (SPEC §12.2)
 *   "state"       the REQUIRED current-state query (SPEC §12.2)
 *   "provenance"  GET_CURRENT_CYCLE_PROVENANCE / P6 (SPEC §12.2)
 *   "ack"         the verdict on one inbound command
 *
 * Inbound: one flat JSON object per command frame.
 *
 * ---- What this layer decides, and what it does not -------------------------------
 *
 * The decoder enforces the WIRE contract only: the frame parses, names a known
 * command, carries the fields that command requires, and obeys the SPEC §12.3
 * `wait_id` rule. It reports nothing about whether the intent is admissible — that
 * is state, and it belongs to truing_orch_submit_intent(). Keeping the split sharp
 * means a decoded command can be handed to the orchestrator unexamined, and the
 * admissibility rules have exactly one implementation.
 *
 * ---- Vocabulary ------------------------------------------------------------------
 *
 * Every enumerated value on the wire is the firmware's OWN string for it, taken from
 * truing_*_str() rather than respelled here, and the reverse lookups the decoder uses
 * are derived from those same tables. Adding an intent or a parameter therefore
 * extends the protocol automatically and renaming one renames it on the wire, so the
 * two can never disagree. The cost is that the existing tables are not uniformly
 * cased — statuses and stations are lowercase ("suspect", "acoustic") while reason
 * codes, states and intents are uppercase — and that inconsistency is deliberately
 * preserved: a client then reads exactly the tokens that appear in the firmware's
 * logs and bring-up reports, which is worth more than a tidier-looking wire format.
 *
 * ---- The `wait_id` rule (SPEC §12.3) ---------------------------------------------
 *
 * "The firmware rejects and logs commands whose wait_id does not match the active
 * wait... a client that omits or fabricates wait_id must be rejected, not
 * accommodated," and "ABORT and START_TRUING carry no wait_id."
 *
 * That splits cleanly across the two layers:
 *
 *   omitted on a confirmation -> rejected HERE, as MISSING_WAIT_ID. There is no
 *       value to correlate, so the frame is malformed rather than stale.
 *   present but wrong         -> decoded here, rejected by the correlator as
 *       STALE_INTENT. Only the orchestrator knows which wait is active.
 *   present on ABORT or START_TRUING -> rejected HERE, as UNEXPECTED_WAIT_ID. A
 *       client that thinks ABORT answers a wait has misunderstood the protocol, and
 *       accepting the field would be accommodating exactly what §12.3 forbids.
 */
#ifndef TRUING_PROTO_WIRE_H
#define TRUING_PROTO_WIRE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "truing/operator_intent.h"
#include "truing/status.h"
#include "truing_hal/telemetry_if.h"
#include "truing_orch/orchestrator.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Frame budgets. The provenance frame is the only large one: it carries a status and
 * a reason code for every spoke and every rim index, and it is answered on demand
 * rather than streamed, so its size costs nothing in the telemetry path. */
#define TRUING_WIRE_EVENT_BUF       512u
#define TRUING_WIRE_STATE_BUF       512u
#define TRUING_WIRE_ACK_BUF         256u
#define TRUING_WIRE_PROVENANCE_BUF  8192u
/* An inbound frame longer than this is rejected unread (SPEC §12.3: reject, do not
 * accommodate). Every defined command is well under 200 bytes. */
#define TRUING_WIRE_COMMAND_MAX     512u

/* ---- Inbound ---------------------------------------------------------------------- */

typedef enum {
    TRUING_WIRE_CMD_UNSET = 0,
    TRUING_WIRE_CMD_INTENT,                        /* `intent` is filled in */
    TRUING_WIRE_CMD_GET_CURRENT_STATE,             /* SPEC §12.2 required query */
    TRUING_WIRE_CMD_GET_CURRENT_CYCLE_PROVENANCE,  /* SPEC §12.2 separate query */
} truing_wire_cmd_kind_t;

typedef enum {
    TRUING_WIRE_OK = 0,
    TRUING_WIRE_ERR_TOO_LONG,
    TRUING_WIRE_ERR_MALFORMED,          /* not JSON, or the root is not an object */
    TRUING_WIRE_ERR_UNKNOWN_COMMAND,
    TRUING_WIRE_ERR_MISSING_WAIT_ID,    /* a confirmation with no wait_id (SPEC §12.3) */
    TRUING_WIRE_ERR_UNEXPECTED_WAIT_ID, /* wait_id on ABORT / START_TRUING (SPEC §12.3) */
    TRUING_WIRE_ERR_UNKNOWN_PARAMETER,
    TRUING_WIRE_ERR_MISSING_FIELD,
    TRUING_WIRE_ERR_FIELD_TYPE,
    TRUING_WIRE_ERR_FIELD_RANGE,        /* right type, outside what the field can hold */
} truing_wire_error_t;

typedef struct {
    truing_wire_cmd_kind_t kind;
    truing_intent_t        intent;   /* valid when kind == INTENT */
    bool                   has_seq;  /* client-supplied correlation number, echoed in the ack */
    uint32_t               seq;
} truing_wire_command_t;

/* Parses one command frame. `len` may be 0 to measure a NUL-terminated string.
 * `out` is always cleared first, so a rejected frame leaves nothing partially filled. */
truing_wire_error_t truing_wire_decode_command(const char *json, size_t len, truing_wire_command_t *out);

/* ---- Outbound --------------------------------------------------------------------- */

/* Each returns the number of bytes written (excluding the NUL), or 0 if the frame did
 * not fit — in which case nothing valid was produced and the caller must send nothing.
 * For telemetry, dropping the frame is the correct outcome (SPEC §12.2 best-effort). */
size_t truing_wire_encode_event(const truing_telemetry_event_t *ev, char *buf, size_t cap);
size_t truing_wire_encode_state(const truing_orch_snapshot_t *snap, char *buf, size_t cap);
size_t truing_wire_encode_provenance(const truing_cycle_provenance_t *prov, char *buf, size_t cap);

/* The verdict on one command. `verdict` is meaningful only when the frame decoded
 * (err == TRUING_WIRE_OK) and the orchestrator ruled on it; pass
 * TRUING_INTENT_ADMIT_ACCEPT with a non-OK `err` for a frame rejected at this layer. */
size_t truing_wire_encode_ack(const truing_wire_command_t *cmd, truing_wire_error_t err,
                              truing_intent_verdict_t verdict, truing_reason_t reason,
                              char *buf, size_t cap);

const char *truing_wire_error_str(truing_wire_error_t e);
const char *truing_wire_cmd_kind_str(truing_wire_cmd_kind_t k);

#ifdef __cplusplus
}
#endif
#endif /* TRUING_PROTO_WIRE_H */
