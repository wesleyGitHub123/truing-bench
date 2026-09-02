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
| 1b–1h, 2+ | Not started. Next dependency-unlocked slices: 1d (orchestrator; depends only on 1a) and 1b (host model preparation; starts with verifying `bike-wheel-calc` runs, SPEC 8.10). |

## Repository layout

```
Truing Repo/                      Git root (independent of the acoustic research repo)
└── autonomous_truing_machine/    PlatformIO project; the specification lives here
    ├── lib/truing_core/          framework-free core (SPEC P3): no ESP-IDF, no RTOS, no I/O
    ├── lib/truing_hal/           subsystem contracts (acoustic, runout, navigation, wheel drive,
    │                             operator intent source, telemetry, clock) + stub/synthetic impls
    ├── lib/truing_fixtures/      SYNTHETIC fixture configurations for tests and bring-up (not defaults)
    ├── lib/truing_board/         board profiles (all pins, SPEC 4.3)
    ├── src/                      ESP-IDF application: bring-up, NVS config store
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

## Open items carried forward (do not invent)

Everything in SPEC 16 and 10A.10. In addition, for this phase: no real wheel
configuration exists yet (hub geometry, `c` per side, the influence fingerprint
and the tension-model parameters all require measurement or model preparation),
so a normal session cannot be admitted until 1b and a measurement campaign
supply them; the firmware reports this honestly as "not provisioned".
