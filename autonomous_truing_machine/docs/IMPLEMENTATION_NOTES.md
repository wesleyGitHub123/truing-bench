# Implementation notes

Decisions, specification reconciliations and phase status that are not
derivable from the code itself. The specification
(`docs/SPEC.md`) is the authority; where a
note below records a reconciliation, the specification's consistency guardrail
was applied and the more specific normative rule was followed.

## Phase status

| Phase | Status |
|---|---|
| 1a Scaffolding | Implemented: data model, configuration, status/provenance, HAL contracts with stub/synthetic implementations, Wheel Navigation contract with manual and synthetic implementations, both PlatformIO environments, NVS persistence, on-device bring-up. See the commit history for the verification performed at each step. |
| 1d Orchestrator | Implemented (`lib/truing_orchestrator`): the SPEC 7 state machine with both loops, operator waits, wait-instance correlation, abort at safe points, admission with bounded re-measurement, safety bound and deadband, verification-grade convergence gating, stall/max-cycle aborts, current-state snapshot and current-cycle provenance. Consumes Wheel Navigation by outcome only and the truing calculation through the `truing_calc` contract (stub + synthetic double until 1c). Host-tested end to end with zero hardware; self-played on the DevKitC-1 (docs/BRINGUP_LOG.md). |
| 1g Runout manual entry | Implemented (`runout_manual.c`): a real implementation; entries arrive through `SUBMIT_RUNOUT` and are consumed by `read_snapshot()` without blocking. |
| 1b Host model preparation | Implemented (`../model_prep`, Python): `bike-wheel-calc` verified (commit 6fc380c, its 52 tests pass); per-spoke analytical Φ_u/Φ_v/Φ_t generation; artifact assembly with per-layout pseudoinverses, rank/conditioning, common-mode evaluation, fingerprints and load checks; host reference two-part solve; SPEC 14.3 host gate (29 tests). Golden fixture artifact and parity cases in `model_prep/golden/`. Two findings below need the owner's attention before mean-tension targeting can ever be enabled. |
| 1c, 1e, 1f, 1h, 2+ | Not started. 1c (firmware truing calculation) is now unlocked by the 1b host gate: compact artifact export for flash, load with the three checks, matrix-vector solve on the active row set, cost, and parity against `model_prep/golden/parity_sym32.json`. 1e (comms) and 1f (acoustic) depend only on 1a. |

## Repository layout

```
Truing Repo/                      Git root (independent of the acoustic research repo)
└── autonomous_truing_machine/    PlatformIO project; the specification lives here
    ├── lib/truing_core/          framework-free core (SPEC P3): no ESP-IDF, no RTOS, no I/O
    ├── lib/truing_hal/           subsystem contracts (acoustic, runout, navigation, wheel drive,
    │                             operator intent source, telemetry, clock) + stub/synthetic impls
    ├── lib/truing_fixtures/      SYNTHETIC fixture configurations for tests and bring-up (not defaults)
    ├── lib/truing_board/         board profiles (all pins, SPEC 4.3)
    ├── lib/truing_calc/          truing-calculation contract (SPEC 8) with stub and synthetic double; real impl is 1c
    ├── lib/truing_orchestrator/  the state machine (SPEC 7) and the auto-operator test driver
    ├── src/                      ESP-IDF application: bring-up, NVS config store, workflow self-play demo
    ├── test/                     PlatformIO native (Unity) test suites
    └── docs/                     this file, BUILD.md
```

The specification's `SPEC.md` at the repo root (SPEC 1.2) is realised as the
file the project owner placed in `autonomous_truing_machine/`; it was not
renamed or moved.

## Decisions (trivial implementation details, SPEC 17.2)

- **Language: C11** for the core and HAL. It compiles identically under the
  xtensa toolchain and the old host MinGW GCC, needs no runtime, and the
  framework-free boundary is a plain function-pointer vtable per interface.
- **Enumerations have an explicit `UNSET = 0` value** that validation rejects,
  because C zero-initialises structures and SPEC 6.2 says `status` has no
  default. A record that was never assigned a status is invalid by construction.
- **Reason-code extensions beyond the SPEC 13.2 minimum set:** `CANCELLED`
  (a measurement discarded because ABORT cancelled it at the acoustic boundary,
  SPEC 7.4/12.3) and `WHEEL_REFERENCE_LOST` (the navigation subsystem cannot
  vouch for the wheel position, SPEC 10A). The specification calls its list a
  minimum set; both are recorded here rather than folded into an existing code
  with a different meaning.
- **Stub implementations carry `source_impl = synthetic`.** The enumeration has
  no "stub" value; a stub is not real hardware, so any session using one is
  correctly flagged `contains_non_real_implementations`. The manual (Stage 1)
  navigation implementation is `real`: its position knowledge comes from a real
  operator on a real wheel.
- **`ABORT` in `READY` is rejected** (nothing is operating). SPEC 12.3 admits it
  in "all operational states"; READY is not one.
- **Machine profile (station geometry) contents are profile-lifecycle data**,
  like the chain profile, and are not `SET_PARAMETER` targets (SPEC 11.6,
  12.3.1). No parameter id was added for them.
- **Wheel coordinates:** spoke `i` sits at rim angle `2πi/n_spokes` and rim
  index `k` at `2πk/n_rim_angles`; spoke 0 is the origin (SPEC 6.5), so with
  the Stage 1 policy `n_rim_angles = n_spokes` runout stop `k` is spoke
  `k`'s location (SPEC 8.9).
- **Two numerical guards are not physical tolerances:**
  `TRUING_RIM_ANGLE_GRID_EPS_RAD` (a stored runout angle must equal the grid
  angle for its index) and `TRUING_DECLARATION_MATCH_REL_EPS` (two declarations
  of the same fact must agree). Physical tolerances are configuration.
- **Capacity bounds** (`TRUING_MAX_SPOKES = 36`, `TRUING_MAX_CANDIDATE_PEAKS =
  8`) are structural, from SPEC 11.1's supported spoke counts and the research
  pipeline's retained peak count.
- **Fixture values are test data.** Reference-wheel figures that appear in
  `lib/truing_fixtures` (c = 473 N/rev, N_lat = 6, N_rad = 13, the reference
  tolerances) are fixture content, never defaults (SPEC P5). Nothing in the
  firmware loads them unless the `s3_devkit_provision` build is flashed on
  purpose, and that build logs loudly that fixture data is in use.
- **Bring-up I2S probe.** The firmware executes the SPEC 9.4.1 bring-up check
  (`i2s_new_channel()` succeeds with PSRAM enabled) with probe-only DMA sizing;
  it builds no capture path. Phase 2 derives the real sizing from the WiFi-load
  stress test.
- **Build path.** PlatformIO's ESP-IDF integration refuses paths with spaces;
  ESP-IDF builds run through a directory junction (docs/BUILD.md). The
  `platform` is pinned to `espressif32@6.10.0` because this PC also has a fork
  registered under the short name `espressif32`.
- **Orchestrator execution model.** A cooperative `step()` machine: each call does
  one bounded autonomous action and reports what it needs (idle, operator wait,
  settle delay, navigation poll, terminal). Intents are validated and recorded
  by `submit_intent()`; transitions happen only inside `step()`. A FreeRTOS task
  on core 0 drives it on the target; tests drive it directly.
- **VERIFY re-measurement is the next cycle's state.** `VERIFY` re-measures
  through the same interfaces as the measurement phase (SPEC 5.1) and stamps the
  records with the next cycle index; the following `MEASURE_WHEEL_STATE` finds
  the cycle complete and passes through to admission. The SPEC 7.1 transitions
  are preserved, nothing is measured twice, and no record is re-stamped. If no
  adjustment was applied (every spoke within the deadband and tolerance), VERIFY
  evaluates the current state without re-measuring.
- **Progress baseline.** `J` of the state a plan corrects (from the solve) is the
  first baseline; each verification's `J` becomes the next. `cycles_run` counts
  outer cycles that reached `EVALUATE_CONVERGENCE`; `max_cycles` bounds it.
- **Truing calculation contract.** The orchestrator never sees Φ, Φ† or weights:
  `truing_calc_if` exposes model availability with artifact identity, policy
  layout selection, R3 conditioning against artifact-recorded values, the solve,
  optional target prediction and per-channel verification. The synthetic double
  is scripted test data (turns proportional to lateral runout; displacement-only
  cost) and is never a demonstration configuration.
- **Geometry-only policy.** SPEC 8.11 sanctions "operator/config requests
  geometry-only" but §11.2 lists no field for it, so it is an orchestrator
  dependency flag (`geometry_only_policy`), not persisted configuration.
- **Navigation fault while positioning for adjustment.** SPEC 10A.10 leaves the
  terminal handling of an unrecoverable navigation fault open. Provisionally the
  run terminates `ABORT_OPERATOR` carrying the navigation reason; a fault while
  positioning for a *measurement* records that measurement as `unavailable` and
  collection continues (SPEC 13.1). Recorded measurements are never touched.
- **`CONFIRM_POSITIONED` after `ABORT`.** A terminal result clears the wait, so
  a late confirmation for the abandoned wait is rejected (no active wait), not
  accepted into a dead session. `truing_orch_reset_to_ready()` starts a new
  session; it routes through INITIALIZE again if the wheel reference was lost.

## Specification reconciliations

- **v1.1 Wheel Navigation clarification (SPEC 10A, 11.6).** The read-only
  "Position Tracking" subsystem of v1.0 was replaced, before any consumer
  existed, by the Wheel Navigation / Positioning contract: requests name a
  wheel feature and the station that needs it; outcomes are `DONE`,
  `PENDING_OPERATOR`, `IN_MOTION`, `REFUSED`, `FAULT`; the manual Stage 1
  implementation resolves through `WAIT_FOR_OPERATOR`; station angles are
  machine-profile data; actuator coordinates stay behind the wheel-drive
  contract. Positioning prompts gained a `station` and a rim-angle target,
  the session header a `machine_profile_id`, telemetry a navigation event, and
  the board profiles reserved wheel-drive pins. The specification was updated
  in the same change so both stay consistent.
- **SPEC 7.5.1 `consecutive_non_improving_cycles ≥ 2`** is guidance until the
  threshold is derived; validation enforces `≥ 1` so a derived value of 1 is
  not rejected by the data layer.
- **SPEC 8.9 `n_rim_angles = n_spokes`** is enforced as a cross-configuration
  rule (`truing_config_check_pair`), not inside `WheelState`, which accepts any
  supported grid so the data model does not encode the Stage 1 policy.

## Model-preparation findings (Phase 1b) — need the owner's decision

Measured on the SYNTHETIC sym32 fixture (bike-wheel-calc example rim section,
2.0 mm spokes, 45 mm flanges at 35 mm offset, 3-cross, 1000 N, N_lat 6, N_rad 13,
32 rim angles); numbers will differ for the real wheel but the structure will not.

1. **Equal tightening is not displacement-invisible in the analytical model.**
   Tightening every spoke by 1/√32 turn produces zero lateral response (5e-16 mm)
   but a UNIFORM radial contraction of the rim of 0.025 mm (non-uniform remainder
   2e-4 mm). Consequently the tolerance-normalised displacement block has full
   rank (32) for BOTH layouts (σ_min 1.07, effective condition 47.9,
   `expected_null_dim = 0`), and condition (i) of SPEC 8.4 Part 1 fails with
   residual 2.027, of which the uniform radial part is 2.027 and the remainder
   0.017. The artifact records all of this and sets `identified = false`; the
   runtime therefore refuses mean-tension targeting and truing runs under
   `TENSION_ABSENT` (SPEC 8.4 fallback), which is the conservative behaviour the
   specification mandates. **The open question:** whether condition (i) is meant
   to hold for *runout* (non-uniform displacement, i.e. after removing the n = 0
   radial mode, which a tare at the reference location would absorb) or for raw
   displacement including a uniform radius change. The specification's
   "structurally rank-deficient TENSION_ABSENT" expectation is not borne out by
   the model under the raw reading. This is a solver-semantics decision (SPEC
   consistency guardrail §1) and was NOT decided here; the raw reading is
   implemented and the decomposition is recorded so either choice can be made
   without regenerating anything but the flag.
2. **Condition (ii) deviation of 0.38 %.** The tension response to equal
   tightening is uniform to within 0.75 % between the two symmetric class pairs
   (B-lead/A-trail: 110.9 N per turn; A-lead/B-trail: 111.8 N per turn); within
   each pair it is exactly equal. The fixture's `n_mt_tension_tolerance` (1e-3)
   is tighter than that. Whether the project tolerance should admit this
   lead/trail effect is a configuration decision; the gate asserts the recorded
   rule and the pair structure, not a pass.
3. **Single-solve sensitivity to gauge noise.** With 0.01 mm noise on every
   lateral and radial reading, one `TENSION_ABSENT` solve on this wheel recovers
   a 0.2-turn-scale disturbance with ~36 % error (norm bound 0.80 turn). This is
   the reason the outer cycle loop and the non-decrease abort exist (SPEC 8.1,
   7.5); it is not a defect, but it sets expectations for Stage 1 dial
   readings.
4. **FULL-layout inversion and the tension scale.** Because the tension residual
   is `T − s·T_norm` with `s` the current scale, the mean of the tension
   disturbance is removed before the inversion; `d_ls` equals `d_applied` plus
   exactly the pseudoinverse image of that removed mean (identity holds to
   1e-15; ~1e-5 relative at trust_tension = 1e-5). The gate asserts both.

Reconciliations made for 1b: the nipple thread pitch (spoke shortening per
revolution) is required to express Φ per revolution and is not named in the
SPEC 11.1 field list; it is treated as spoke geometry, recorded in the
generating parameters and fingerprinted (SPEC 8.7). The Rayleigh-Ritz mode
count of the bike-wheel-calc solution (36) and the dense fit sample counts
(256, ≥ 4× the floor) are generator parameters, recorded and fingerprinted. The
fixture rim section is bike-wheel-calc's own example section, labelled as such.

## Open items carried forward (do not invent)

Everything in SPEC 16 and 10A.10. In addition, for this phase: no real wheel
configuration exists yet (hub geometry, `c` per side, the influence fingerprint
and the tension-model parameters all require measurement or model preparation),
so a normal session cannot be admitted until 1b and a measurement campaign
supply them; the firmware reports this honestly as "not provisioned".
