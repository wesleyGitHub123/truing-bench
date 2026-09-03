/**
 * @file net_transport.h
 * SPEC §12.1 primary transport: the ESP32 hosts its own WiFi network in AP mode and
 * serves the web UI, so the operator connects with a browser and installs nothing.
 *
 * This is a DRIVER. Every frame it carries is produced and consumed by truing_proto,
 * which knows nothing about WiFi — that is what makes the §12.1 USB CDC fallback a
 * change of transport only, with the same frontend and the same frames.
 *
 * ---- The threading contract ------------------------------------------------------
 *
 * The web server runs on its own task and the orchestrator on another, so nothing here
 * touches the state machine. Frames cross between them through two queues:
 *
 *   inbound   the websocket handler enqueues raw text; the orchestrator task collects
 *             it with truing_net_poll_inbound() and feeds truing_wire_session_handle()
 *   outbound  the sink (called ON the orchestrator task) enqueues a copy and returns
 *             immediately; a sender task performs the blocking socket write
 *
 * The outbound queue is what keeps SPEC §12.2's "never blocks the control path" true in
 * the presence of a real socket: a slow or dead client fills the queue and frames are
 * dropped, which telemetry explicitly permits, rather than stalling a measurement.
 *
 * ---- Credentials -----------------------------------------------------------------
 *
 * The AP's SSID and passphrase are derived from the board's own MAC address and logged
 * on the debug console at start-up. Nothing is stored in the repository: a checked-in
 * passphrase would be a shared secret published to everyone who can read the source.
 */
#ifndef TRUING_NET_TRANSPORT_H
#define TRUING_NET_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "truing_proto/session.h"

typedef struct {
    uint32_t frames_sent;
    uint32_t frames_dropped;    /* queue full, no client, or out of memory */
    uint32_t frames_received;
    uint32_t inbound_dropped;   /* inbound queue full, or frame over the wire limit */
    uint32_t clients;
} truing_net_stats_t;

/* Brings up the AP and the HTTP server. False if either failed; the firmware carries on
 * regardless, because SPEC §12.2 makes the host optional for autonomous work. */
bool truing_net_start(void);

/* The outbound sink to hand to truing_wire_session_init(). Never blocks. */
truing_wire_sink_t *truing_net_sink(void);

/* Copies one queued inbound frame out, NUL-terminated. Returns its length, or 0 if none
 * is waiting. Non-blocking; call from the orchestrator's task. */
size_t truing_net_poll_inbound(char *out, size_t cap);

bool truing_net_client_connected(void);
void truing_net_get_stats(truing_net_stats_t *out);
bool truing_net_started(void);

/* ---- SPEC §9.4 RF load generator (bring-up only) ---------------------------------
 *
 * SPEC §9.4 requires the capture path to be shown intact while the radio is busy, and
 * a soft-AP with nobody associated is nearly silent — beacons and no more. So the
 * generator injects raw 802.11 frames on the AP's own interface, addressed from and to
 * this board, purely to put the transmitter, its DMA and its interrupts to work while a
 * capture runs. It is a self-test load on the device's own radio and channel; it is not
 * traffic for anyone else, and nothing outside bring-up calls it.
 *
 * Returns false when WiFi is not up, or when the driver refuses injection — in which
 * case the caller must report the test as NOT PERFORMED rather than as passed. */
bool truing_net_rf_load_start(void);
void truing_net_rf_load_stop(uint32_t *frames_injected, uint32_t *inject_failures);

#endif /* TRUING_NET_TRANSPORT_H */
