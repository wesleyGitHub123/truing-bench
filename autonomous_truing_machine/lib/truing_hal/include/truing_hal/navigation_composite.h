/**
 * @file navigation_composite.h
 * COMPOSITE Wheel Navigation: a person moves the wheel where it matters physically, a stand-in
 * answers everywhere else (SPEC §10A; SOLENOID_CAMPAIGN_PLAN.md A6 / B1b).
 *
 * Why it exists. The acoustic demonstration image fires REAL solenoids but runs a SYNTHETIC runout
 * and solve. With purely synthetic navigation the orchestrator believes it has positioned spoke i at
 * its acoustic station while nothing moved, so the derived station's actuator strikes whatever spoke
 * happens to be under it and the measurement is filed under a spoke that was never struck. That is a
 * correctness bug, not a cosmetic one. Here the stations that need a real spoke under a real
 * plunger go to the operator; the stations whose data is simulated stay simulated.
 *
 * Routing is by STATION, because a positioning request always names the feature and the station
 * (§10A.3): stations listed as physical go to the physical child, every other station to the
 * simulated child.
 *
 * One authority. query() answers from the PHYSICAL child only. A simulated positioning never
 * overwrites it: the demo's runout is not a physical event, and letting it rewrite where the wheel is
 * believed to be would make the position authority describe a wheel that does not exist. Each
 * physical positioning is confirmed by the operator and re-anchors the rotation state, so nothing
 * accumulates. Simulated requests are refused until the physical reference exists.
 *
 * What query() therefore does NOT say: a stand-in DONE is not reflected in it. After a simulated
 * positioning (rim 5 at the runout station) it still answers the last PHYSICAL placement, so the
 * wheel_position it reports -- and the session's end-of-run position -- describes the last placement
 * the operator confirmed, not the last positioning the orchestrator requested. That reinterprets
 * SPEC 10A.2's DONE ("the feature is at the station") for simulated stations; it is defensible only
 * because those stations' data is simulated and the source is labelled SYNTHETIC. The telemetry
 * NAVIGATION event for a simulated request likewise pairs a simulated target with the physical rotation.
 *
 * Provenance. The composite is as real as its least real part: source_impl is the weakest of the
 * two children's, so a composite with a synthetic child is SYNTHETIC. The vocabulary has no "mixed"
 * value, and adding one would be a spec reconciliation. The interface's impl_name states the mix, but
 * that name is not persisted: the only navigation provenance that reaches a session record is the
 * boolean derived from source_impl (truing_session_any_non_real), and runout was already synthetic, so
 * the composite changes nothing there. "Operator-confirmed at the acoustic stations" is therefore on
 * the interface and in telemetry, not in the session header.
 *
 * Contract only: this depends on navigation_if.h and on nothing that could reach a wheel drive.
 */
#ifndef TRUING_HAL_NAVIGATION_COMPOSITE_H
#define TRUING_HAL_NAVIGATION_COMPOSITE_H

#include <stdbool.h>
#include <stddef.h>

#include "truing_hal/navigation_if.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TRUING_NAV_COMPOSITE_NO_CHILD = 0,   /* nothing active */
    TRUING_NAV_COMPOSITE_PHYSICAL,
    TRUING_NAV_COMPOSITE_SIMULATED,
} truing_nav_composite_child_t;

typedef struct {
    truing_navigation_if_t      *physical;    /* the operator: navigation_manual */
    truing_navigation_if_t      *simulated;   /* the stand-in: navigation_synthetic */
    bool                         physical_station[TRUING_STATION__COUNT];
    truing_nav_composite_child_t active;      /* which child holds the active request */
    uint32_t                     physical_requests;
    uint32_t                     simulated_requests;
    char                         name[128];   /* impl_name, built from the children's */
} truing_navigation_composite_ctx_t;

/* Both children must already be initialised. `physical_stations` names the stations answered by the
 * physical child (at least one). Returns false, leaving `self` refusing everything, for a NULL child,
 * the same child given twice, or an empty, unset or out-of-range station list. */
bool truing_navigation_composite_init(truing_navigation_if_t *self, truing_navigation_composite_ctx_t *ctx,
                                      truing_navigation_if_t *physical, truing_navigation_if_t *simulated,
                                      const truing_station_id_t *physical_stations, unsigned n_physical_stations);

#ifdef __cplusplus
}
#endif
#endif /* TRUING_HAL_NAVIGATION_COMPOSITE_H */
