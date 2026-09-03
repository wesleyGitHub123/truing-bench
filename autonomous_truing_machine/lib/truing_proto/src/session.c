#include "truing_proto/session.h"

#include <string.h>

static bool send_frame(truing_wire_session_t *s, size_t len)
{
    if (len == 0u) {
        return false;   /* the frame did not fit; nothing valid was produced */
    }
    if (s->sink == NULL || s->sink->send == NULL) {
        return false;
    }
    return s->sink->send(s->sink, s->scratch, len);
}

bool truing_wire_session_init(truing_wire_session_t *s, truing_orchestrator_t *orch,
                              truing_telemetry_ring_ctx_t *ring, truing_wire_sink_t *sink,
                              bool debug_channel_enabled)
{
    if (s == NULL || orch == NULL) {
        return false;
    }
    memset(s, 0, sizeof(*s));
    s->orch = orch;
    s->ring = ring;
    s->sink = sink;
    s->debug_channel_enabled = debug_channel_enabled;
    return true;
}

bool truing_wire_session_send_event(truing_wire_session_t *s, const truing_telemetry_event_t *ev)
{
    if (s == NULL || ev == NULL) {
        return false;
    }
    if (send_frame(s, truing_wire_encode_event(ev, s->scratch, sizeof(s->scratch)))) {
        s->events_sent++;
        return true;
    }
    s->events_dropped++;
    return false;
}

uint32_t truing_wire_session_pump(truing_wire_session_t *s, uint32_t max_events)
{
    if (s == NULL || s->ring == NULL) {
        return 0u;
    }
    uint32_t sent = 0u;
    uint32_t processed = 0u;
    truing_telemetry_event_t ev;
    /* Bounded by events PROCESSED, not by events sent, so a run of frames the sink
     * refuses cannot keep this loop spinning on the control path. */
    while ((max_events == 0u || processed < max_events) && truing_telemetry_ring_pop(s->ring, &ev)) {
        processed++;
        /* A frame that will not encode, or that the sink refuses, is a dropped frame —
         * which SPEC §12.2 permits: telemetry is best-effort and the event has already
         * left the ring, so it is gone rather than retried. */
        if (truing_wire_session_send_event(s, &ev)) {
            sent++;
        }
    }
    return sent;
}

bool truing_wire_session_send_state(truing_wire_session_t *s)
{
    if (s == NULL) {
        return false;
    }
    truing_orch_snapshot_t snap;
    truing_orch_snapshot(s->orch, &snap);
    return send_frame(s, truing_wire_encode_state(&snap, s->scratch, sizeof(s->scratch)));
}

truing_wire_error_t truing_wire_session_handle(truing_wire_session_t *s, const char *json, size_t len)
{
    if (s == NULL) {
        return TRUING_WIRE_ERR_MALFORMED;
    }
    truing_wire_command_t cmd;
    const truing_wire_error_t err = truing_wire_decode_command(json, len, &cmd);

    truing_intent_verdict_t verdict = TRUING_INTENT_ADMIT_ACCEPT;
    truing_reason_t reason = TRUING_REASON_NONE;

    if (err == TRUING_WIRE_OK) {
        switch (cmd.kind) {
        case TRUING_WIRE_CMD_GET_CURRENT_STATE:
            /* SPEC §12.2: present state only, idempotent, safe to issue at any time. */
            (void)truing_wire_session_send_state(s);
            break;
        case TRUING_WIRE_CMD_GET_CURRENT_CYCLE_PROVENANCE: {
            /* P6: the in-RAM authoritative record explaining the adjustment on offer. */
            truing_cycle_provenance_t prov;
            truing_orch_provenance(s->orch, &prov);
            (void)send_frame(s, truing_wire_encode_provenance(&prov, s->scratch, sizeof(s->scratch)));
            break;
        }
        case TRUING_WIRE_CMD_INTENT:
            /* The decoded intent goes to the orchestrator unexamined: every SPEC §12.3
             * admissibility rule has exactly one implementation, and it is not here. */
            verdict = truing_orch_submit_intent(s->orch, &cmd.intent, &reason);
            break;
        default:
            break;
        }
    }

    if (err == TRUING_WIRE_OK && verdict == TRUING_INTENT_ADMIT_ACCEPT) {
        s->commands_accepted++;
    } else {
        s->commands_rejected++;
    }

    /* Exactly one acknowledgement per inbound frame, always, so a client is never left
     * waiting on a command the firmware silently discarded. A rejected frame keeps its
     * `seq`, which is what lets the client tell WHICH frame was refused. */
    (void)send_frame(s, truing_wire_encode_ack(&cmd, err, verdict, reason, s->scratch, sizeof(s->scratch)));
    return err;
}
