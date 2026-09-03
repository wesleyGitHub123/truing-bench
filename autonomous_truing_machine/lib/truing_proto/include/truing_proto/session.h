/**
 * @file session.h
 * The server half of the SPEC §12 endpoint: telemetry out, commands in.
 *
 * Two operations, and both run ON THE ORCHESTRATOR'S TASK:
 *
 *   pump()    drains the telemetry ring, encodes each event, hands it to the sink
 *   handle()  decodes one inbound frame, answers the two queries or submits the
 *             intent, and sends an acknowledgement
 *
 * ---- Why the task matters --------------------------------------------------------
 *
 * The orchestrator is not thread-safe and its snapshot and provenance readers take a
 * mutable pointer, so reading them from a network task while the state machine steps
 * would be a race. Rather than bolt a lock onto a framework-free library, the whole
 * session runs where the orchestrator already lives; the transport's job is only to
 * move bytes to and from these calls. That is also what SPEC §12.4 means by operator
 * intent being an event and the transport a driver.
 *
 * Because handle() runs on the orchestrator's own task, it can submit an intent and
 * report the real §12.3 verdict in the same acknowledgement, rather than deferring it.
 *
 * ---- Why the sink must never block ----------------------------------------------
 *
 * SPEC §12.2 requires telemetry never to block the control path, and this session runs
 * ON that path. A sink implementation therefore copies the frame into its own buffer
 * or drops it, and returns; it must not wait on a socket. Dropping is correct and is
 * counted in `frames_dropped` — telemetry is best-effort with no replay, and the
 * current-state query exists precisely so a client that missed events can resynchronise
 * without one.
 */
#ifndef TRUING_PROTO_SESSION_H
#define TRUING_PROTO_SESSION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "truing_hal/telemetry_if.h"
#include "truing_orch/orchestrator.h"
#include "truing_proto/wire.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct truing_wire_sink truing_wire_sink_t;

struct truing_wire_sink {
    const char *impl_name;
    /* One complete frame, NUL-terminated, `len` bytes. MUST NOT BLOCK. Return false to
     * report the frame was dropped; the session counts it and carries on. */
    bool (*send)(truing_wire_sink_t *self, const char *frame, size_t len);
    void *ctx;
};

typedef struct {
    truing_orchestrator_t       *orch;
    truing_telemetry_ring_ctx_t *ring;    /* drain side of the orchestrator's telemetry sink */
    truing_wire_sink_t          *sink;    /* optional; a session with no sink simply drops */
    bool                         debug_channel_enabled;   /* SPEC §12.5: development only */

    uint32_t events_sent;
    uint32_t events_dropped;
    uint32_t commands_accepted;
    uint32_t commands_rejected;

    /* One scratch buffer, sized for the largest frame (provenance). Held in the session
     * rather than on the stack because this runs on the orchestrator's task. */
    char scratch[TRUING_WIRE_PROVENANCE_BUF];
} truing_wire_session_t;

bool truing_wire_session_init(truing_wire_session_t *s, truing_orchestrator_t *orch,
                              truing_telemetry_ring_ctx_t *ring, truing_wire_sink_t *sink,
                              bool debug_channel_enabled);

/* Encodes and sends up to `max_events` queued telemetry events (0 = drain the ring).
 * Returns the number actually sent. Bounded so one call cannot monopolise the task. */
uint32_t truing_wire_session_pump(truing_wire_session_t *s, uint32_t max_events);

/* Handles one inbound frame and sends exactly one acknowledgement.
 * A query also sends its answer frame, before the acknowledgement.
 * The return value is the WIRE result; the admissibility verdict rides in the ack. */
truing_wire_error_t truing_wire_session_handle(truing_wire_session_t *s, const char *json, size_t len);

/* Sends an unsolicited current-state snapshot. The frame a client asks for on connect
 * or reconnect (SPEC §12.2), also useful to push when a wait is issued. */
bool truing_wire_session_send_state(truing_wire_session_t *s);

#ifdef __cplusplus
}
#endif
#endif /* TRUING_PROTO_SESSION_H */
