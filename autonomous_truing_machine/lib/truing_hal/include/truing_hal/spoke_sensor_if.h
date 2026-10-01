/**
 * @file spoke_sensor_if.h
 * Spoke presence sensor at an acoustic station: SCAFFOLD for the planned automatic roller mode.
 *
 * The plan (operator, 2026-10-02; hardware not delivered): a laser break-beam across the strike point,
 * transmitter on one side of the wheel and receiver on the other, near the station's plunger. A spoke
 * passing through the beam breaks it. The roller turns the wheel at a known speed; on the beam break of
 * the wanted spoke it decelerates so the wheel stops with that spoke at the solenoid. Position knowledge
 * then comes from beam edges, NOT from step counts: on the spring-loaded roller a back-and-forth of the
 * same step count did not return the wheel to the same spoke (2026-10-01), so steps are a distance
 * only. The beam being blocked when the wheel stops is the corroboration a navigation implementation
 * can report as `sensor_confirmed` (navigation_if.h).
 *
 * Only the seam and a fake exist. TRUING_SPOKE_SENSOR (build_mode.h) refuses to build until a real
 * implementation and its bench check exist; docs/DEMO_BUILD.md describes the intended navigation.
 */
#ifndef TRUING_HAL_SPOKE_SENSOR_IF_H
#define TRUING_HAL_SPOKE_SENSOR_IF_H

#include <stdbool.h>
#include <stdint.h>

#include "truing/status.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct truing_spoke_sensor_if truing_spoke_sensor_if_t;

typedef enum {
    TRUING_SPOKE_EDGE_ENTER = 1,   /* the beam became blocked: a spoke arrived */
    TRUING_SPOKE_EDGE_LEAVE = 2,   /* the beam is clear again: the spoke left */
} truing_spoke_edge_t;

/* One beam edge, timestamped where it happened (an ISR, for a real sensor), with the drive's step
 * counter at that instant so the deceleration distance can be planned from it. */
typedef struct {
    truing_spoke_edge_t edge;
    uint32_t            t_us;
    int32_t             drive_steps;
} truing_spoke_event_t;

struct truing_spoke_sensor_if {
    const char          *impl_name;
    truing_source_impl_t source_impl;
    bool (*available)(truing_spoke_sensor_if_t *self);                            /* fitted and powered */
    bool (*blocked)(truing_spoke_sensor_if_t *self);                              /* a spoke is in the beam now */
    bool (*next_event)(truing_spoke_sensor_if_t *self, truing_spoke_event_t *out); /* oldest unread edge; false if none */
    void *ctx;
};

/* Fake sensor for host tests: script edges with truing_spoke_sensor_fake_push(); `blocked` follows them. */
#define TRUING_SPOKE_SENSOR_FAKE_QUEUE 16u
typedef struct {
    bool                 fitted;
    bool                 blocked;
    truing_spoke_event_t queue[TRUING_SPOKE_SENSOR_FAKE_QUEUE];
    uint8_t              head, count;
    uint32_t             dropped;   /* edges pushed while the queue was full */
} truing_spoke_sensor_fake_ctx_t;

void truing_spoke_sensor_fake_init(truing_spoke_sensor_if_t *self, truing_spoke_sensor_fake_ctx_t *ctx, bool fitted);
bool truing_spoke_sensor_fake_push(truing_spoke_sensor_fake_ctx_t *ctx, truing_spoke_edge_t edge, uint32_t t_us,
                                   int32_t drive_steps);

#ifdef __cplusplus
}
#endif
#endif /* TRUING_HAL_SPOKE_SENSOR_IF_H */
