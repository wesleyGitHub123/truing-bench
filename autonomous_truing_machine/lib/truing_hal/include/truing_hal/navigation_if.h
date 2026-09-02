/**
 * @file navigation_if.h
 * Wheel Navigation / Positioning subsystem contract (SPEC §10A).
 *
 * Fundamental responsibility: rotate the wheel so that a requested wheel feature
 * (spoke i, rim index k, or rim angle θ) is aligned with the physical STATION
 * that currently requires it. It is the single authority on the wheel's logical
 * position. Consumers (orchestrator, acoustic, runout, adjustment) request
 * positioning through this contract and never own wheel drive, station offsets,
 * or actuator-to-wheel conversion.
 *
 * Both Stage implementations satisfy the same contract:
 *   - MANUAL (Stage 1): the human is the actuator. request() answers
 *     PENDING_OPERATOR with the prompt kind to issue; the orchestrator runs
 *     WAIT_FOR_OPERATOR and, on CONFIRM_POSITIONED, calls note_operator_confirmation().
 *   - AUTOMATED (Stage 2): request() commands the wheel drive and answers
 *     IN_MOTION; the orchestrator polls until DONE or FAULT.
 * The orchestrator's POSITION state handles both by outcome alone.
 *
 * Coordinates: targets are WHEEL coordinates; stations are MACHINE coordinates
 * from the machine profile; ACTUATOR coordinates never appear here
 * (truing/wheel_geometry.h, truing_hal/wheel_drive_if.h).
 */
#ifndef TRUING_HAL_NAVIGATION_IF_H
#define TRUING_HAL_NAVIGATION_IF_H

#include <stdbool.h>
#include <stdint.h>

#include "truing/config.h"
#include "truing/operator_intent.h"
#include "truing/status.h"
#include "truing_hal/clock_if.h"
#include "truing_hal/wheel_drive_if.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TRUING_NAV_TARGET_UNSET = 0,
    TRUING_NAV_TARGET_SPOKE,       /* spoke `index` */
    TRUING_NAV_TARGET_RIM_INDEX,   /* rim index `index` */
    TRUING_NAV_TARGET_RIM_ANGLE,   /* arbitrary rim angle `rim_angle_rad` (wheel coordinates) */
} truing_nav_target_kind_t;

typedef struct {
    truing_nav_target_kind_t kind;
    uint8_t                  index;
    float                    rim_angle_rad;
    truing_station_id_t      station;   /* which station requires the feature */
} truing_nav_target_t;

typedef enum {
    TRUING_NAV_IDLE = 0,           /* no request active */
    TRUING_NAV_DONE,               /* feature is at the station, as far as the implementation can establish */
    TRUING_NAV_PENDING_OPERATOR,   /* manual implementation: prompt the operator (wait_kind) */
    TRUING_NAV_IN_MOTION,          /* automated implementation: poll for completion */
    TRUING_NAV_REFUSED,            /* invalid target, unknown station, or no reference */
    TRUING_NAV_FAULT,              /* motion fault; wheel position no longer vouched for */
} truing_nav_outcome_t;

typedef struct {
    truing_nav_outcome_t outcome;
    truing_reason_t      reason;      /* for REFUSED / FAULT */
    truing_wait_kind_t   wait_kind;   /* for PENDING_OPERATOR: the prompt to issue */
    truing_nav_target_t  target;      /* the request this result describes */
} truing_nav_result_t;

/* The wheel's logical position — ONE authority (SPEC §10A). */
typedef struct {
    truing_status_t status;                /* valid: rotation_rad is vouched for; unavailable: no reference */
    truing_reason_t reason;
    bool            reference_established;
    float           rotation_rad;          /* R: wheel feature θ sits at machine angle wrap(θ + R) */
    bool            operator_confirmed;    /* last positioning confirmed by the operator */
    bool            sensor_confirmed;      /* corroborated by an index/reference sensor or encoder */
    uint32_t        timestamp_ms;
} truing_wheel_position_t;

typedef struct truing_navigation_if truing_navigation_if_t;

struct truing_navigation_if {
    const char          *impl_name;
    truing_source_impl_t source_impl;
    void (*request)(truing_navigation_if_t *self, const truing_nav_target_t *target, truing_nav_result_t *out);
    void (*poll)(truing_navigation_if_t *self, truing_nav_result_t *out);
    /* Manual implementation: the operator confirmed the ACTIVE request (CONFIRM_POSITIONED). */
    void (*note_operator_confirmation)(truing_navigation_if_t *self, truing_nav_result_t *out);
    /* Establish (or re-establish) the wheel reference: spoke 0 at the reference station (SPEC §6.5). */
    void (*establish_reference)(truing_navigation_if_t *self, truing_nav_result_t *out);
    void (*query)(truing_navigation_if_t *self, truing_wheel_position_t *out);
    /* Safe stop of any wheel motion (ABORT safe point). */
    void (*stop)(truing_navigation_if_t *self);
    void *ctx;
};

/* Null-safe wrappers; a missing implementation answers REFUSED / NOT_IMPLEMENTED. */
void truing_navigation_request(truing_navigation_if_t *self, const truing_nav_target_t *target, truing_nav_result_t *out);
void truing_navigation_poll(truing_navigation_if_t *self, truing_nav_result_t *out);
void truing_navigation_confirm(truing_navigation_if_t *self, truing_nav_result_t *out);
void truing_navigation_establish_reference(truing_navigation_if_t *self, truing_nav_result_t *out);
void truing_navigation_query(truing_navigation_if_t *self, truing_wheel_position_t *out);
void truing_navigation_stop(truing_navigation_if_t *self);

/* Wheel-coordinate angle of a target feature; NaN when the target is invalid. */
float truing_nav_target_wheel_angle(const truing_nav_target_t *t, uint8_t n_spokes, uint8_t n_rim_angles);
/* Prompt kind the manual implementation issues for a target kind. */
truing_wait_kind_t truing_nav_target_wait_kind(truing_nav_target_kind_t kind);
/* Convert a navigation result into the wait prompt to issue (PENDING_OPERATOR only). */
bool truing_nav_result_to_prompt(const truing_nav_result_t *r, truing_wait_prompt_t *prompt_out);

const char *truing_nav_outcome_str(truing_nav_outcome_t o);
const char *truing_nav_target_kind_str(truing_nav_target_kind_t k);

/* ---- MANUAL implementation (Stage 1: the human is the actuator) ---------------- */
typedef struct {
    truing_clock_if_t               clock;
    uint8_t                         n_spokes;
    uint8_t                         n_rim_angles;
    const truing_machine_profile_t *machine;
    truing_nav_result_t             active;
    truing_wheel_position_t         position;
    uint32_t                        requests;
    uint32_t                        confirmations;
} truing_navigation_manual_ctx_t;

void truing_navigation_manual_init(truing_navigation_if_t *self, truing_navigation_manual_ctx_t *ctx,
                                   truing_clock_if_t clock, uint8_t n_spokes, uint8_t n_rim_angles,
                                   const truing_machine_profile_t *machine);

/* ---- SYNTHETIC automated implementation (tests / simulation, SPEC §14.5) ----------- */
typedef struct {
    truing_clock_if_t               clock;
    uint8_t                         n_spokes;
    uint8_t                         n_rim_angles;
    const truing_machine_profile_t *machine;
    truing_wheel_drive_if_t        *drive;                 /* optional actuator to command */
    float                           steps_per_wheel_rev;   /* CALIBRATION knob (test-provided); required if drive != NULL */
    uint8_t                         polls_per_move;        /* 0 = moves complete inside request() */
    uint8_t                         polls_remaining;
    float                           pending_delta_rad;
    float                           pending_target_rotation_rad;   /* R' the implementation believes it reaches */
    float                           true_rotation_rad;     /* what the wheel REALLY did (test oracle) */
    float                           scripted_slip_rad;     /* applied to the next move, then cleared */
    bool                            fault_next;            /* next move ends in FAULT */
    truing_nav_result_t             active;
    truing_wheel_position_t         position;
    uint32_t                        requests;
} truing_navigation_synthetic_ctx_t;

void truing_navigation_synthetic_init(truing_navigation_if_t *self, truing_navigation_synthetic_ctx_t *ctx,
                                      truing_clock_if_t clock, uint8_t n_spokes, uint8_t n_rim_angles,
                                      const truing_machine_profile_t *machine, truing_wheel_drive_if_t *drive,
                                      float steps_per_wheel_rev, uint8_t polls_per_move);
float truing_navigation_synthetic_true_rotation(const truing_navigation_synthetic_ctx_t *ctx);

#ifdef __cplusplus
}
#endif
#endif /* TRUING_HAL_NAVIGATION_IF_H */
