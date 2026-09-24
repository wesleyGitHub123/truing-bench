# Automated Bicycle Wheel Truing System — Firmware Architecture Specification

**Document status:** Implementation contract, v1.1 — v1.1 adds the Wheel Navigation / Positioning capability (§10A, §11.6) and propagates it to the sections it touches; no other architecture changed.
**Audience:** AI coding agent implementing this system, and human reviewers
**Repository:** Truing Repo (this document lives at the repo root)

---

## §1 Purpose, Scope, and Repository Topology

### 1.1 What this document is

This is an **implementation contract**. It defines the firmware architecture, subsystem boundaries, data contracts, state machine, and testing requirements for an automated bicycle wheel truing system. An agent implementing this system should treat the constraints here as binding, and should ask rather than assume when this document is silent on something material.

This document is **not** a research report, a mechanical design, or a justification of the underlying physics. Where a decision has a non-obvious rationale, the rationale is stated inline — because the rationale is often the only thing preventing a well-meaning "simplification" that breaks the system.

### 1.2 Repository topology

Two separate repositories exist:

```
<workspace>/
├── truing-repo/          ← THIS repo. All implementation happens here.
│   └── SPEC.md           ← This document.
└── acoustic-repo/        ← Research repo. READ-ONLY reference.
    └── AI_guide.md       ← Governs behavior in acoustic-repo ONLY.
```

**Truing Repo** is the root for all work described in this document. Every file the agent creates or modifies lives here.

**Acoustic Repo** is a separate research codebase containing the in-progress acoustic measurement work: DSP pipeline, frequency extraction, tension models, and recorded measurement campaigns. The agent **may read it** for context — how frequency extraction works, what the data formats are, what the validated algorithms do.

**Three hard constraints on this relationship:**

1. **The truing repo MUST NOT depend on the acoustic repo at build time.** No imports, no submodules, no path references, no package dependency. The acoustic repo is a research environment that churns; making it load-bearing would mean every experiment there can break the firmware build.

2. **Ported code is copied in and versioned.** When acoustic algorithms are ported into the truing repo, copy the code, and record the source commit hash in a comment or manifest. The copy is now truing-repo code with its own lifecycle.

3. **`AI_guide.md` in the acoustic repo governs behavior in that repo only.** It contains vibe-coding rules for research work — rapid iteration, exploratory scripts, looser standards appropriate to experimentation. **Those rules do not apply here.** This repo is production firmware for a physical machine. When the two documents conflict, this one wins inside this repo.

### 1.3 Non-goals

Explicitly out of scope. Do not implement these, and do not expand the architecture to accommodate them unless directed:

- **No ML or learned models in the control path.** Unbounded inference cost breaks the timing contract of a real-time control loop. Closed-form models only.
- **No automated runout sensing in Stage 1.** The interface exists; the implementation is manual entry.
- **No nipple actuation in Stage 1.** The system outputs the required adjustment; a human applies it.
- **No wheel rotation actuation in Stage 1.** The operator rotates the wheel manually — as the Stage 1 *implementation* of the Wheel Navigation / Positioning capability (§10A), not as an absence of the capability.
- **No enterprise patterns.** No microservices, no dependency-injection frameworks, no plugin systems. The simplest structure that satisfies the interface requirements.

---

## §2 System Overview

### 2.1 The final system

A machine that mounts a bicycle wheel, measures it, computes required spoke adjustments, applies them, and verifies the result:

```
mount wheel → [ measure state → compute adjustments → apply → verify ] → done
                        ↑                                          │
                        └──────── if not converged ────────────────┘
```

Where "measure state" means, for each spoke: position the wheel, excite the spoke, capture its acoustic response, extract vibration frequency, estimate tension; and for each rim index: measure lateral and radial displacement.

### 2.2 Stage 1 scope

Stage 1 implements the complete computational and acoustic path, with human actuation. The contractual split:

| System Function | Stage 1 | Notes |
|---|---|---|
| Initialize System Parameters | ✓ | |
| Execute Software Architecture | ✓ | |
| Track Spoke Position | ✓ | The position-authority half of Wheel Navigation / Positioning (§10A) |
| Identify Spoke Frequency | ✓ | |
| Calculate Spoke Tension | ✓ | |
| Process Runout Data | — | Manual entry adapter, not automated sensing |
| Calculate Truing Adjustment | ✓ | |
| Verify Truing Results | ✓ | |
| Mount Wheel | ✓ | Passive fixture |
| Rotate Wheel | — | **Manual implementation** of Wheel Navigation / Positioning (§10A): the operator is the actuator. Automated in Stage 2 behind the same contract |
| Excite Spoke | ✓ | |
| Capture Spoke Vibration | ✓ | |
| Engage Spoke Nipple | — | Operator turns nipples manually |

**The critical architectural consequence:** Stage 1 is a human-in-the-loop system. The state machine must treat operator actions as first-class states, not as glue code around an assumed-automated flow. See §7.

**Wheel navigation is a capability, not a gap.** Wherever the workflow needs the wheel moved — every `POSITION` in the measurement, apply and verification loops — it requests that a wheel feature (a spoke or a rim location) be brought to the **physical station that requires it**, through the Wheel Navigation / Positioning capability (§10A). In Stage 1 the human is the actuator behind that capability; in Stage 2 a wheel drive is. The request, and the workflow around it, do not change.

**What Stage 1 demonstrates:** a complete, testable implementation of the computational and acoustic architecture for the human-in-the-loop scope defined above, verified against synthetic and recorded data.

**What Stage 1 does NOT demonstrate:** physical truing accuracy. That requires automated sensing, actuation, a validated influence matrix for the physical wheel, and empirical validation of the linearity assumption (§8.1) — all deferred to Stage 2. No claim about how well the machine trues a real wheel is supported by Stage 1 work.

---

## §3 Architectural Principles

These generate the specific decisions in later sections. When this document is silent, apply these.

**P1 — Stable interfaces over shared internals.** Components interact through defined contracts. A component never reaches into another's implementation. When an implementation changes (manual entry → sensor driver, host DSP → firmware DSP), only that implementation changes.

**P2 — Placeholders return status, never neutral values.** A stub that returns `0.0` produces a truing calculation confidently derived from nothing. Unimplemented capabilities return an explicit `unavailable` status with a reason code, forcing callers to handle the gap.

**P3 — Core logic is framework-free.** The solver, state machine, and data model must not depend on ESP-IDF, FreeRTOS, or any I/O framework. They take data and return data. This is what makes them testable on a host and portable across the hardware boundary.

**This applies to those three components only.** Hardware drivers, RTOS adapters, the comms layer, and the HAL naturally and correctly use ESP-IDF and FreeRTOS — that is their job. The boundary is: framework-dependent code may *call* core logic; core logic may not call framework-dependent code. Do not attempt to make the whole firmware framework-free.

**P4 — Golden fixtures for every algorithm.** Every non-trivial computation has recorded inputs and expected outputs, checked in. This is what makes a later port verifiable rather than a leap of faith.

**P5 — Config over literals.** Any value that could change with a different wheel, a different sensor, a different research result, or a different tolerance is configuration. Constants in code are for things determined by physics or by this specification.

*Enforcement:* no quantity listed in §11.1–§11.3 may appear as a numeric literal outside its configuration definition. Reference values quoted in this document (μ_v = 0.5, c = 473 N/rev, N_rad = 13, and similar) are provenance for the reader — **they are not defaults to hardcode.**

**P6 — Provenance on every record.** Every measurement and every computed result records what produced it: which implementation, which model, which artifact, which cycle.

**Telemetry alone CANNOT satisfy P6.** Telemetry is best-effort and droppable (§12.2), so a provenance requirement resting on it would be unsatisfiable by construction. The two are separate:

| Channel | Guarantee |
|---|---|
| **Authoritative control provenance** | The ESP32 **retains in RAM** the current cycle's `WheelState` and generated `AdjustmentPlan`, including artifact ID and fingerprint, tension-model profile ID, chain profile ID, active row set and layout, weights and tolerances applied, and every contributing measurement's status and reason code. **Queryable on demand** (§12.2). |
| **Telemetry** | Observational. May drop events, may gap on disconnect, no replay. |

**The rule: data needed to explain the CURRENT adjustment must not exist only in a best-effort queue.** Historical events may be lost; the present authoritative state may not. No event-replay system is required.

**P7 — Fail loudly on structural violations.** Silent degradation is the enemy. Aliasing, dimension mismatches, missing calibration, **unexpected** rank or nullity, and conditioning outside configured limits must raise errors, not produce plausible output. *Expected* structural nullity recorded in the artifact is a normal property, not a violation (§8.5).

---

## §4 Platform and Hardware/Software Boundary

### 4.1 Hardware

**Development target:** ESP32-S3-DevKitC-1 (N16R8 variant — 16 MB flash, 8 MB PSRAM).
**Final form factor target:** Arduino Nano ESP32 (ESP32-S3 based).

Both are ESP32-S3, so **application and core logic port between them.** Peripheral availability, GPIO count, USB topology, and PSRAM presence do **not**. All board-specific differences MUST stay isolated behind the board-profile mechanism (§4.3).

The DevKitC-1 is the development board because it exposes ~40 GPIO (vs. ~20 on the Nano), has separate UART-bridge and native-USB ports, and has an explicit PSRAM variant.

**Known portability constraints to the Nano:**

| Difference | Consequence |
|---|---|
| ~20 GPIO vs ~40 | Peripheral budget tightens as actuators and sensors are added (wheel drive and index sensor for §10A, nipple actuator, runout sensor) |
| **Native USB only** (no separate UART bridge) | **Debug and UI transport cannot occupy separate ports (§12.1).** On the Nano, debug shares the single USB or moves to WiFi/UART-over-GPIO |
| PSRAM presence varies by variant | **Verify on the actual board.** Note PSRAM does **not** relieve DMA buffer sizing — I2S DMA buffers must be internal (§9.4.1) |

None of these block Stage 1 development on the DevKitC-1. They are recorded so the eventual migration is a known set of changes rather than a discovery.

### 4.2 Toolchain

**PlatformIO with ESP-IDF framework.**

ESP-IDF rather than Arduino because Phase 2+ requires direct I2S/DMA control, FreeRTOS task priority management, and `esp-dsp`. Arduino abstracts exactly what needs controlling. Switching frameworks mid-project would churn build configuration during audio bring-up, which is the worst possible time.

**`platformio.ini` must define both environments from day one**, sharing a common base:

```ini
[env]
platform = espressif32
framework = espidf
; shared build flags, monitor settings, library deps

[env:s3_devkit]
board = esp32-s3-devkitc-1
build_flags = ${env.build_flags} -DBOARD_PROFILE_S3_DEVKIT

[env:nano_esp32]
board = arduino_nano_esp32
build_flags = ${env.build_flags} -DBOARD_PROFILE_NANO
```

**Both environments must remain buildable at all times.** Portability breakage should surface immediately, not at the end of the project.

### 4.3 Pin assignments

**All pin assignments live in a single board-profile header per board.** No pin numbers inline in driver code, ever. Switching to the Nano is a build flag plus one header, not an audit. Pins are owned by exactly one subsystem: the wheel-drive and index/reference-sensor pins belong to the Wheel Navigation / Positioning implementation (§10A), the I2S pins to the acoustic front end, and no other code touches them.

### 4.4 What runs where

| Responsibility | Location |
|---|---|
| State machine / orchestration | ESP32 |
| Acoustic acquisition, DSP, tension estimation | ESP32 |
| Truing solve (matrix-vector application) | ESP32 |
| Runout acquisition | ESP32 (driver) or host (manual entry) |
| Wheel navigation / positioning (§10A) | ESP32 (subsystem). C2: manual implementation resolving through operator waits; C3: wheel-drive hardware behind the same contract |
| Configuration storage | ESP32 (NVS/flash) |
| **Influence matrix generation** | **Host (Python), offline** |
| **Pseudo-inverse computation** | **Host (Python), offline** |
| UI rendering | Host (browser) |
| Session logging | Host (with ESP32 emitting events) |

The ESP32 orchestrates. The host provides UI, observability, and offline model preparation.

### 4.4.1 Acoustic processing boundary — EXPLICIT RESOLUTION

**Decision: acquisition, DSP, and tension estimation all run on the ESP32, from Stage 1 onward. The host does not participate in the control path.**

**Rationale — this follows necessarily from ESP32 orchestration.** If the ESP32 orchestrated but the host performed DSP, the ESP32 would have to ship audio to the host and block on a result. That makes the host load-bearing inside the control path, which directly contradicts §12.2. The two choices are not independent: ESP32 orchestration requires ESP32 DSP.

**Feasibility, per acoustic layer (§9.2):**

| Layer | Stage 1 status |
|---|---|
| 1. Front end (I2S) | Implement on-device |
| 2. Spectral analysis / peak candidates | **Port now** — validated and stable in the research repo |
| 3. Mode identification | **Interim implementation.** Unsettled research; replaceable behind the seam |
| 4. Physics model | **Plumbing ports now; model selection and parameters are NOT settled** — see below |

**On layer 4 — implement the mechanism, not a settled answer.** The contract requires "Calculate Spoke Tension," so the capability must exist. What is in scope for Stage 1 is the *plumbing*: multiple candidate models behind one interface, `model_name` recorded on every estimate, model choice as configuration. The formulas themselves are closed-form and short.

**What is NOT settled, and MUST NOT be treated as settled:**

- **Which model is the default.** Several candidates exist (ideal string, stiffness-corrected, higher-mode, empirical) with differing reliability in the research data. Model selection is configuration, not a hardcoded choice.
- **Effective vibrating length.** This is a research output, not a config constant to fill in. The research repo's damping work demonstrated that the effective length in a laced wheel differs materially from the nominal free-span figure — the same measured spoke yielded substantially different fundamentals depending on how the span was constrained. A model fed a wrong `L_eff` produces a confidently wrong tension.
- **Absolute accuracy.** Unvalidated against a traceable force reference.

**Trustworthiness is gated on layer 3.** A tension estimate derived from an interim mode-identification rule inherits that rule's uncertainty regardless of how correct the physics model is: wrong f₁ in, wrong tension out.

**Required behaviour:** while layer 3 is an interim implementation, `TensionEstimate.status` MUST NOT be `valid`. It is `suspect`, with a reason code indicating provisional mode identification. Downstream consumers — most importantly the truing solver — must then treat the tension channel accordingly, and the tension-absent operating mode (§8.5) remains the safe fallback.

This is what makes the Stage 1 claim honest: tension estimation is *implemented and wired end to end*, not *validated*.

Layer 3 being unresolved does **not** block the port. That is the entire purpose of the seam: an interim rule ships now and is replaced when the research settles, with no change above or below it.

**Schedule risk, stated plainly:** porting layer 2 to C is L4 work and now sits on the Stage 1 critical path. Golden fixtures (§14.2) make the port verifiable against the Python reference, which is the mitigation. This is a known cost of the orchestration decision, not an oversight.

**Permitted development fallback:** a host-side DSP implementation may exist behind the same layer-2 interface for development and debugging. If used, it MUST be recorded in `source_impl` and the session flagged accordingly (§6.2). **It is never the demonstration configuration**, because a demo in which the host performs the analysis does not show what the demo claims to show.

**On host-disconnected operation — precisely (§12.2).** Autonomous acquisition, DSP, estimation, solving, and verification already in flight continue without the host, and the machine then **waits safely** at the next operator-dependent state until an input source reconnects. In Stage 1 this is **not standalone operation**: manual positioning, runout entry, and adjustment confirmation all require the host UI (§7.3). The property is real and demonstrable, but it is not "the whole cycle runs unattended.

### 4.5 FreeRTOS task model

Minimum task set, with core affinity:

| Task | Core | Priority | Notes |
|---|---|---|---|
| Audio capture (DMA drain) | 1 | High | **Hard deadline** — must drain before the next buffer fills |
| Acoustic DSP (FFT, peak pick) | 1 | Normal | **Soft deadline** — must finish before the next pluck |
| Orchestrator / state machine | 0 | Normal | |
| Comms / telemetry | 0 | Low | Never blocks; drops on full |
| WiFi stack | 0 | (system) | ESP-IDF managed |

**Capture and DSP have different deadline classes and MUST NOT share a priority level.** DMA drain has a hard, short deadline set by buffer refill period; missing it drops samples and corrupts the spectrum (§9.4). FFT and peak picking have no hard deadline — they merely need to complete before the next excitation.

Running them at the same high priority means compute-heavy DSP occupies the CPU at capture priority for no benefit; running them in one task risks the DSP delaying a drain. **Separate tasks with a queue between them is the expected structure**, but a single task is acceptable if the implementation demonstrably meets the drain deadline under load (§9.4 stress test). The requirement is the deadline separation, not the task count.

**Pinning the audio path to core 1, away from WiFi and comms, is a requirement, not an optimization.**

---

## §5 Subsystem Decomposition

```
┌─ CONTROL LAYER ────────────────────────────────────────────┐
│  Orchestrator  (state machine, cycle + per-spoke loops)     │
└────┬───────────────────────────────────────────────────────┘
     │ drives
     ├──────────────────────┬───────────────────────────────┐
     ▼                      ▼                               ▼
┌─ DOMAIN LAYER (pure, framework-free per P3) ──────────────────┐
│  Wheel State      Truing Calculation      Verification        │
│  (data)           (solver)                (cost, convergence) │
│                        ▲                       │              │
│                        │ reads                 │ reuses       │
│              [Influence Matrix Artifact]       │ measurement  │
│                                                │ interfaces   │
└────────────────────────────────────────────────┼──────────────┘
                                                 │
     ┌───────────────────┬───────────────────┬───┘
     ▼                   ▼                   ▼                ▼
┌─ SUBSYSTEM LAYER (hardware-facing) ───────────────────────────┐
│  Acoustic       Runout        Wheel           Comms /         │
│  (sensor+       (sensor)      Navigation /    Telemetry       │
│   actuator)                   Positioning                     │
└────┬────────────────┬──────────────┬──────────────┬───────────┘
     ▼                ▼              ▼              ▼
┌─ HARDWARE ABSTRACTION LAYER ──────────────────────────────────┐
│        Real    |    Recorded    |    Synthetic                │
└───────────────────────────────────────────────────────────────┘
```

**Dependency rules the diagram encodes — these are binding:**

1. **The domain layer NEVER touches the HAL.** Truing Calculation consumes `WheelState` and the influence matrix artifact and returns an `AdjustmentPlan`. It performs no I/O. This is P3 applied concretely — the solver is pure math and must remain host-testable with no hardware present.
2. **Every hardware-facing component sits in the subsystem layer and goes through the HAL** — including Wheel Navigation / Positioning (index/reference sensor, wheel-drive motor driver) and Comms (WiFi/USB radio and serial peripherals), not only Acoustic and Runout.
3. **Verification is a control/domain module, not a measurement subsystem** — see §5.1.
4. **Wheel State is pure data.** It holds measurements; it does not acquire them.

### 5.1 Subsystem responsibilities

**Orchestrator** — owns the state machine, the cycle loop, and the per-spoke loop. Sequences everything else. Holds no measurement logic.

**Wheel State** — the current model of the wheel: per-spoke tension measurements, per-index runout measurements, cycle history. Pure data plus accessors.

**Wheel Navigation / Positioning** — rotates the wheel so that a requested spoke or rim location is aligned with whichever physical station currently requires it, and is the single authority on the wheel's logical position (§10A). It owns wheel rotation, indexing/localization, reference establishment, station-relative positioning and safe stopping. Stage 1 implements it manually (the operator is the actuator, confirmed through `WAIT_FOR_OPERATOR`); Stage 2 implements it with a wheel drive behind the same contract. Other subsystems request or consume positioning; none of them moves the wheel or knows where a station is.

**Acoustic Subsystem** — **a sensor+actuator composite, not a sensor.** It commands the plucking mechanism, captures the response, and returns a tension estimate. It owns its own internal sequencing, including any damping ritual. See §9.

**Runout Subsystem** — returns lateral and radial displacement at a given rim index. See §10.

**Truing Calculation** — consumes `WheelState`, produces `AdjustmentPlan`. Includes the influence matrix application, the two-part solve, and the cost function. See §8.

**Verification** — **a control/domain module, not a measurement subsystem.** It is a real module with real responsibilities: computing the scalar cost (§8.6), comparing against tolerances, and evaluating convergence and abort conditions. What it does **not** do is own any measurement implementation — it re-invokes the same acoustic and runout interfaces the measurement phase uses. Giving it its own acquisition path would duplicate measurement logic and allow the two paths to diverge.

**Hardware Abstraction** — drivers. Every hardware-touching interface has a real implementation and at least one simulated/recorded implementation.

**Comms / Telemetry** — event emission and command reception. See §12.

### 5.2 What is deliberately NOT a separate subsystem

- **Influence matrix** — it is a *data artifact* consumed by the truing calculation, not an active component.
- **Adjustment planning** — part of truing calculation. Splitting it creates a boundary with nothing meaningful crossing it.
- **Simulation infrastructure** — not a subsystem; it is alternate implementations of the hardware-touching interfaces.

---

## §6 Data Model and Contracts

### 6.1 The measurement chain

```
RawAudioBuffer
     ↓  (acoustic: spectrum + peak candidates)
FrequencyMeasurement
     ↓  (acoustic: physics model)
TensionEstimate
     ↓
SpokeMeasurement ──┐
                   ├──→ WheelState ──→ TruingCalculation ──→ AdjustmentPlan
RunoutMeasurement ─┘                                              ↓
                                                          VerificationResult
```

### 6.2 Universal fields

**Provenance is split between per-record fields and a per-session header.** Do not replicate session-constant information onto every measurement — it bloats embedded structures for no traceability gain.

**Per-record (compact, on every measurement-derived type):**

```
status: enum {valid, suspect, rejected, unavailable, stale}
reason_code: enum        // required when status != valid
cycle_index: uint8
timestamp: uint32        // ms since boot; see §6.6
source_impl: enum        // NOT a string — {real, host_service, recorded, synthetic}
```

**Per-session header (written once at session start):**

```
session_id
firmware_version
influence_matrix_artifact_id        (identity, not just a version string)
influence_matrix_fingerprint
tension_model_profile_id            (§11.3.1 — the session-fixed model)
chain_profile_id
target_tension                      (session-fixed value actually used)
wheel_class_config snapshot
solver_config snapshot (tolerances, trust weights, acceptance limits)
contains_non_real_implementations: bool
```

**Together these answer "why did the system calculate this adjustment"** — the per-record fields locate a measurement in the cycle and state its trustworthiness; the session header pins the model, configuration, and code that consumed it. Neither alone is sufficient; duplicating the header per record is waste.

**`status` is not optional and has no default.** A significant fraction of real measurements in this system land outside `valid` — ambiguous frequency peaks, out-of-range readings, unavailable sensors. The system must represent this rather than assume clean data.

**`contains_non_real_implementations`** is set if any interface in the session was satisfied by a recorded, synthetic, or host-service implementation. This is a data-integrity requirement: a synthetic reading that looks real, six weeks later, is indistinguishable and corrupts your results.

### 6.3 Type-specific requirements

**`FrequencyMeasurement`** — **MUST NOT assert that the selected peak is physically the fundamental.** Mode identification is unresolved research (§9.5), and the research data shows selected peaks that were in fact a neighbouring spoke's mode or a different vibrational mode entirely. A field named "fundamental" encodes a claim the algorithm has not established.

Required fields:

```
selected_frequency_hz          // the peak the rule chose — a selection, not an identification
mode_identity: enum {
    confirmed_fundamental,     // independently corroborated (e.g. harmonic series verified)
    presumed_fundamental,      // selection rule's output, unverified  ← default while §9.5 is open
    unknown
}
candidate_peaks[]              // all in-band candidates considered, with magnitudes
snr_db
selection_rule_version         // which rule produced the selection
```

Candidates are retained because post-hoc analysis of mode-selection errors is an active research need, and because a stored measurement whose candidates were discarded cannot be re-examined when the rule changes.

**While layer 3 is an interim implementation, `mode_identity` is `presumed_fundamental`** and the downstream `TensionEstimate` carries `status: suspect` with `PROVISIONAL_MODE_ID` (§4.4.1).

**`TensionEstimate`** — **must carry `model_name`.** Multiple tension models exist (see §9); no single one is a settled default. A tension value without its model is not interpretable.

**`RunoutMeasurement`** — **must carry `rim_angle`.** A displacement value without its angular index is meaningless. The timestamp is not a substitute.

**`WheelState`** — must tolerate `n_measured < n_spokes`. Partial state is legal (see §7.4). Must expose which spokes and indices are measured, and with what status.

**Representable is not the same as solvable.** `WheelState` records what exists; the solver decides whether that is sufficient, per the admission rules in §8.11.

**`AdjustmentPlan`** — per-spoke required rotation, plus the artifact ID, generating fingerprint, active layout and `active_row_set`, weights and tolerances used, solver version, and relevant policy/reason metadata that produced it.

**`VerificationResult`** — must include a **scalar cost value** (see §8.6), not only raw parameter values. Convergence and abort logic depend on comparing cost across cycles.

### 6.4 Units and sign conventions — MANDATORY

**This section prevents a class of silent, high-cost errors.** Three independent sources use different conventions. If they are not reconciled explicitly, the system produces a correctly-scaled adjustment vector pointing the wrong direction.

**System-internal canonical convention:**

| Quantity | Unit | Positive direction |
|---|---|---|
| Lateral displacement | mm | **Toward Side B** (§6.4.1) |
| Radial displacement | mm | Outward from hub |
| Spoke tension | N | Tensile (always positive) |
| Rim angle | radians | **Counter-clockwise as viewed from Side A** (i.e. looking at the cassette/rotor face), referenced to the valve stem |
| Spoke adjustment | revolutions | Positive = tightening |

**Known conflicts to reconcile at the boundary:**

- **`bike-wheel-calc` uses SI meters**, not mm. Convert at the artifact generator.
- **`bike-wheel-calc` defines positive radial as INWARD toward the hub.** The reference method (Hunter) and this system define it as OUTWARD. **This is a sign flip that must be applied during artifact generation.** Getting it wrong produces a Φv that drives radial corrections backwards.
- **`bike-wheel-calc` positive lateral is toward NDS** — this agrees with our convention. No flip.
- **Rim angle origin:** the reference method references θ to the valve stem; `bike-wheel-calc` uses its own spoke-0 origin. See §6.5.

### 6.4.1 Side naming — front and rear wheels

"Drive side" is meaningless on a front wheel, but `bike-wheel-calc` uses DS/NDS naming and defines positive lateral relative to NDS. A physical, unambiguous mapping is therefore mandatory:

| System term | Definition |
|---|---|
| **Side A** | The side carrying the cassette (rear) or the brake rotor (front) |
| **Side B** | The opposite side |

**Fallback for wheels with neither feature** (e.g. a rim-brake front wheel): the assignment is **arbitrary but MUST be explicitly declared in wheel-class configuration and used consistently** across the artifact generator, the physical machine setup, and the sign convention. An *undeclared* assignment produces an inconsistency between generator and machine even when the choice itself does not matter. Declare it; do not leave it implicit.

**This does NOT imply such a wheel is symmetric.** Symmetry follows from hub geometry alone (below) — absence of a cassette or rotor is not evidence of equal flange spacing.

**Mapping: Side A → `bike-wheel-calc` drive side (DS). Side B → NDS.**

Consequently, **positive lateral displacement is toward Side B.**

**Symmetry is determined by configured hub geometry, not by wheel type.** A front wheel carrying a disc rotor is *frequently* asymmetric — the rotor mount occupies axial space, often placing that flange further inboard, producing unequal bracing angles — but the magnitude varies by hub design and some are near-symmetric. **The `asymmetric` flag MUST follow the configured per-side flange dimensions and be corroborated by measurement (§11.1), never inferred from the presence or absence of a rotor.** Equally, do not assume a front wheel is symmetric merely because it is a front wheel.

### 6.4.2 Physical ground-truth assertions — REQUIRED FOR TESTING

**The sign-convention test MUST assert physical direction derived from mechanics, NOT from the generator's own output.** A test written by inspecting what the generator produces will pass with the sign wrong, locking the error in behind a green check.

The following are derivable from physics and are the required assertions:

1. **Radial:** tightening a spoke shortens it, pulling the rim *toward* the hub at that spoke's location. Under this system's convention (positive radial = outward), a positive tightening adjustment MUST produce a **negative** radial displacement at that spoke's rim angle.

2. **Lateral:** tightening a Side A spoke pulls the rim *toward* Side A. Under this system's convention (positive lateral = toward Side B), that MUST produce a **negative** lateral displacement at that spoke's rim angle.

3. **Tension:** a positive (tightening) adjustment MUST produce a positive tension change in the adjusted spoke itself.

If the generator's output contradicts any of these, **the generator is wrong, not the test.**

### 6.5 Indexing origin — MANDATORY

Which physical spoke is index 0, and what its class is, must be explicitly declared and reconciled.

**`bike-wheel-calc`'s `lace_cross()` convention (documented in its README):** the bottom-most spoke is a **non-drive-side leading** spoke; spokes alternate NDS, DS, NDS, … and leading, leading, trailing, trailing, …

**System requirement:** the machine's spoke-0 must be mapped to the generator's spoke-0, including flange side and lead/trail class. If they are offset, **every column of the influence matrix is shifted**, and the system will confidently adjust the wrong spokes.

This is a setup-time declaration (a small set of config parameters), not a measurement campaign. Record it in the wheel-class configuration and in the influence matrix artifact.

**Required procedure:**

1. **Physically mark spoke 0** on the wheel, referenced to the valve stem — a permanent, unambiguous landmark.
2. **Declare its class** in wheel-class configuration: side (A or B, per §6.4.1) and lead/trail.
3. **The artifact generator applies an angular and class offset** so its spoke 0 corresponds to the machine's spoke 0.

**Required guardrail — column localization test:**

The physical basis: tightening spoke *i* applies its load to the rim at spoke *i*'s nipple, so the largest lateral response should occur near that location. Rim bending distributes the response into a wave, but the extremum should remain local to the perturbation.

**Evidence this holds for the reference geometry:** the reference work's mean lateral influence function, normalized to the 16th spoke of a 32-spoke wheel, reaches its largest excursion at approximately θ = π — which is that spoke's angular position. The extremum coincides with the perturbed spoke.

**The assertion, stated safely:**

```
circular_distance( argmax( |detrend(Φ_u[:, i])| ), θ_i )  ≤  tol_angular
```

Three qualifications, all required:

- **Detrend first.** A DC or low-order offset in the column can dominate `argmax` and mask the local peak. Remove the mean before locating the extremum.
- **Use circular angle distance.** Rim angle wraps at 2π; naive subtraction fails for spokes near the origin.
- **`tol_angular` is configuration, not zero.** At minimum one angular sample spacing (2π / n_rim_angles), since the true extremum generally falls between samples.

**Scope limit — do not treat this as universally guaranteed.** It follows from mechanics and is confirmed for the reference geometry, but it is a *localization* property whose sharpness depends on rim-to-spoke stiffness ratio and angular sampling density. A very stiff rim with sparse sampling could produce a broad, poorly-localized response where `argmax` becomes sensitive to noise.

**If a legitimate configured geometry fails this test, investigate before loosening the tolerance.** The test exists to catch column/index offsets — a genuine offset displaces the extremum by a whole spoke pitch or more, which is far outside any reasonable tolerance and remains detectable even where localization is weak. A marginal failure means the *test* needs tuning for that geometry; a failure by one or more spoke pitches means the *indexing* is wrong.

**Setup verification:** at `INITIALIZE`, the system MUST confirm that spoke 0 is at the machine's **reference station** (a machine-profile fact, §11.6) — by operator confirmation, index sensor, or both — before any measurement cycle begins. This is the Wheel Navigation capability's reference-establishment operation (§10A); the wheel's logical rotation state is anchored from it.

### 6.6 Time and provenance

The ESP32 has **no real-time clock** by default. Consequently:

- Timestamps are **milliseconds since boot** (`uint32`, per-record).
- A `session_id` is generated at session start and lives in the **session header only** (§6.2). Records are associated with their session by containment in that session's log — it is **not** stamped on every record. If records are exported or merged across sessions, the exporter attaches the `session_id` at extraction time.
- If the host supplies wall-clock time at connect, the offset is recorded **once in the session header**, so relative timestamps can be resolved to absolute time later.

Session logs are written host-side. The ESP32 emits events; the host persists them. The ESP32 persists only configuration and model artifacts (see §11.5).

---

## §7 State Machine and Control Flow

### 7.1 Structure — two nested loops

```
BOOT
 ↓
INITIALIZE          (load config, load artifacts, self-test)
 ↓
READY               (await START_TRUING intent)
 ↓
┌─────────────────── OUTER CYCLE LOOP ──────────────────────┐
│                                                            │
│  MEASURE_WHEEL_STATE                                       │
│     ├─ per spoke:  POSITION* → MEASURE_SPOKE_TENSION       │
│     └─ per index:  POSITION* → READ_RUNOUT*                │
│        (* = resolves through WAIT_FOR_OPERATOR in C2)      │
│        MEASURE_SPOKE_TENSION is ONE coarse call (§9.1);    │
│        excite/capture/analyze/estimate are INTERNAL to     │
│        the acoustic subsystem and invisible here.          │
│  ↓                                                         │
│  CHECK_SOLVER_ADMISSION  (§8.11)                           │
│     ├─ admissible          → continue                      │
│     └─ PARTIAL_WHEEL_STATE → REMEASURE_GAPS (bounded)      │
│           ├─ gaps closed   → continue                      │
│           └─ retries spent → ABORT_PARTIAL_STATE           │
│  ↓                                                         │
│  COMPUTE_ADJUSTMENTS   (full vector, all spokes at once)   │
│  ↓                                                         │
│  COMPUTE_TARGETS       (predicted intermediate states)     │
│  ↓                                                         │
│  ┌────────── INNER PER-SPOKE APPLY LOOP ──────────┐        │
│  │  POSITION_TO_SPOKE_i                            │        │
│  │  APPLY_ADJUSTMENT_i                             │        │
│  │     C2: display turns, WAIT_FOR_OPERATOR        │        │
│  │     C3: closed-loop to lateral target (§7.6)    │        │
│  │  i++ until complete                             │        │
│  └─────────────────────────────────────────────────┘        │
│  ↓                                                         │
│  VERIFY                (re-measure, compute scalar cost)   │
│  ↓                                                         │
│  EVALUATE_CONVERGENCE                                      │
│     ├─ fully converged       → CONVERGED                   │
│     ├─ geometric only        → CONVERGED_GEOMETRIC_ONLY    │
│     ├─ cost did not decrease → ABORT_NO_PROGRESS           │
│     ├─ max cycles reached    → ABORT_MAX_CYCLES            │
│     └─ else                  → repeat outer loop           │
└────────────────────────────────────────────────────────────┘
 ↓
CONVERGED / CONVERGED_GEOMETRIC_ONLY / ABORT_*   (§8.6.2, §13.2)
```

**Every `POSITION*` above is a request to the Wheel Navigation / Positioning capability (§10A)** of the form "bring wheel feature X (spoke, rim index, or rim angle) to station S", where S is whichever station needs it next — acoustic, runout, or adjustment — and its placement is machine-profile data (§11.6). The orchestrator issues the request and acts only on its outcome: *done*, *pending operator* (Stage 1 → `WAIT_FOR_OPERATOR`), *in motion* (Stage 2 → poll), *refused*, or *fault*. It never sees drive mechanics, station offsets, or actuator coordinates. That is what lets the Stage 2 automated implementation slot in without touching this diagram.

### 7.2 Why this structure specifically

**Global measure, global compute, per-spoke apply** is the reference method's structure, not an arbitrary choice. Computing the full adjustment vector at once, then applying spoke-by-spoke against predicted intermediate states, is what allows Stage 2's closed-loop feedback to slot in without restructuring.

**A naive design** — measure all, compute all, apply all as a batch, verify — works for Stage 1 and then cannot accommodate per-spoke feedback. That is a subsystem-scale rewrite between stages. Build the nested structure now; it costs nothing extra.

**The Stage 1 → Stage 2 delta is a set of driver swaps, not a change of shape.** The state machine's structure is identical across phases; what changes is the implementation behind three operations:

| Operation | Stage 1 | Stage 2 |
|---|---|---|
| `POSITION` (measurement and apply loops) | **Manual** Wheel Navigation implementation: operator confirmation via `WAIT_FOR_OPERATOR` (§10A) | **Automated** Wheel Navigation implementation: wheel drive behind the same contract (§10A) |
| `READ_RUNOUT` | Manual-entry implementation | Sensor driver |
| `APPLY_ADJUSTMENT_i` | Displayed turns + operator confirmation | Closed-loop actuation (§7.6) |

An earlier revision said "exactly two steps," which predates the measurement-phase operator waits (§7.3) and the manual runout implementation (§10.2). Everything else — cost function, convergence check, abort logic, cycle history, admission rules — is identical and is exercised in the Stage 1 demo.

### 7.3 `WAIT_FOR_OPERATOR` is a first-class state

Operator actions are states, not gaps between states. Each carries:

- a typed prompt (what the operator must do)
- an expected intent (what ends the wait)
- optional timeout behavior (default: wait indefinitely)

**In Stage 1, measurement itself contains operator waits.** The contract table (§2.2) defers wheel rotation and automated runout sensing, so:

| Step | C2 implementation | C3 implementation |
|---|---|---|
| `POSITION` | **Resolves through `WAIT_FOR_OPERATOR`** — the manual navigation implementation (§10A) prompts the operator to rotate the target spoke / rim location to the **station that requires it** (the prompt names both), and waits for confirmation | Automated navigation implementation (§10A); no wait |
| `READ_RUNOUT` | **Resolves through `WAIT_FOR_OPERATOR`** — the orchestrator waits, the typed `SUBMIT_RUNOUT(lateral, radial)` intent supplies the values and resumes the state machine. **The driver does not block on network I/O** (§10.2) | Sensor driver; no wait |
| `EXCITE`, `CAPTURE`, `ANALYZE`, `ESTIMATE` | Autonomous | Autonomous |

**This resolves an apparent contradiction between §7.3 and §12.2.** §12.2's claim that measurement does not depend on the host applies to **acquisition, DSP, estimation, and computation** — the parts the ESP32 performs. Manual positioning and manual runout entry are *operator actions*, and they depend on the host exactly as every other operator action does: through `WAIT_FOR_OPERATOR`. Both statements are true once the boundary is drawn at "computation vs. operator input" rather than "measurement vs. adjustment."

**Consequence for comms:** operator-wait states are the *only* places where the system depends on the host. Because the system is already stopped there, a slow, lost, or post-reconnect command cannot corrupt an in-flight operation. This is what keeps host-dependence from requiring reconnect logic and state reconciliation. See §12.3.

**REQUIRED — wait-instance correlation.** The above is only safe if an intent is bound to the specific wait it answers. Without correlation there is a real defect:

> The system waits for confirmation that spoke 5 was adjusted. The operator confirms; the message is delayed. The operator confirms again. The first arrives, the system advances to spoke 6 and waits. **The second arrives and is accepted as confirmation for spoke 6 — which was never adjusted.**

**Minimal mechanism:**
- Every `WAIT_FOR_OPERATOR` instance is issued a **monotonically increasing `wait_id`**.
- The prompt sent to the input source carries that `wait_id`.
- An intent MUST echo the `wait_id` it answers.
- **An intent whose `wait_id` does not match the currently active wait is silently discarded** and logged as stale.

One counter and one echoed field. This is not reconnect machinery — it introduces no retry, acknowledgement, or session-recovery logic. It closes an ordering hazard that would otherwise be invisible and would manifest as spokes silently skipped.

### 7.3.1 `REMEASURE_GAPS` — resolving a partial state

When §8.11 refuses the `active_row_set`, the orchestrator does **not** terminate immediately. It enters `REMEASURE_GAPS`:

1. Identify which rows the **active row set** (§8.11) lacks relative to a shippable layout.
2. Re-measure **only those** spokes/indices, using the normal retry path (§7.4) — in C2 this means re-prompting the operator for the specific readings.
3. Re-run §8.11 admission.
4. Bounded by `partial_state_remeasure_attempts` (§11.2). On exhaustion, terminate with `ABORT_PARTIAL_STATE`.

**This is the transition the "remedy is to re-measure" language implies.** Without it, a refused row set has no defined control-flow outcome and an implementation would have to invent one.

### 7.4 Retry and partial state

**Retry is owned by the acoustic subsystem, NOT sequenced by the orchestrator.**

> An earlier revision described the retry loop as `EXCITE → CAPTURE → ANALYZE → ESTIMATE`, which reaches directly into the acoustic subsystem's internals and contradicts §9.1's coarse boundary. Both cannot be authoritative; the coarse boundary wins, because the entire point of it is that research changes to excitation, damping, or analysis do not leak into the orchestrator.

**Two valid placements, both preserving the boundary:**

| Placement | Mechanism |
|---|---|
| **Internal (preferred)** | The acoustic subsystem performs bounded re-excitation inside a single `measure_spoke_tension()` call and returns the final outcome |
| **External** | The orchestrator re-invokes the coarse operation, bounded by `measurement_retry_count` — it never invokes sub-stages |

**The retry semantics themselves are unchanged and remain acoustic-subsystem requirements (§9):** most failures — `NO_ONSET_DETECTED`, `LOW_SNR`, `ONSET_COUNT_MISMATCH` — mean the *excitation* produced no usable signal, so re-analysing the same buffer fails identically. **A retry therefore re-excites**, with `excitation_settle_ms` before re-excitation so a still-ringing spoke does not contaminate the capture.

**Cooperative cancellation at the boundary.** If `ABORT` arrives while `measure_spoke_tension()` is active, it is delivered as a **cancellation signal to the acoustic subsystem**, which returns at its next internal safe point. The orchestrator does **not** gain visibility of internal stages merely to implement abort — exposing the internals to support cancellation would defeat the boundary it exists to protect.

Two required details:
- **Settle delay before re-excitation.** The spoke may still be ringing from the previous pluck. Re-exciting into a decaying mode contaminates the capture. Settle time is configuration.
- **Re-excitation has physical cost** (mechanical wear, cycle time). Retry count is bounded and configurable.

**Analysis-only retry is permitted but is not the default.** Re-running `ANALYZE → ESTIMATE` on an existing buffer is only meaningful if analysis *parameters* change (different window, different selection rule). This is a research and debugging affordance, not a control-path behaviour, and must be explicitly requested rather than occurring automatically.

**Status on retry exhaustion — determined by failure semantics, NOT by exhaustion itself.** Exhausting retries is not itself a diagnosis. Map by cause:

| Cause | Status | Example reason codes |
|---|---|---|
| Capability absent or not implemented | `unavailable` | `NOT_IMPLEMENTED`, `SENSOR_TIMEOUT`, `CALIBRATION_MISSING` |
| Signal obtained but of degraded confidence | `suspect` | `AMBIGUOUS_PEAK`, `LOW_SNR`, `PROVISIONAL_MODE_ID` |
| Signal obtained but invalid | `rejected` | `FREQ_OUT_OF_RANGE`, `VALUE_OUT_OF_RANGE`, `MODEL_REJECTED` |
| No usable signal after bounded retries | `rejected` | `NO_ONSET_DETECTED`, `ONSET_COUNT_MISMATCH` |

**This distinction is load-bearing, not cosmetic.** §8.11 admits `suspect` rows into the solve and excludes `rejected` ones. Collapsing everything to `rejected` on exhaustion discards usable evidence and can push an otherwise solvable state below the determinacy rule.

**The cycle continues after an individual measurement failure.** The system must not halt a whole cycle for one bad spoke.

**Partial wheel state is legal — and separately adjudicated.** `n_measured < n_spokes` is a valid state to *record*. Whether it is a valid state to *solve from* is decided by the solver's admission rules (§8.11), not by the measurement phase. Continuing collection after failures does **not** imply the solver will accept whatever remains; it may refuse with `PARTIAL_WHEEL_STATE`.

**It must never be silently zero-filled** — a zero tension reading is a valid-looking number that will corrupt the solve.

### 7.5 Abort conditions

| Condition | Action |
|---|---|
| Cost failed to decrease across consecutive cycles | `ABORT_NO_PROGRESS` |
| Partial state unresolved after bounded re-measurement (§7.3.1) | `ABORT_PARTIAL_STATE` |
| Max cycles reached | `ABORT_MAX_CYCLES` |
| Requested adjustment exceeds safe bounds | `ABORT_UNSAFE_ADJUSTMENT` |
| Influence matrix unavailable or invalid | `ABORT_NO_MODEL` |
| Operator abort intent | `ABORT_OPERATOR` |

**The non-decrease abort is essential, not defensive padding.** When measurement noise approaches correction magnitude, a naive loop oscillates indefinitely. This condition catches it.

### 7.5.1 Convergence and non-decrease — precise semantics

**Literal floating-point comparison of J across cycles is NOT acceptable.** J is computed from noisy measurements — in Stage 1, hand-read dial gauges and provisional tension estimates. Two consecutive cycles can differ by measurement noise alone, producing a false abort on a converging wheel or masking a genuinely stalled one.

**Required structure:**

```
improved(k)  :=  J(k) < J(k−1) · (1 − min_relative_improvement)
                 — only when cycle k and k−1 used the SAME layout (§8.6.1)

converged    :=  per-channel tolerance test of §8.6 — NOT a J threshold
                 (see §8.6.2 for TENSION_ABSENT semantics)

abort_no_progress := improved(k) false for
                     `consecutive_non_improving_cycles` successive cycles
                     within a single layout
```

**J is a stall-detection metric only.** Convergence is decided per-channel against configured tolerances (§8.6). An earlier revision suggested J ≲ 1 as a convergence indicator; that was incorrect — see §8.6. J must be row-count normalized, and **compared only within a single solver layout** — a layout change resets the progress baseline (§8.6.1).

**Configuration (§11.2):** `min_relative_improvement`, `consecutive_non_improving_cycles`.

> **OPEN — values not yet derivable.** The correct threshold depends on the measurement repeatability of J, which is unmeasured. J combines runout (manual dial entry) and tension (provisional mode identification, §4.4.1); neither noise floor is characterized. **Derive these values from measured cycle-to-cycle repeatability on a stationary wheel before relying on the abort.** Until then, set `consecutive_non_improving_cycles ≥ 2` so a single noisy cycle cannot abort a converging run.
>
> Do not substitute a guessed threshold and treat it as validated.

### 7.6 Stage 2 closed-loop apply — target contract

**Deferred to Stage 2.** Recorded here so the state machine does not imply its own interpretation.

**What is specified now** (derived from the reference method, §8):

- **The target** is the *predicted* lateral displacement at the rim location of the spoke being adjusted, taken from the intermediate state prediction sequence computed in `COMPUTE_TARGETS`. It is not a fixed nipple rotation.
- **Measured where:** at the rim, at that spoke's angular position. **This couples actuation to wheel navigation** — the Wheel Navigation capability (§10A) must bring the spoke under adjustment to a location the lateral sensor can read while the nipple actuator can reach it. Whether the runout and adjustment stations coincide is a machine-profile fact (§11.6), not an assumption of this loop.
- **Why lateral rather than tension:** lateral displacement has the highest sensitivity to spoke tension change and is the reference method's designated feedback parameter (§8.3).
- **A lateral deadband is required configuration**, distinct from `adjustment_deadband`. The latter is in revolutions and governs whether an adjustment is worth performing at all; the deadband here is in mm and governs when the closed loop stops driving.

> **OPEN — not derivable from current project information:**
> - The deadband magnitude. Depends on the achievable resolution and update rate of a runout sensor that is not yet selected (§10.4).
> - Behaviour when a target cannot be reached — nipple seized, adjustment range exhausted, oscillation around the target. The reference work does not address unreachable targets.
>
> Both must be resolved with the actuation hardware, not designed around beforehand.

---

## §8 Truing Subsystem

**This is the highest-risk subsystem in the project, for a specific reason: it is the only one that fails silently.** Acoustic failures are loud (no signal, no peak, out of range). Sensor failures are loud (timeout, impossible value). A wrong influence matrix or a wrong solve produces confident, plausible, well-formatted numbers that are wrong, and nothing in normal operation catches it.

It therefore gets a heavier verification regime (§14.3) and the most explicit specification.

### 8.1 The wheel model

The influence matrix models the effect of unit spoke adjustments on wheel parameters. For spoke *s*, its influence functions on lateral, radial, and tension parameters, evaluated at rim angles θ₁…θₙ, form column vectors **u**ₛ, **v**ₛ, **t**ₛ.

```
Φ_u = [u₁ u₂ … u_ns]        (n_angles × n_spokes)
Φ_v = [v₁ v₂ … v_ns]        (n_angles × n_spokes)
Φ_t = [t₁ t₂ … t_ns]        (n_spokes × n_spokes)

Φ = ⎡Φ_u⎤
    ⎢Φ_v⎥
    ⎣Φ_t⎦
```

Given an adjustment vector **d** (spoke nipple rotations), the predicted wheel state is:

```
Ŷ = Y_b + Φd                                              (Eq. 2)
```

where Y_b is the state prior to adjustment.

**Linearity is a MODEL ASSUMPTION, not a guarantee.** The reference work asserts the wheel is a linear structure absent plastic deformation, and the superposition of per-spoke contributions follows from that. However, several nonlinearities exist within the elastic regime and MUST NOT be assumed away:

- **Nipple-thread friction and spoke torsional windup** produce discrete jumps in effective adjustment, worsening at higher tension. The reference documents this explicitly and introduces closed-loop feedback specifically to compensate for it.
- **Tension-dependent stiffness** — spoke tension alters both rim and spoke-system lateral stiffness. `bike-wheel-calc` exposes this as an explicit modelling option, which is evidence it is not negligible.
- **Spoke slack is a hard nonlinearity.** A spoke at or near zero tension cannot carry compression. Superposition fails outright below that threshold.

**Requirement:** linearity must be treated as an assumption to be validated empirically, not a property guaranteed by construction. The synthetic round-trip test (§14.3) validates the solver *given* linearity; it does not validate linearity itself. Prediction-versus-measurement comparison on a physical wheel is the only test that does, and it is a Stage 2 activity.

**Practical consequence for Stage 1:** the outer cycle loop and its non-decrease abort (§7.5) are the operational safeguard against linearity breaking down. If the model mispredicts, cost fails to decrease and the system stops rather than compounding error.

### 8.2 The weighted least-squares solve

Weights μ_v and μ_t scale the radial and tension channels relative to lateral:

```
Φ̃ = ⎡  Φ_u      ⎤        Ỹ = ⎡  u − u₀        ⎤
    ⎢ Φ_v·√μ_v  ⎥            ⎢ (v − v₀)·√μ_v  ⎥         (Eq. 3, REFERENCE FORM)
    ⎣ Φ_t·√μ_t  ⎦            ⎣ (T − T̄)·√μ_t   ⎦

d_ls = Φ̃† · Ỹ                                             (Eq. 4)
```

> **⚠ Eq. 3 is reproduced for traceability to the reference work. DO NOT IMPLEMENT IT IN THIS FORM.**
> Its dimensional bookkeeping is ambiguous (§8.3.1). Implement the tolerance-normalized dimensionless formulation of §8.3.1 instead. The structure of the solve and Eq. 4 are unchanged — only the construction of Φ̃ and Ỹ differs.

where Φ̃† is the pseudo-inverse. u₀ and v₀ are typically zero (gauge zeroed at the desired location during setup).

**The reference one-part correction is `d_adj = −d_ls`.**

> **⚠ THAT IS NOT THIS PROJECT'S RUNTIME ALGORITHM.** It is shown here only as the reference intuition, because §8.2 precedes §8.4 and an implementation agent could otherwise implement this line literally.
>
> **Project implementation MUST use the two-part solve of §8.4:** recover `d_ls`, remove the `n_mt` component for shape truing, construct `d_cm` independently, then combine. `d_adj = −d_ls` is valid only as a description of the one-part reference method, which §8.4 documents as producing tension overshoot and worse non-uniformity when used for tension targeting.

### 8.3 Weighting — CRITICAL, DO NOT "CORRECT"

**Runout is the primary channel. Tension is secondary and heavily de-weighted.**

An agent implementing a project named after acoustic tension sensing will naturally assume tension should dominate. **It must not.** The reference method de-weights tension by roughly five orders of magnitude relative to lateral, for two reasons:

1. **Unit scaling** — the unit disturbance magnitudes differ (≈0.6 mm lateral vs. ≈250 N tension).
2. **Trust** — the tension influence functions are noisy and imprecise relative to displacement.

#### 8.3.1 Dimensionless formulation — USE THIS

The reference reports μ_t = 10⁻⁵ with units given as (mm/N). This is dimensionally ambiguous: for √μ_t·ΔT to share units with the lateral rows (mm), μ_t must carry (mm/N)². **Do not attempt to reproduce the reference's dimensional bookkeeping.** Use the following formulation instead, which is unambiguous and matches the reference's stated derivation intent (normalizing each parameter's disturbance magnitude against its specification tolerance).

**Step 1 — normalize each channel by its tolerance, producing dimensionless residuals:**

```
û = (u − u₀) / tol_lateral                    [dimensionless]
v̂ = (v − v₀) / tol_radial                     [dimensionless]
T̂ = (T − s·T_norm) / tol_tension              [dimensionless]  — see below
```

**The tension residual measures deviation from the correctly-balanced operating DISTRIBUTION, not from a scalar mean.**

> An earlier revision wrote `T̂ = (T − T̄)/tol_tension`. That is the reference/symmetric form: it drives every spoke toward one scalar tension, which on an asymmetric wheel is the wrong target and fights the lateral balance the geometry requires. The `n_mt` machinery was made asymmetric-aware while this residual was not.

**Definition, fully specified so nothing is left to invent:**

```
T_norm  = artifact's T_target_assumed, normalized to mean 1.0   (§11.2)

s       = current overall tension SCALE, estimated by least squares:
              s = (T · T_norm) / (T_norm · T_norm)

T̂_i     = ( T_i − s · T_norm_i ) / tol_tension
```

Three things this fixes, in order: **how the current scale is estimated** (LS projection of the measured tensions onto the balanced-distribution direction), **how that scale combines with `T_norm`** (elementwise product), and **what vector is subtracted from `T`** (`s·T_norm`, not `T̄·[1,…,1]`).

**Symmetric reduction.** When `T_norm = [1,…,1]`, `s = (T·1)/(1·1) = mean(T) = T̄`, so `T̂ = (T − T̄)/tol_tension` — the reference form emerges as the special case, exactly as it should.

**Note `s` is the *current* scale, not `target_tension`.** This residual drives the wheel toward *uniform balance* at whatever tension it currently sits; moving the overall level to `target_tension` is Part 2's job (§8.4), and mixing the two is the documented failure the two-part solve exists to avoid.

**Step 2 — apply dimensionless trust weights:**

```
w_u = 1.0                    (reference channel, by definition)
w_v = trust_radial           [dimensionless]
w_t = trust_tension          [dimensionless]
```

Influence matrix rows are scaled by the same tolerance factors so Φ̃ and Ỹ remain consistent.

**Step 3 — apply weights as √w to BOTH the matrix rows and the residuals.** This is not cosmetic:

```
Φ̃ = [ Φ_u_norm ; Φ_v_norm·√w_v ; Φ_t_norm·√w_t ]
Ỹ = [ û        ; v̂·√w_v        ; T̂·√w_t        ]
```

Because `‖√w · r‖² = w·‖r‖²`, **√w is what realizes the intended cost** `‖û‖² + w_v‖v̂‖² + w_t‖T̂‖²` (§8.6). **Multiplying by `w` instead of `√w` yields an effective weight of `w²`** — silently mis-weighting every solve, by five orders of magnitude on the tension channel. Any implementation or summary that applies `w` directly is wrong.

**Consequences of this formulation:**

- All weights are **dimensionless**, so no unit reasoning is required at any call site.
- Unit scaling is handled by the tolerance normalization, not folded into a weight.
- **The trust weights carry only trust**, which is the quantity this project must re-derive for its own sensing chain.
- The reference's numeric values (μ_v = 0.5, μ_t = 10⁻⁵) are **NOT directly transferable** into this formulation. They are recorded as a sanity reference for relative magnitude only.

#### 8.3.2 Reference values versus project values

| | Reference work | This project |
|---|---|---|
| Tolerances | ±0.1 mm lat, ±0.05 mm rad, 1000±100 N | From this project's acceptance criteria |
| Weight form | Dimensional (μ_v, μ_t) | Dimensionless trust weights |
| μ_v | 0.5 | Re-derive |
| μ_t | 10⁻⁵ (mm/N) | Re-derive |
| Tension sensing | Mechanical gauge, ~10% claimed accuracy | Acoustic, precision TBD |

**`trust_tension` must be re-derived for this project, not copied.** The reference's de-weighting reflects *its* tension instrument. Whether this project's acoustic tension estimate warrants more or less trust is an empirical question and one of the project's actual research contributions. Until measured, start conservative — matching the reference's relative magnitude — and record the value used in the artifact.

### 8.4 Two-part solve — MANDATORY SEPARATION

**The shape-truing solve and the mean-tension shift MUST be separate operations.** Do not combine them into one pseudo-inverse.

**This is backed by a documented negative result in the reference work:** attempting to target a new mean tension via the combined solve (setting T̄ = T_d in Eq. 3) overshot the target by 126 N and made tension non-uniformity **50% worse** (4.2% → 6.8%).

**Part 1 — shape truing (projection out of the common-mode direction):**

```
d = −( d_ls − proj_{n_mt}(d_ls) )
```

where `n_mt` is the **mean-tension direction**: the spoke-adjustment direction that changes mean tension without changing rim displacement.

**Why d_ls has a mean component at all.** The mean-tension direction is invisible to the displacement blocks but **visible to Φ_t** — it changes tension. So the weighted solve does assign a component along it, driven by the *tension* residual, which is the least trustworthy channel (§8.3). Part 1 strips that untrustworthy component out; Part 2 restores a mean set from the independently measured `c`. That is the whole rationale for the separation.

**For a symmetric wheel, n_mt = [1, 1, …, 1] / √n_spokes**, and the projection reduces to subtracting the arithmetic mean — the reference method's form.

> **⚠ OPEN ISSUE — n_mt IS NOT SIMPLY THE DISPLACEMENT NULL SPACE**
>
> **Two earlier revisions of this document were wrong here, in different directions:**
> 1. The first claimed shape truing is unaffected by asymmetry. It is not — the arithmetic mean projects out `[1,…,1]`, which is the mean-tension direction *only* under the symmetry property §8.4 Part 2 already identifies as failing on asymmetric wheels.
> 2. The correction then over-reached, characterizing `n_mt` as spanning the null space of `[Φ_u ; Φ_v]`. **That is necessary but not sufficient**, and projecting out an entire null-space basis would be actively wrong.
>
> **The null space contains more than the mean-tension direction.** Every adjustment direction invisible to displacement lies in it — the reference attributes the rank deficiency partly to two rim degrees of freedom absent from the model (tangential and torsional), which are displacement-invisible but are **not** mean-tension shifts. If `dim(null([Φ_u ; Φ_v])) > 1`, projecting out the whole basis would strip legitimate displacement-invisible correction components along with the mean.
>
> **Both conditions are required to identify `n_mt`:**
>
> ```
> (i)  displacement invariance:  [Φ_u ; Φ_v] · n_mt  =  0
> (ii) tension response preserves the operating tension distribution
> ```
>
> Condition (i) alone locates a *subspace*. Condition (ii) selects the direction within it, and requires the **tension block** — geometry alone does not determine it.
>
> **⚠ CONDITION (ii) IS NOT "EQUAL TENSION CHANGE IN EVERY SPOKE."** An earlier revision of this document wrote condition (ii) as `Φ_t · n_mt ∝ [1,1,…,1]`. **That form is itself a symmetry assumption, and on an asymmetric wheel it directly contradicts condition (i):**
>
> Lateral balance on a wheel requires the sides' lateral force contributions to match — `T_A · sin(α_A) = T_B · sin(α_B)`, where α is the bracing angle. Adding the *same* tension increment ΔT to both sides gives `(T_A + ΔT)·sin(α_A)` and `(T_B + ΔT)·sin(α_B)`, which are no longer equal unless `sin(α_A) = sin(α_B)` — i.e. unless the wheel is symmetric. **So on an asymmetric wheel an equal-increment direction changes lateral displacement, violating condition (i).** No direction can satisfy both conditions in that form; they are mutually inconsistent.
>
> **The generalized form:** condition (ii) requires the tension response to be a *scaling of the operating tension distribution* — moving along the family of correctly-balanced tension states rather than adding a constant:
>
> ```
> Φ_t · n_mt  ∝  T_target      (the operating per-spoke tension distribution)
> ```
>
> For a **symmetric** wheel every spoke operates at the same tension, `T_target ∝ [1,…,1]`, and this reduces to the reference form — which is why the reference's version is correct for its wheel and only for its wheel.
>
> **What is NOT established for asymmetric geometry:**
> - the **side-to-side target tension ratio** for this project's wheel. It follows from bracing-angle geometry in principle and `bike-wheel-calc` can compute it, but it has **not been measured or established** by current project information;
> - whether a direction satisfying both (i) and generalized (ii) **exists** within tolerance;
> - whether it is **unique** up to scale;
> - whether the shape/mean decomposition retains its meaning if it is not.
>
> **These cannot be resolved from current project information and MUST NOT be assumed.** Do not substitute an invented side ratio.
>
> **⚠ OPEN MODEL-PREPARATION ITEM — THE IDENTIFICATION PROCEDURE IS NOT YET SPECIFIED.** The conditions above define what a valid `n_mt` must satisfy, but current project information does not justify a numerical procedure for generating candidate directions inside `null([Φ_u ; Φ_v])`, selecting among them using condition (ii), defining the "next candidate," or computing the uniqueness margin. The Phase 1b implementation MUST NOT invent that procedure.
>
> The existing thresholds, normalization/sign convention, provenance fields, and validation conditions remain authoritative. Until a justified identification procedure is documented, model preparation MAY emit `common_mode_direction.identified = false`. It MUST set `identified = true` only when an exact, justified procedure has been added to this specification and its recorded residuals and uniqueness margin pass the recorded thresholds.
>
> **Conservative runtime behaviour — required:**
> - The generator records whether `n_mt` was **identified**, the condition-(i) and condition-(ii) residuals and thresholds where evaluated, the uniqueness margin and threshold where evaluated, the assumed `T_target` distribution, the normalization/sign convention, and `dim(null([Φ_u ; Φ_v]))`. When no justified identification procedure is available, it records `identified=false` rather than choosing one.
> - **If a unique `n_mt` is not identified**, the solver MUST refuse mean-tension targeting (Part 2) with the **non-terminal** reason code `MEAN_TENSION_MODEL_UNAVAILABLE`.
>
>   **Not `ABORT_NO_MODEL`** — that is a *terminal session result* (§13.2), and an earlier revision used it here while simultaneously defining a geometry-only fallback. A run cannot terminate and continue at the same time. `ABORT_NO_MODEL` is reserved for cases that genuinely end the run, such as no loadable artifact at all.
> - **Shape-only fallback — mathematically defined, not left to the implementation.** With `n_mt` unknown, `proj_{n_mt}` is undefined, so Part 1 as written cannot execute. The correct fallback is to **solve under the `TENSION_ABSENT` layout (§8.7.1) and apply NO projection at all.**
>
>   *Why this is correct rather than merely convenient:* with the tension block removed, Φ̃ contains only displacement rows. The pseudoinverse yields the **minimum-norm** least-squares solution, which lies in the row space and is therefore orthogonal to the null space of the matrix. Since `n_mt ∈ null([Φ_u ; Φ_v])` by condition (i), the solution has **no component along `n_mt` by construction** — there is nothing to project out. The mean-tension component was only ever introduced by the tension block, which is precisely what this layout removes.
>
>   **Selecting `TENSION_ABSENT` here is a POLICY decision, not a measurement failure.** §8.11 distinguishes *available* measurements from the *active solver row set*: the solver may deliberately exclude the whole tension channel even when every tension measurement succeeded. Without that distinction this fallback would be unreachable — all tension rows present maps to `FULL`, and `TENSION_ABSENT` could never match.
>
>   The result carries `MEAN_TENSION_MODEL_UNAVAILABLE` and `CONVERGED_GEOMETRIC_ONLY` semantics (§8.6.2), with reduced confidence on the `AdjustmentPlan`.
> - **Never fall back to the arithmetic mean on a wheel configured `asymmetric: true`.** Refusing is correct; guessing is not.
> - For `asymmetric: false`, the artifact's `n_mt` must match `[1,…,1]/√n_spokes` within tolerance, or the generator has a bug — treat a mismatch as `ARTIFACT_INVALID`.

**Part 2 — mean tension shift:**

**`d_cm` is a VECTOR over spokes, not a scalar.** Scalar treatment is valid only for symmetric wheels and is not valid for a wheel configured `asymmetric: true` (§6.4.1).

**Symmetric case (both sides equal bracing angle):**

```
d_cm[i] = (target_tension − mean(T)) / c          for all spokes i
```

Every spoke receives the same rotation. This works because tightening every spoke of a symmetric wheel equally changes only the mean tension, leaving lateral and radial displacements unaffected.

**Asymmetric case (differing bracing angles — SEE OPEN ISSUE BELOW):**

```
d_cm[i] = f( ΔT_side(i), side(i), c_side_a, c_side_b )
```

A uniform rotation on an asymmetric wheel does **not** leave displacement unaffected — lateral force balance depends on bracing angle, which differs per side. Compensation requires different rotations per side.

**A single EQUAL-PER-SPOKE target is a symmetric-case construct; a single overall SCALE is not.** On an asymmetric wheel the two sides operate at different tensions by design, so one tension value repeated across every spoke is meaningless. But one **overall mean-tension scale** is perfectly well defined, and that is what this project configures (§11.2):

```
target[i] = target_tension × T_target_assumed_normalized[i]      (mean of T_norm = 1.0)
```

**Per-side absolute targets are DERIVED from the artifact's normalized distribution — they are NOT independently configurable.** An earlier revision here called for separate `T_d_side_a` / `T_d_side_b` configuration fields; that contradicts §11.2, which deliberately does not expose them, because independently settable side targets would let runtime change the ratio `n_mt` was identified against (§8.4 Part 1).

**What remains unresolved is the compensation mapping, not the target representation.** The target side of this is settled: one session-fixed scale × one artifact-bound normalized distribution. What is open is `d_cm` — how a requested change in overall scale maps to per-side nipple rotations on an asymmetric wheel — and the side ratio itself, which must come from validated geometry (§16).

> **⚠ OPEN ISSUE — CANNOT BE RESOLVED FROM CURRENT PROJECT INFORMATION**
>
> The reference work states the asymmetric extension is possible "by compensating for the different spoke angles" but **never derives it**. No validated per-side formulation exists in the source material.
>
> **This project's primary donor wheel is *expected* to be asymmetric** (front wheel with a disc rotor), but per §6.4.1 that must follow from **configured and measured hub geometry**, not from the presence of a rotor. Until measured, treat it as `expected asymmetric, unverified` — not established.
>
> **Required approach:**
> - Implement `d_cm` as a vector-valued quantity from the outset. The symmetric case is the special case where all entries happen to be equal.
> - Implement and validate the symmetric path first.
> - Derive the asymmetric compensation from bracing-angle geometry, or determine it empirically (apply a known per-side rotation, measure the resulting per-side mean tension change and residual displacement).
> - **Validate** the asymmetric path before making any Stage 2 accuracy claim. Do not ship an inferred formula unvalidated.
> - Until validated, an asymmetric wheel with a requested mean-tension change MUST refuse rather than applying the symmetric formula silently.
>
> Shape truing (Part 1) is **also affected by asymmetry** — see the open issue under Part 1. An earlier revision claimed otherwise; that claim was wrong. The two issues are related but distinct: Part 1 needs the correct *projection direction*, Part 2 needs the correct *per-side compensation*, and both depend on the same unestablished side-to-side tension relationship.

**Combined:**

```
d_adj[i] = d[i] + d_cm[i]                                 (Eq. 6)
```

**Element-wise over spokes.** `d_cm` is **not** a scalar broadcast across the vector — that collapse is valid only in the symmetric special case where every component is equal. Implement it as a per-spoke vector unconditionally, or the asymmetric path cannot be added later without reworking every call site.

**The `c` constant:**
- Configured **per side** (`c_side_a`, `c_side_b`) per §6.4.1. For a symmetric wheel both entries are equal and the scalar form applies.
- A **per-wheel-class configuration value**, measured empirically as a **response slope**: `c_side = Δ(mean tension on that side) / Δ(rotation)`, using a known, bounded, safe adjustment. For asymmetric wheels, measure each side separately.

  > **A full revolution is not architecturally required** and may be an unnecessarily aggressive physical instruction on an already-tensioned wheel. The reference derived its value that way; any bounded known rotation that produces a measurable response is equally valid, and a smaller increment is safer.
- Reference value for the reference wheel: 473 N/rev. **That wheel's value, not a universal constant.** Do not hardcode it.

### 8.5 Rank structure — per layout, not universal

**An earlier revision of this document stated that "the influence matrix is structurally rank-deficient." That is too broad and is corrected here.** Rank structure differs by layout, and conflating them produces both wrong numerical-method choices and a wrong validation criterion.

**The displacement block `[Φ_u ; Φ_v]` has a structural null space.** The mean-tension direction `n_mt` lies in it by definition (condition (i), §8.4 Part 1), and it may contain further displacement-invisible adjustment directions — the reference attributes part of the deficiency to two rim degrees of freedom absent from the model (tangential and torsional).

**The FULL matrix `[Φ_u ; Φ_v ; Φ_t]` is NOT automatically rank-deficient for that reason.** `n_mt` satisfies `[Φ_u ; Φ_v]·n_mt = 0` but `Φ_t·n_mt ≠ 0` (condition (ii)) — so `Φ·n_mt ≠ 0`, and `n_mt` is **not** in the null space of FULL. The tension rows resolve precisely the direction the displacement rows cannot see.

| Layout | Rank expectation | How it is determined |
|---|---|---|
| `TENSION_ABSENT` | **Structurally rank-deficient** — contains `n_mt` and any other displacement-invisible directions | Expected by construction; verify dimension |
| `FULL` | **Not assumed deficient.** May be full column rank if `Φ_t` resolves those directions | **Measure** effective rank and conditioning from the generated artifact — do not assume either way |

**This is a model-preparation finding, not an architectural assumption.** If a particular wheel geometry yields a rank-deficient or ill-conditioned FULL layout, that is recorded in the artifact and surfaced (§8.11 R3). It must not be encoded as a universal property of the solver.

**A second, independent rank limit: Fourier truncation.** The displacement blocks are generated from Fourier series of order `N_lat` and `N_rad`, so their rank is bounded by the available basis vectors (`2·N + 1` per channel), independent of row count. **This couples to §8.9:** reducing `N_rad` to stabilize a noisy radial fit also reduces the displacement blocks' rank, and can push `TENSION_ABSENT` from usable into unusable. Model prep MUST re-verify `TENSION_ABSENT` rank and conditioning after any change to `N_lat` or `N_rad`.

**Consequence, and a required capability:** because the mean-tension direction is invisible to displacement, shape truing and tension targeting are separable. **The system MUST support a degraded mode where the tension channel is unavailable and geometric (shape) truing still executes** — a legal, supported operating mode with an explicit status (§8.6.2), not an error path.

This matters practically: if the acoustic subsystem is unavailable or failing, the machine still does useful work.

### 8.6 The cost function

Required for the non-decrease abort (§7.5.1). It is the weighted residual of the *current* state, using the dimensionless normalization of §8.3.1.

**Normalized by active-row count:**

```
J(L) = ‖r̃_active(L)‖² / n_active_rows

FULL:
    J = ( ‖û‖² + w_v·‖v̂‖² + w_t·‖T̂‖² ) / n_active_rows

TENSION_ABSENT:
    J = ( ‖û‖² + w_v·‖v̂‖² ) / n_active_rows
```

`r̃_active(L)` is the weighted, dimensionless residual vector containing exactly the rows selected by the active layout `L`. **A channel excluded from `active_row_set` contributes neither rows nor residual energy to J.**

**Two corrections from an earlier revision of this document, both material:**

**1. `J ≈ 1` does NOT mean "the wheel is at tolerance."** A raw sum of squared normalized residuals scales with observation count — if every residual sat exactly at tolerance, the sum would equal the row count, not 1. That earlier claim was wrong. Row-count normalization makes J a *mean* squared normalized residual, which restores rough interpretability, but see below for what actually governs convergence.

**2. Normalization is not cosmetic once partial state is possible.** If the active-row count differs between cycles, an unnormalized J changes for that reason alone — the non-decrease abort would then react to *which rows were in the problem* rather than to whether the wheel improved.

**Use `n_active_rows`, not `n_valid_rows`.** Only rows in the **active row set** (§8.11) contribute to the residual. Counting excluded-by-policy rows would divide the sum by a denominator larger than the numerator's row count — for example a `TENSION_ABSENT` solve whose `n_spokes` tension measurements succeeded but were deliberately excluded.

**Convergence is NOT decided by J.** The authoritative test is **per-channel, against configured tolerances**, with every quantity defined explicitly:

```
geometric_converged  :=  max|u − u₀|  ≤ tol_lateral
                     AND max|v − v₀|  ≤ tol_radial

per side S ∈ {A, B}:
    non_uniformity(S) := stdev( T_i : i ∈ S ) / mean( T_i : i ∈ S )    [CV, dimensionless]
    mean_error(S)     := | mean( T_i : i ∈ S ) − target(S) |           [N]

tension_compliant    :=  for every side S:
                             non_uniformity(S) ≤ tol_tension_cv
                         AND mean_error(S)     ≤ tol_mean_tension_error
                         AND every contributing tension measurement
                             is VERIFICATION-GRADE (below)

fully_converged      :=  geometric_converged AND tension_compliant
```

**Non-uniformity is the per-side coefficient of variation.** An earlier revision said "non-uniformity ≤ `tol_tension`" without defining the statistic, while `tol_tension` is in newtons — dimensionally incompatible with a CV. Two distinct configuration values are therefore required (§11.2):

| Value | Units | Role |
|---|---|---|
| `tol_tension` | N | **Normalization scale** for the cost function (§8.3.1) |
| `tol_tension_cv` | dimensionless | **Acceptance** criterion for per-side non-uniformity |
| `tol_mean_tension_error` | N | **Acceptance** criterion for hitting the requested mean |

**Mean-target error is part of full convergence.** An earlier revision checked geometry and non-uniformity but never whether the requested `target_tension` was actually reached — a wheel could be perfectly uniform at entirely the wrong tension and report converged.

#### 8.6.3 Verification-grade data — prevents the central overclaim

> **A `suspect` measurement may be admissible to the SOLVER and still not be evidence of COMPLIANCE.** These are different bars, and conflating them would let Stage 1 announce `CONVERGED` on the strength of tension estimates the same document calls scientifically provisional.

| Bar | Requirement | Governed by |
|---|---|---|
| **Solver-admissible** | `valid` **or** `suspect` | §8.11 |
| **Verification-grade** | `valid` only | this section |

**`tension_compliant` requires verification-grade tension data.** If any contributing tension measurement is `suspect`, `tension_compliant` is **not satisfied** — it is *unevaluated*, and the run terminates at `CONVERGED_GEOMETRIC_ONLY` with `TENSION_NOT_VERIFICATION_GRADE`.

**Direct consequence for Stage 1:** while layer-3 mode identification is provisional, every `TensionEstimate` carries `status: suspect` with `PROVISIONAL_MODE_ID` (§4.4.1). Therefore **Stage 1 cannot reach `CONVERGED`** — its best terminal result is `CONVERGED_GEOMETRIC_ONLY`. That is the correct and honest outcome, and it is exactly what §2.2 states the phase does and does not demonstrate. An implementation that reports `CONVERGED` in Stage 1 has a bug.

This matches how the project's acceptance criteria are actually stated, and unlike a mean it cannot be satisfied while one location sits badly out of spec. **J is a scalar progress metric used solely for stall detection** (§7.5.1).

#### 8.6.1 J is only comparable within a single layout

**`FULL` and `TENSION_ABSENT` optimize different objectives.** One includes the tension block; the other does not. Their J values are computed over different residual sets and are **not comparable as the same metric**, even after row-count normalization — normalization fixes scaling within an objective, not comparison across objectives.

**Required behaviour:**

- **J is compared only against prior cycles that used the same layout.**
- **When the active layout changes, the progress baseline resets.** The first cycle under a new layout establishes a new baseline; `improved(k)` is undefined for it and MUST NOT count toward `consecutive_non_improving_cycles`.
- **The same reset applies to any other change that alters the objective.** Note that `target_tension` cannot trigger this in Stage 1 — it is session-fixed (§12.3.1), precisely because resetting J would not address the stale `AdjustmentPlan` that a mid-session retarget would strand.
- A layout change is recorded in the cycle history so a stall analysis after the fact can see where the objective changed.

**Consequence:** after a layout change, at least two cycles under the new layout are needed before `ABORT_NO_PROGRESS` can trigger. That is correct — the alternative is aborting on an artifact of the objective changing rather than on a genuine stall.

#### 8.6.2 Convergence in TENSION_ABSENT mode

**`TENSION_ABSENT` means tension rows are excluded from the ACTIVE SOLVE** (§8.11) — the underlying measurements may be unavailable, rejected, **or deliberately excluded by policy** while perfectly usable. In every case `tension_compliant` **cannot be established from that solve** — it is unknown, not satisfied.

**Terminal results are distinct and MUST NOT be conflated:**

| Result | Condition | Meaning |
|---|---|---|
| `CONVERGED` | `fully_converged` | Geometry and tension both verified in spec |
| `CONVERGED_GEOMETRIC_ONLY` | `geometric_converged` **and** tension compliance **not established with verification-grade evidence** | Rim is true; **tension compliance unverified** |

**General definition — broader than "tension was never measured."** An earlier revision defined this result as tension having never been evaluated. That is now too narrow: a `FULL` solve may use tension measurements and numerically evaluate them, yet still be unable to *establish compliance* because those measurements are `suspect` (§8.6.3).

**Qualifying reasons, all producing `CONVERGED_GEOMETRIC_ONLY`:**

| Reason code | Situation |
|---|---|
| `NOT_IMPLEMENTED` / `SENSOR_TIMEOUT` | Tension channel unavailable |
| `MEAN_TENSION_MODEL_UNAVAILABLE` | `n_mt` unidentified → geometry-only fallback (§8.4) |
| `TENSION_NOT_VERIFICATION_GRADE` | Measurements existed and were used, but are `suspect` |
| policy | Explicit geometry-only operation |

**Do not imply tension measurements were absent or unevaluated** — say that compliance was not *established*.

**`CONVERGED_GEOMETRIC_ONLY` MUST NOT be reported, logged, or displayed as successful truing.** The project's acceptance criteria include per-side tension non-uniformity; a run that could not establish it cannot claim it. The result carries its reason, and the UI must surface the distinction rather than showing a generic success state.

This is the terminal-state counterpart to `PROVISIONAL_MODE_ID` (§4.4.1): the system reports what it actually verified, never what it merely did not disprove.

**Use the same tolerances and weights as the solve.** A cost function weighted differently from the optimizer measures something the optimizer is not minimizing.

### 8.7 Host/firmware split — IMPORTANT

**Φ̃† depends only on Φ and the weights μ. Neither is a runtime input.** Φ comes from the influence matrix artifact; μ comes from configuration.

**Therefore the pseudo-inverse is computed offline, host-side, during model preparation, and shipped as part of the artifact.**

Consequences:

- **The ESP32 never performs SVD or matrix inversion.** The per-cycle solve is a matrix-vector multiply — roughly 20 lines, no linear algebra library, no numerical stability risk on-device.
- **The firmware truing code is L1–L2 complexity.** The hard, silent-failure-prone work lives permanently in host Python where numpy and pytest make it tractable.
- **Weights become model-prep-time parameters, not runtime knobs.** Changing μ means regenerating the artifact host-side. This is correct — μ are tuning constants derived from specification tolerances, not per-session inputs.
- Φ itself is retained on-device for the prediction step (Eq. 2), which is again matrix-vector multiply.

**Dimensions — derive per layout, never hardcode.** Both `n_spokes` and `n_rim_angles` are configuration (§11.2).

```
n_full_rows   = 2·n_rim_angles + n_spokes     (lateral + radial + tension blocks)
Φ  :  n_full_rows × n_spokes                  (the FULL influence matrix)

per layout L:
    n_layout_rows(L) = popcount(row_mask(L))
    Φ†(L) :  n_spokes × n_layout_rows(L)
```

**`n_full_rows` describes Φ only.** Each layout's pseudoinverse has its **own** row count: `FULL` uses all `n_full_rows`; `TENSION_ABSENT` uses `2·n_rim_angles`. An earlier revision stated one generic dimension expression as if it applied to every pseudoinverse — checking `TENSION_ABSENT`'s Φ† against `n_full_rows` would reject a correct artifact.

**Memory, worked for both relevant configurations (float32):**

| Configuration | n_rows | Φ | Φ† | Total |
|---|---|---|---|---|
| n_spokes=32, n_rim_angles=32 (C2 rule) | 96 | 12.3 KB | 12.3 KB | ≈25 KB |
| n_spokes=36, n_rim_angles=36 (C2 rule) | 108 | 15.2 KB | 15.2 KB | ≈30.4 KB |

Multiply by the number of shipped layouts (below).

#### 8.7.1 Precomputation versus partial state — RESOLVED CONSTRAINT

> **The pseudoinverse is not row-separable.** Removing rows from Φ̃ changes the least-squares problem, and the pseudoinverse of the reduced matrix is **not** a submatrix of the full Φ̃†. Applying the shipped full-matrix Φ̃† to a reduced observation vector produces a well-formed, plausible, **wrong** solution — the exact silent-failure class this document exists to prevent.

**NON-NEGOTIABLE: the firmware MUST NOT apply a pseudoinverse to any row set other than the one it was computed for.** Every shipped Φ† is tagged with its layout; the solver verifies the **active row set** (§8.11) matches `row_mask(L)` before use, and refuses otherwise.

**Stage 1 resolution — a finite set of precomputed admissible layouts.**

The artifact ships one Φ† per layout:

| Layout | Rows included | Purpose |
|---|---|---|
| `FULL` | all lateral + radial + tension | Nominal case |
| `TENSION_ABSENT` | lateral + radial only | Degraded mode (§8.5); required for §8.11 R4 |

**Any actual row set not exactly matching a shipped layout → refuse with `PARTIAL_WHEEL_STATE`.** This is restrictive: a single failed tension measurement makes the row set match neither layout. That is the honest Stage 1 behaviour — refusing is correct, and re-measuring the failed spoke is the operator's remedy.

Additional layouts may be added if a specific partial pattern proves common in practice, at the cost of artifact size. Arbitrary partial patterns are combinatorial and cannot be enumerated.

> **DEFERRED — general reduced solve (named task: on-device reduced-solve evaluation).**
>
> The general case requires computing the reduced solve on-device when the row set does not match a shipped layout. This is **not ruled out**, and the §8.7 rationale should not be read as forbidding it: that rationale targets avoiding SVD in a *per-measurement* budget. The solve runs **once per cycle**, which is a different budget entirely — a decomposition of a ~96×32 matrix at that cadence is plausibly affordable on an S3.
>
> **What must be evaluated before adopting it** (none of which is established by current project information):
> - measured execution time and memory on the target part
> - **numerical behaviour, scoped per layout (§8.5).** `TENSION_ABSENT` is structurally rank-deficient, so normal equations are unusable for it — they square the condition number and are singular for a rank-deficient system; a rank-revealing decomposition is required. `FULL` is **not** assumed deficient, so its numerical requirements follow from its *measured* rank and conditioning rather than from a blanket assumption. A rank-revealing method remains the safer default for both.
> - conditioning of realistic reduced layouts (§8.11 R5)
>
> Do not implement this speculatively. Stage 1 uses the fixed-layout path.

**Weights are baked into every shipped Φ†.** Changing μ or tolerances invalidates all layouts and requires artifact regeneration host-side.

**Required guardrail — FINGERPRINT check at artifact load, not merely dimensions.**

Verify that the artifact's stored dimensions match `n_full_rows` for Φ and `n_layout_rows(L)` for each Φ†. **A dimension mismatch MUST abort with `ARTIFACT_INVALID`, never be reshaped, padded, or truncated.**

> **Dimension checking alone is NOT sufficient.** Two different wheels can both be 32-spoke / 32-angle and produce matrices of **identical dimensions that are physically wrong for each other** — different flange geometry, rim stiffness, spoke gauge, cross pattern, or indexing origin all change Φ's contents while leaving its shape untouched. A dimensions-only check passes that case silently, which is precisely the failure class this document exists to prevent.

> **An artifact carrying both its generating parameters and a hash of those same parameters can always verify ITSELF.** Self-consistency proves nothing about whether the artifact belongs to the wheel currently configured — a perfectly self-consistent artifact for the *wrong* wheel would pass.

**Three checks, in order, and the second requires an INDEPENDENT expectation:**

| # | Check | Against |
|---|---|---|
| 1 | **Integrity** — `content_hash` over stored coefficients and pseudoinverses | The artifact itself (detects corruption) |
| 2 | **Compatibility** — artifact's `generating_fingerprint` | **`expected_influence_fingerprint` in the wheel-class configuration** (§11.1) — *not* in the artifact. **This is the sole compatibility authority**; there is no separate artifact-ID binding to check, because an ID adds nothing the fingerprint does not already cover. |
| 3 | **Shape** — dimensions and layout row masks | Active configuration (§8.7) |

**Check 2 is the load-bearing one and needs an authority outside the artifact.** An artifact whose fingerprint differs from that binding aborts with `ARTIFACT_INVALID`, however internally consistent it is.

**Two distinct hashes, do not conflate:** `generating_fingerprint` is *provenance* (which parameters should have produced this); `content_hash` is *integrity* (are the stored numbers intact).

**The fingerprint covers every model-preparation parameter whose value can change Φ, Φ†, or whether the artifact is accepted:**

`n_spokes` · `n_cross` · `rim_diameter` · rim cross-section properties · spoke geometry and material · full hub geometry (per-side flange diameters and widths) · `asymmetric` · `indexing_origin` · `N_lat` · `N_rad` · `n_fit_samples_lat` · `n_fit_samples_rad` · `n_rim_angles` · all normalization/acceptance tolerances · all trust weights · `max_condition_number` · `rank_tolerance` · `symmetry_angle_tolerance` · `n_mt_displacement_tolerance` · `n_mt_tension_tolerance` · `n_mt_uniqueness_threshold` · `T_target_assumed` · `c_side_a` / `c_side_b` · per-block source/generation assumptions · generator version

Two artifacts generated from materially different model-preparation assumptions MUST NOT produce the same compatibility fingerprint merely because their matrix dimensions match. Runtime/session fields that do not affect the influence artifact are excluded.

**Mismatch ⇒ `ARTIFACT_INVALID`.** Regenerate host-side; do not proceed on a dimensionally-compatible but physically foreign artifact.

### 8.8 Influence matrix generation — column structure

**Φ_u and Φ_v (displacement channels):**

> **⚠ The single-rotated-curve reduction is a SYMMETRY assumption, not a general property.**
>
> The reference work generated its displacement influence functions as the average of 32 measured curves normalized to a common spoke angle — i.e. one mean curve, rotated per column. **That averaging is only meaningful if all spokes produce the same response shape**, which holds on its symmetric test wheel and does not hold in general: Side A and Side B spokes on an asymmetric wheel have different bracing angles, hence different lateral leverage, and may differ in magnitude and shape.
>
> The reduction was additionally a *measurement-economy* device — averaging 32 noisy measured curves into one suppresses noise. **An analytical generator has no measurement noise to suppress and no reason to average.**

**Required generation rule:**

- **Generate displacement columns per spoke (or per class) directly.** For `asymmetric: false`, columns will emerge identical up to rotation — which is a useful self-check on the generator, not something to assume in advance.
- **Do not apply the single-curve reduction to a wheel configured `asymmetric: true`.**
- The `coarse_measured` and `full_measured` artifact sources may still average within a class for noise suppression, but **never across classes on an asymmetric wheel.**

**Φ_t (tension channel):** generated from **four distinct class patterns**, cycled across columns:
1. Side A leading
2. Side A trailing
3. Side B leading
4. Side B trailing

For a **symmetric** wheel these collapse into two mirror pairs. **On an asymmetric wheel all four are independent** — the four-class *structure* (side × lead/trail) holds regardless of symmetry; only the mirror relationship between classes is symmetry-dependent.

**CRITICAL — do not write one generic "rotate the curve" generator for all three channels.** Applying the single-curve rotation rule to Φ_t produces a matrix of the correct dimensions, with plausible magnitudes, that is wrong. Nothing downstream will catch it.

**Mirroring is an acquisition-time shortcut, not a structural property.** Store four independent class curves. For a symmetric wheel, mirroring may be used to derive two of them from the other two. For a dished wheel, all four are independent. Baking mirroring into the storage format forecloses dished-wheel support.

**Required unit test — stated precisely enough to implement deterministically.**

Let `k = n_spokes / 4` be the number of spokes per class, and let spokes of the same class be separated by 4 index positions (the class pattern repeats every 4 spokes, §6.5). `Φ_t` is `n_spokes × n_spokes`: entry `[j, i]` is the tension change at spoke *j* from a unit adjustment at spoke *i*.

**Same-class columns are related by cyclic shift, not by equality:**

```
POSITIVE:  for all i,  circshift( Φ_t[:, i], 4 )  ≈  Φ_t[:, i+4]     (indices mod n_spokes)
NEGATIVE:  for all i,  circshift( Φ_t[:, i], 1 )  ≉  Φ_t[:, i+1]
```

where `circshift(v, m)` rotates the row index by `m` positions modulo `n_spokes`, `≈` means element-wise agreement within a configured tolerance, and `≉` means that agreement fails for at least one element by more than that tolerance.

**Both directions are required.** The positive assertion alone is satisfied by a degenerate generator that produces identical columns everywhere. The negative assertion is what proves the four classes are genuinely distinct.

**Note:** the positive test compares same-class columns *after* index alignment — they are **not** literally equal, since each column's response is centred on its own spoke. Comparing raw columns for equality will fail on a correct generator.

### 8.9 Fourier representation and sampling

Influence functions are fitted to a Fourier series to reduce measurement noise and because they are continuous and 2π-periodic:

```
y(θ) = a₀ + Σ[n=1..N] (aₙ·cos(nθ) + bₙ·sin(nθ))
```

**Reference model orders: N_lat = 6 (13 coefficients), N_rad = 13 (27 coefficients).**

**These are the reference wheel's values.** Spatial content depends on rim stiffness, so a different rim may require different orders. **Both are configuration, not constants.**

**Two different sample counts — do not conflate them.** An earlier revision applied the Fourier floor to `n_rim_angles`, which is the *solver evaluation grid*, not the fitting sample count.

| Quantity | Stage | Meaning |
|---|---|---|
| `n_fit_samples` | **Model prep** | Observations used to FIT the Fourier coefficients |
| `n_rim_angles` | **Solver grid** | Angles at which the fitted curves are EVALUATED to build Φ rows, and at which runtime runout is measured |

An already-fitted compact curve can be expanded onto **any** angular grid (§11.4). The two are independent: an analytical generator may evaluate densely, fit from `n_fit_samples = 256`, and still emit a 32-row runtime matrix.

**The floor applies to `n_fit_samples` only, and covers BOTH channels:**

```
n_fit_samples ≥ 2·max(N_lat, N_rad) + 1
```

If the channels are fitted from **different** sample counts, store them separately (`n_fit_samples_lat`, `n_fit_samples_rad`, §11.4) and apply each channel's own floor. **The counts actually used are recorded in generating parameters** and enter the fingerprint.

A Fourier series with highest mode N has 2N+1 unknowns. Below this count the radial fit **aliases silently** — it produces a smooth, plausible curve that is wrong.

**Margin guidance for `n_fit_samples`:** at exactly `2N+1` the fit is interpolation, not regression — it passes through every point and rejects no noise. The reference used 64 fitting samples against `N_rad = 13` (≈2.4× the minimum).

**For an `analytical` artifact this is nearly free.** `bike-wheel-calc` can be evaluated at as many rim angles as desired, so `n_fit_samples` should be set generously (dense evaluation, e.g. ≥ 4× the floor) — there is no measurement cost. **The margin problem only bites for `coarse_measured` / `full_measured` sources**, where each sample is a physical reading.

**Stage 1 policy: `n_rim_angles = n_spokes`.** So a 32-spoke wheel uses 32 runout angles and a **36-spoke wheel uses 36** — an earlier revision wrote "32 (one per spoke)", which cannot hold for the 36-spoke wheels this project's scope supports (§11.1).

**This ties the runout grid to spoke indexing deliberately**, because the wheel is already positioned spoke-by-spoke for acoustic measurement, so runout readings are taken at the same stops with no additional indexing scheme to track.

**This is a runtime-measurement choice, not a fitting constraint.** It is operationally free because the wheel is already indexed spoke-by-spoke for acoustic measurement. A complete lateral/radial pass requires `2 × n_spokes` scalar dial readings. It must satisfy §8.11's determinacy and conditioning rules for each shipped layout — which is a rank question, not a Fourier-coefficient-count question.

**Clearing the coefficient count is a necessary condition, not a sufficient one.** For measured artifacts, inspect radial fit residuals and check stability across cycles; if fits are unstable, reduce `N_rad` before increasing sample count.

Once runout sensing is automated (Stage 2), sampling becomes cheap and this constraint stops binding.

> **⚠ Reducing `N_lat` or `N_rad` also reduces the displacement blocks' rank (§8.5).** Fourier order bounds the available basis vectors independently of row count, so a reduction that stabilizes a noisy fit can simultaneously push `TENSION_ABSENT` past its expected null-space dimension into an unusable state. **Model prep MUST re-verify rank and conditioning for every layout after any change to `N_lat` or `N_rad`**, and the artifact records the orders used alongside the resulting rank figures.

### 8.10 Influence matrix source

The artifact carries a `source` field: `{literature_seeded, analytical, coarse_measured, full_measured}`.

**Stage 1 primary path: `analytical`, generated host-side via `bike-wheel-calc`** (github.com/dashdotrobot/bike-wheel-calc, MIT licensed, Python/NumPy).

Its governing equation includes a spoke adjustment term directly:

```
(K_rim + K_spk)·d = F_ext + A_adj·a
```

where `A_adj` is the spoke adjustment matrix and `a` is the vector of spoke length changes due to nipple rotation. Setting `a` to a unit adjustment on one spoke with zero external force, solving, and evaluating `rim_def_lat(theta, d)` / `rim_def_rad(theta, d)` at the rim angles yields Φ_u and Φ_v columns directly.

**Advantages:** parameterized by geometry (32/36 spoke, 3-cross, dish all supported natively via `lace_cross()` and per-side hub dimensions); requires no measurement campaign; documented spoke class ordering matches the four-class structure.

**AGENT'S FIRST TASK on this path: verify the repository runs.** Clone it, install it, run its examples and tests against the current Python version. If it is bit-rotted or its API has drifted, report this before building anything on top of it. Fallbacks: `bike-wheel-api` (REST service variant by the same author), or digitizing published influence function figures.

#### 8.10.1 Generating Φ_t — must be resolved before implementation

The procedure above yields Φ_u and Φ_v columns directly. **`FULL` also requires Φ_t, and its analytical generation path is NOT yet specified.**

**Required first step (part of the §8.10 repo verification):** determine whether `bike-wheel-calc` exposes the per-spoke tension response to a unit spoke adjustment. Its documentation describes computing changes in spoke tension under applied loads, so the capability plausibly exists via the same `A_adj` path — **but this must be confirmed against the actual API, not assumed.** Document the exact call sequence once confirmed.

**If it does not**, Φ_t must come from another analytical, empirical, or reference source, and that is a legitimate outcome — the four-class structural rules of §8.8 hold regardless of where the numbers originate.

**Per-block provenance is therefore MANDATORY.** A single artifact-wide `source` value would falsely assert identical provenance across blocks that may legitimately differ:

```
source_u   {literature_seeded, analytical, coarse_measured, full_measured}
source_v   (same domain)
source_t   (same domain)
```

The artifact-level `source` field, where retained, means "the most provisional of the three" and is never a substitute for the per-block values.

**Known input burden:** the tool requires rim cross-section properties (area, second moments of area for lateral and radial bending, torsion constant, Young's and shear moduli). Rim manufacturers do not publish second moments of area. For Stage 1, estimated values from measured cross-section geometry are acceptable — a plausible estimate produces a *structurally valid* Φ, which is what the Stage 1 claim requires. Accuracy against the physical wheel is a Stage 2 concern.

**The artifact MUST record the wheel parameters that generated it** — not merely `source: analytical`. A later comparison between analytical and measured Φ is only reproducible if the generating inputs are known.

---

### 8.11 Partial wheel state — solver admission rules

Partial state is representable (§6.3). It is **not** automatically solvable. The solver MUST evaluate these rules and either proceed or refuse with `PARTIAL_WHEEL_STATE`.

**Structure of the problem.** Missing measurements remove *rows* (observations) from Φ̃ and Ỹ. They do not remove *columns* (the per-spoke unknowns). A spoke with no measurements can still receive an adjustment — it is an unknown, not an observation. What partial state costs is constraint, not reach.

**Availability and the active row set are DIFFERENT things.**

```
available_rows  = rows whose measurement is valid or suspect
active_row_set  = f( available_rows, solver policy )
```

**Policy may deliberately exclude an entire channel even when its measurements succeeded.** The case that forces this distinction: if `n_mt` cannot be identified (§8.4 Part 1), the solver must run `TENSION_ABSENT` — but all `n_spokes` tension measurements may be perfectly good. Selecting a layout from availability alone would make that fallback unreachable, since all-tension-present maps to `FULL`.

**Sanctioned policy exclusions** (each recorded on the `AdjustmentPlan` with its reason):

| Trigger | Active row set | Reason |
|---|---|---|
| `n_mt` not identified | `TENSION_ABSENT` | `MEAN_TENSION_MODEL_UNAVAILABLE` |
| Operator/config requests geometry-only | `TENSION_ABSENT` | policy |

**A policy exclusion MUST NOT be recorded as measurement failure.** The measurements keep their true status; only the active row set changes.

**Admission rules — all must hold:**

| Rule | Condition | On failure |
|---|---|---|
| **R1 — layout match** | The **active row set** exactly matches a shipped Φ† layout (§8.7.1) | Refuse: `PARTIAL_WHEEL_STATE` |
| **R2 — determinacy** | `n_active_rows ≥ n_spokes` | Refuse: `PARTIAL_WHEEL_STATE` |
| **R3 — conditioning** | The layout's `effective_condition_number` / effective rank is within configured limits | Refuse: `ARTIFACT_INVALID` |
| **R4 — tension channel** | Tension rows are **all present** or **all absent** | Partial tension loss → refuse via R1 |

```
n_active_rows = cardinality( active_row_set )
```

**All solver row-count logic uses `n_active_rows`** — R2 determinacy, J normalization (§8.6), and the observation-vector dimension. `available_rows` answers *what data exists*; `active_row_set` answers *what optimization problem is being solved*. Never substitute one for the other.

**R1 is the operative constraint in Stage 1** and subsumes most partial-state cases: since only `FULL` and `TENSION_ABSENT` layouts ship, any other pattern of missing rows is refused. R2 is retained as an independent necessary check that must hold for any future layout.

**R3 — effective conditioning, defined explicitly for rank-deficient layouts.**

> **A standard condition number is INFINITE for an exactly rank-deficient matrix.** `TENSION_ABSENT` is rank-deficient by design (§8.5), so an implementation using an ordinary `cond()` would reject **every legal `TENSION_ABSENT` artifact**. The criterion must be defined over the retained subspace.

```
σ₁ ≥ σ₂ ≥ … ≥ σ_n            singular values of Φ̃(L)
retained  = { σ_k : σ_k > rank_tolerance · σ₁ }
effective_rank(L)  = |retained|
effective_cond(L)  = σ₁ / min(retained)

admissible  ⇔  effective_cond(L) ≤ max_condition_number
           AND effective_rank(L) == artifact's recorded effective_rank(L)
           AND ( n_spokes − effective_rank(L) ) == recorded expected_null_dim(L)
```

`rank_tolerance` is a **relative** threshold against the largest singular value, so the test is scale-invariant. Singular values at or below it are treated as structurally zero and excluded from the ratio.

**Both the rank and the nullity checks are equality tests against the artifact's recorded values.** Deficiency itself is not the failure; **disagreement with what the artifact says to expect** is (§8.5).

**Where it is computed.** Row count is necessary but **not sufficient**: a square or overdetermined reduced matrix can still be rank-deficient or numerically unusable. Because Stage 1 restricts to precomputed layouts, **conditioning is verified host-side at model-prep time**, once per layout, and the result is recorded in the artifact. The firmware verifies the recorded value against configured limits rather than computing it. If the deferred general reduced solve (§8.7.1) is ever adopted, conditioning must be checked on-device at solve time.

The artifact field `effective_condition_number` means exactly `effective_cond(L)` above. It MUST NOT contain the ordinary full-matrix condition number for a rank-deficient layout. Host model preparation computes and records this definition; firmware interprets and validates the stored field using the same definition and `rank_tolerance` contract.

**The rank criterion differs by layout (§8.5).** For `TENSION_ABSENT`, structural deficiency is expected, so the test is *effective* rank and conditioning against the expected null-space dimension. For `FULL`, deficiency is **not** assumed — model prep measures its rank and conditioning, and a deficient or ill-conditioned FULL layout is an artifact finding to be surfaced, not a normal condition to be accommodated. Both results are recorded per layout in the artifact.

**No Fourier floor applies at runtime.** The `2·N + 1` sampling constraint (§8.9) governs **influence-function generation**, where Fourier fitting actually occurs — model prep, host-side. Runtime runout measurements enter the solve as **raw sample rows**, not as fitted coefficients, so no coefficient-count floor applies to them. An earlier revision of this document incorrectly placed that constraint here. *If* runtime Fourier smoothing of measured state is ever implemented, the floor applies at that stage.

**R4 — the tension channel is all-or-nothing in Stage 1.**

An earlier revision stated that "any number of tension rows may be missing" and that this is "never a refusal." **That contradicted §8.7.1 and is corrected here.** The three cases:

| Tension rows | Layout | Outcome |
|---|---|---|
| All present | `FULL` | Nominal solve |
| **All** absent, rejected, **or excluded by policy** | `TENSION_ABSENT` | Geometric truing proceeds; `CONVERGED_GEOMETRIC_ONLY` semantics (§8.6.2) |
| **Some** missing | matches neither | **Refuse via R1** — `PARTIAL_WHEEL_STATE` |

**What remains true:** losing the tension channel *as a whole* never blocks shape truing, because mean tension is unconstrained by the displacement channels (§8.5). What is **not** supported in Stage 1 is arbitrary partial tension-row loss, because no precomputed Φ† exists for that row set. The remedy is to re-measure the failed spokes.

Additional layouts may be shipped if a specific partial pattern proves common (§8.7.1), at the cost of artifact size.

> **Dropping the channel is NOT the same as zeroing its residuals.** Setting `T̂ = 0` asserts that tension is exactly at target — a claim, not an absence — and biases the solution. Tension-absent operation requires a **Φ† computed without the tension block**, which is why `TENSION_ABSENT` is a separately shipped layout rather than a runtime masking operation.

**Handling `suspect` rows.** Rows whose measurement is `suspect` — including every `TensionEstimate` produced while mode identification is provisional (§4.4.1) — are admissible but MUST be recorded in the result. `AdjustmentPlan` carries the count of valid versus suspect rows that contributed, so a downstream consumer can judge how much of the solution rests on provisional data.

**Never do this:** do not zero-fill, interpolate across, or otherwise synthesize a missing measurement to satisfy a rule. A fabricated row is indistinguishable from a real one inside the solve and corrupts the result silently. Refuse instead.

---

### 8.12 Provenance audit — what is general, what is inherited, what needs validation

**The reference work validated on a symmetric front wheel. This project's primary donor wheel is *expected* to be asymmetric — pending confirmation from configured and measured hub geometry per §6.4.1, not inferred from the presence of a rotor.** Every result below is classified so that a reference-wheel simplification is never mistaken for a general law.

| Item | Class | Status |
|---|---|---|
| Superposition of per-spoke influences | General mechanics | Valid within the linear regime — **but linearity itself is an assumption** (§8.1) |
| Four tension classes (side × lead/trail) | General mechanics | Structure holds for any wheel |
| Weighted least squares as the solve | General method | Valid |
| Displacement channels dominate; tension de-weighted | Reference-instrument property | **Re-derive** — depends on this project's tension precision (§8.3.2) |
| Mirror relationship between Φ_t classes | **Reference simplification** | Symmetric wheels only (§8.8) |
| Single rotated curve for Φ_u / Φ_v | **Reference simplification** | Symmetric wheels only (§8.8) — was wrongly stated as general |
| Common-mode direction = `[1,…,1]` | **Reference simplification** | Symmetric wheels only (§8.4 Part 1) — was wrongly stated as general |
| Scalar `c` | **Reference simplification** | Per-side required (§8.4 Part 2) |
| Asymmetric mean-tension compensation | **Not derived anywhere** | OPEN (§8.4) |
| Displacement null space is 1-D | **Not established** | OPEN — and null space alone does not identify `n_mt` (§8.4 Part 1) |
| Rank deficiency of the FULL matrix | **Project assumption, was wrongly stated as general** | Not assumed — `n_mt` is visible to Φ_t; measure per artifact (§8.5) |
| Displacement block has a structural null space | General mechanics | Holds; `n_mt` lies in it, possibly with other directions |
| Uniform tension change is the mean-tension mode | **Reference simplification** | Symmetric only — contradicts displacement invariance on asymmetric wheels (§8.4 Part 1) |
| Equal-per-spoke scalar tension target | **Reference simplification** | Symmetric wheels only. The general target is one overall `target_tension` scale applied to artifact-bound `T_target_assumed`; per-side absolute targets are derived. Asymmetric `d_cm` compensation remains OPEN (§8.4 Part 2). |
| `n_mt` exists and is unique, asymmetric | **Not established** | OPEN — refuse mean-tension targeting if not identified (§8.4 Part 1) |
| μ_v = 0.5, μ_t = 10⁻⁵ | Reference values | Not transferable (§8.3.2) |
| N_lat = 6, N_rad = 13 | Reference values | Rim-dependent; config (§8.9) |
| c = 473 N/rev | Reference value | That wheel only (§8.4) |
| n_rim_angles = 64 | Reference choice | Stage 1 uses `n_rim_angles = n_spokes`; validation required (§8.9) |
| Single-iteration convergence | Reference claim | This project assumes multiple cycles (§7.1) |

**Two errors were present in earlier revisions of this document and are corrected above:** the single-rotated-curve rule for displacement columns, and the arithmetic-mean projection for shape truing. Both were reference-wheel simplifications stated as general rules, and both would have produced plausible, physically wrong results on the project's actual donor wheel.

---

## §9 Acoustic Subsystem

### 9.1 External interface — coarse-grained

```
measure_spoke_tension(spoke_id, wheel_geometry) → TensionEstimate
```

**One call.** The subsystem internally owns excitation, capture, analysis, any damping ritual, and model application.

**Rationale for coarse granularity:** the internal sequence is an unresolved research question. If the orchestrator sequenced `pluck()` → `capture()` → `analyze()`, then adding a damping step would change the orchestrator. With a coarse boundary, it changes nothing outside the subsystem.

**Why `wheel_geometry` is a parameter:** if damping targets a *neighbor* spoke, the subsystem must resolve which spoke that is. This dependency is declared regardless of whether damping is ultimately implemented.

**Classification: sensor+actuator composite.** This subsystem commands hardware. Do not model it as a passive sensor.

**It does not move the wheel.** The spoke is brought to the acoustic station by the Wheel Navigation / Positioning capability (§10A) before `measure_spoke_tension()` is called. The acoustic subsystem consumes that positioning and owns none of it — neither the wheel drive nor the station's location.

### 9.2 Internal seams

Four internal layers, each independently replaceable:

```
1. Front end          transducer → RawAudioBuffer
2. Spectral analysis  RawAudioBuffer → spectrum + peak candidates
3. Mode identification candidates → selected frequency + identity metadata
4. Physics model      selected frequency + spoke params → TensionEstimate
```

**Layer 2 is the validated, portable piece.** Source it from the acoustic repo.

**Layer 3 — provisional implementation REQUIRED, validated rule unresolved.** These are not in conflict, and an earlier revision's "do not invent an implementation" was too blunt:

| Aspect | Status |
|---|---|
| A scientifically validated selection rule | **Unresolved research.** Do not invent one. |
| A working implementation for Stage 1 | **Required.** Port whatever rule currently exists in the acoustic repo. |
| Its output | **Marked provisional** — `mode_identity: presumed_fundamental`, downstream `TensionEstimate.status: suspect` with `PROVISIONAL_MODE_ID` (§4.4.1) |
| Replacing it later | **MUST NOT change layers 1, 2, or 4.** That is what the seam is for. |

"Do not invent" means do not fabricate a *validated* rule or present a provisional one as settled. Porting the research repo's current rule and labelling it provisional is correct and required.

**Layer 3 does NOT establish physical mode identity.** It returns a *selected candidate* plus identity metadata, consistent with `FrequencyMeasurement` (§6.3). It must not name its output "the fundamental" or otherwise encode as fact something the rule has not established.

**Layer 4 — three distinct concepts, do not collapse them.** "No default is settled" does not mean "no model is selected"; every run needs a concrete model.

| Concept | Status |
|---|---|
| **Supported candidates** | Ideal string, stiffness-corrected, higher-mode-based, empirical. All implementable; all behind one interface. |
| **Session-selected model** | **Required, and session-FIXED.** Chosen by configuration before `START_TRUING`, recorded in `TensionEstimate.model_name` and the session header, and **immutable for the duration of the session** (§12.3.1). A run with no model selected is a configuration error, not a degraded mode; changing models requires a new session. |
| **Project-wide validated default** | **Does not exist.** No candidate has been validated against a traceable reference on this project's hardware. The config has no blessed value to fall back on. |

An agent must therefore require the model to be specified in configuration rather than hardcoding a fallback — a silent default here would be an unvalidated model presented as a considered choice.

### 9.3 Front end

**Stage 1 implementation: INMP441 (I2S digital MEMS microphone).**

**The seam must exist even though only one implementation is built.** An analog path (magnetic pickup + ADC/codec) may be required if MEMS SNR proves inadequate. Because layer 1 is its own interface, adding it later is a new driver file — layers 2–4 are unaffected. Implement I2S only; define the interface so the alternative is a drop-in.

**Fixed system audio format:** 48 kHz, 24-bit input in 32-bit I2S slots, processed as float32 internally.

Sources adapt to this format; the format does not adapt to sources. DSP constants are expressed against sample rate, and existing recorded fixtures are at 48 kHz.

### 9.4 Capture robustness — REQUIRED

**The capture path must tolerate task preemption.**

Capture is DMA-driven: hardware fills a buffer, the audio task drains it before the next fills. If the task is starved longer than the buffer holds, samples are dropped. A dropped-sample discontinuity smears energy across the entire spectrum in an FFT, raising the in-band noise floor and corrupting the measurement.

**Requirements:**

- **Size DMA buffers from internal DMA-capable RAM — NOT from PSRAM.** See the memory constraint below.
- **Separate the drain deadline from the DSP deadline (§4.5).** The requirements in this section apply specifically to the **DMA drain path**, which has a hard deadline set by buffer refill period. FFT and peak picking have a soft deadline (finish before the next excitation) and must not run at drain priority. Treating the acoustic pipeline as one undifferentiated "audio task" is the design error this separation exists to prevent.
- Pin the drain path to core 1, away from WiFi and comms (§4.5).
- **Stress test:** verify capture integrity under active WiFi load. This must pass before the acoustic subsystem is considered complete.

#### 9.4.1 DMA memory and buffer sizing

> **An earlier revision of this document made two errors here, both corrected:** it claimed PSRAM makes DMA buffer sizing free (wrong on the ESP32-S3), and it then required the *application* to allocate all I2S DMA buffers and descriptors (wrong for the ESP-IDF I2S driver, which owns them).

**Ownership: the driver allocates, the application does not.**

- `i2s_new_channel()` allocates DMA buffers and descriptors **internally**, sized by `dma_desc_num` and `dma_frame_num` in `i2s_chan_config_t`. The ESP-IDF documentation is explicit that these buffers are internal to the read path and that descriptors are created automatically inside the driver.
- `i2s_channel_read()` **copies** from those driver-owned DMA buffers into a caller-supplied buffer. **That caller buffer is not a DMA target** and may live in PSRAM. The documentation requires it to be at least as large as the total size of all DMA buffers.

**Invariants — stated as requirements, not as allocation calls:**

1. **Driver-owned DMA structures must reside in memory the selected ESP-IDF and GDMA configuration supports** — i.e. internal DMA-capable memory. This is a property to **verify for the IDF version actually in use**, not to enforce by reallocating driver internals.
2. **Any application-owned DMA buffer, if one is ever introduced,** must use explicitly DMA-capable internal memory (`MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA`). **Never rely on plain `malloc()` placement when PSRAM is enabled** — with PSRAM configured, `malloc()` may serve from external RAM.
3. **Reserve internal DMA-capable memory** via `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL` (or the equivalent in the IDF version used) where the internal pool proves tight. ESP-IDF provides this precisely because some buffers can only be allocated internally.
4. **PSRAM remains useful for the analysis path.** Post-capture working buffers, FFT scratch, and spectra are not DMA targets.
5. **Bring-up check (required):** confirm `i2s_new_channel()` **returns success** with PSRAM enabled before building on it. This is the check that matters — the failure mode is silent when return values go unchecked.

*Supporting evidence for requirement 5, not the normative basis:* a public issue report against the ESP32-S3-DevKitC-1 N16R8 — this project's development target — records I2S channel creation failing with PSRAM enabled, because driver-internal allocations landed in PSRAM and GDMA rejected them, with downstream failures that were silent because return values were unchecked. The normative requirement is derived from the IDF memory documentation; the report is why the bring-up check is mandatory rather than optional.

**Hard platform constraints on DMA sizing:**

```
dma_buffer_size = dma_frame_num × slot_num × slot_bit_width / 8   ≤ 4092 bytes
dma_frame_num   ≤ 511
```

**At 24-bit data width, `dma_frame_num` must be a multiple of 3.** This project's fixed format is 24-bit (§9.3), so **this constraint binds** — a non-multiple-of-3 value corrupts the data or the effective sample rate.

**Documented sizing method** — use this rather than an unquantified "generous margin":

```
interrupt_interval = dma_frame_num / sample_rate
dma_desc_num       > polling_cycle / interrupt_interval
read_buffer_size   ≥ dma_desc_num × dma_buffer_size
```

Choose `polling_cycle` from the worst-case interval at which the drain path can be scheduled under load — which is what the WiFi-load stress test measures.

This is a robustness requirement covering *all* preemption sources, not a prohibition on any specific one.

### 9.5 Unresolved — do not invent a validated answer

**"Unresolved" here means no *validated* answer exists. Where Stage 1 needs something to run, port the research repo's current implementation and mark its output provisional (§9.2) — do not fabricate a rule and do not present a provisional one as settled.**

- **Mode identification rule.** Active research. A provisional implementation is required for Stage 1 and its output is marked `presumed_fundamental` / `PROVISIONAL_MODE_ID`. Read the acoustic repo for current state.
- **Validated tension model.** Candidates are implementable and one must be selected per session by configuration; none is validated as a project default (§9.2).
- **DSP window and gate constants.** Research outputs. **Configuration, never literals.** Window length materially affects frequency resolution, and the acoustic repo's own analysis recommends longer windows than early defaults.
- **Effective vibrating length (`L_eff`).** Research output, not a config constant to fill in (§4.4.1).
- **Damping mechanism.** May or may not prove necessary. Owned internally by this subsystem either way; the architecture is unaffected by the outcome.

---

## §10 Runout Subsystem

### 10.1 Interface

```
read_snapshot(rim_angle) → RunoutMeasurement    // lateral + radial
stream_samples(...) → sample stream             // DEFINED, NOT IMPLEMENTED
tare()                                          // zero the gauges at setup
```

**The orchestrator owns the indexing loop and pulls readings at known rim angles.** The subsystem does not decide when or where to read. Nor does it move the wheel: the rim location is brought to the runout station by the Wheel Navigation / Positioning capability (§10A), and the runout subsystem never touches the wheel drive.

**Snapshot and streaming are separate capabilities, and an implementation may support only one.** A snapshot-only device (manual entry, a slow dial gauge) MUST report `stream_samples()` as `unavailable` with `NOT_IMPLEMENTED` — per P2, it returns a status, never a fabricated or degraded stream. Forcing a snapshot-only sensor to synthesize a stream would produce plausible-looking feedback data for a closed loop that has no real feedback, which is the worst available outcome.

**Capability query is part of the contract.** A caller can ask which capabilities an implementation supports before attempting to use one, so the actuation loop can refuse cleanly at setup rather than failing mid-adjustment.

### 10.2 Implementations

All satisfy the same contract:

| Implementation | Status |
|---|---|
| Manual entry (operator reads dial, enters value) | Stage 1 primary |
| Digital dial gauge via GPIO (clock/data, level-shifted) | Candidate |
| Laser displacement sensor | Candidate, Stage 2 preferred |
| Simulated / recorded | Testing |

**Manual entry is a real implementation, not a stub.** It must not be removed when an automated sensor lands; the two coexist behind the same interface.

**Manual `read_snapshot()` MUST NOT block synchronously on a network or browser operation.** The orchestrator enters `WAIT_FOR_OPERATOR`, receives a typed `SUBMIT_RUNOUT(lateral, radial)` intent carrying the active `wait_id` (§12.3), and then resumes the logical runout acquisition. A low-level driver that blocks a task waiting on a websocket would put an unbounded host dependency inside the HAL, where the state machine cannot see or cancel it.

### 10.3 `tare()` is part of the contract

The solve assumes u₀ and v₀ are zero — the gauges are zeroed at the desired lateral and radial reference during setup. This is a hardware action with real effect, not a configuration constant. It belongs in the sensor interface.

### 10.4 Streaming — defined, unimplemented

Stage 2's closed-loop actuation requires *continuous* monitoring of lateral displacement during nipple rotation, because commanded nipple rotation does not map reliably to tension change (spoke torsional compliance, nipple-thread friction causing discrete jumps at higher tension). The reference method adjusts until measured lateral displacement reaches the predicted target, rather than applying a fixed rotation.

**Open constraint:** the achievable sample rate of a streaming implementation is unresolved and gates closed-loop actuation. **A sensor adequate for `read_snapshot()` may be inadequate for `stream_samples()`** — a slow dial gauge readout cannot serve a real-time feedback loop. These may require different hardware. This fork is deferred, not decided.

**Snapshot and streaming are different consumers, not competing designs.** Snapshot serves the measurement loop; streaming serves the actuation loop. Adding streaming later does not restructure the measurement path.

---

## §10A Wheel Navigation / Positioning Subsystem

*(Added in v1.1. Numbered "10A" so that existing cross-references to §11–§17 stay valid.)*

**A first-class capability of the final machine, and a large part of what makes it autonomous.** Its fundamental responsibility:

> **Rotate the wheel so that a requested spoke or rim location is aligned with whichever physical station currently requires that location.**

This is more general than "rotate by N steps" or "go to spoke N". It owns, logically:

- rotating the mounted wheel;
- navigating a requested spoke, rim index, or arbitrary rim angle to the station that needs it — for acoustic excitation/measurement, runout measurement, nipple adjustment, and verification / re-measurement passes;
- maintaining sufficient wheel-position/index knowledge during navigation;
- establishing or re-establishing a known wheel reference (§6.5);
- translating logical wheel targets into the physical motion the *current* machine geometry requires;
- stopping wheel motion safely when required.

It serves the whole workflow: every `POSITION` / `POSITION_TO_SPOKE_i` in §7.1 is a request to it. **It is the single authority on the wheel's logical position.** No other subsystem keeps its own idea of where the wheel is, and none of them moves it.

### 10A.1 Three coordinate domains — never collapse them

| Domain | Meaning | Where it lives |
|---|---|---|
| **Wheel coordinates** | rim angle θ of a wheel feature (§6.4 convention; spoke 0 is the origin, §6.5) | data model, orchestrator, measurement subsystems |
| **Station (machine) coordinates** | machine-frame angle φ_S at which a physical station sits | **machine profile (§11.6) — data, never code** |
| **Actuator coordinates** | motor steps, roller revolutions, encoder counts | **inside the navigation implementation only** |

The wheel's rotation state R links the first two: the feature at wheel angle θ currently sits at machine angle `wrap(θ + R)`; bringing θ to station S requires `R' = wrap(φ_S − θ)`. This relation is implemented in exactly one place (`truing/wheel_geometry.h`). **Actuator coordinates never reach the orchestrator or any measurement subsystem**, and no consumer compiles a station offset into its own logic.

A bicycle wheel is a one-degree-of-freedom rotary system; this is the smallest abstraction that keeps the three domains distinct. Do not generalise it into a robotics coordinate framework.

### 10A.2 Contract

```
request(target)               target = { feature: spoke i | rim index k | rim angle θ ,  station: S }
                              → outcome ∈ { DONE, PENDING_OPERATOR, IN_MOTION, REFUSED, FAULT }
poll()                        → current outcome of the active request
note_operator_confirmation()  manual implementation: the operator confirmed the active request
establish_reference()         spoke 0 at the reference station (§6.5) — may itself be PENDING_OPERATOR
query()                       → the position authority: rotation state R, reference_established,
                                 operator_confirmed / sensor_confirmed, status + reason
stop()                        safe stop of any wheel motion (the ABORT safe point, §12.3)
```

**The orchestrator handles both Stage implementations by outcome alone:**

| Outcome | Stage 1 (manual) | Stage 2 (automated) |
|---|---|---|
| `PENDING_OPERATOR` | enter `WAIT_FOR_OPERATOR` with a prompt naming the **feature and the station**; on `CONFIRM_POSITIONED` call `note_operator_confirmation()` | never returned by `request()`; `establish_reference()` MAY return it (hybrid reference, §10A.6) and is handled exactly as in Stage 1 |
| `IN_MOTION` | never returned | poll until `DONE` or `FAULT` |
| `DONE` | feature is at the station; proceed | same |
| `REFUSED` | bad target, unknown station, no reference, unknown calibration | same |
| `FAULT` | — | motion fault; position no longer vouched for (`WHEEL_REFERENCE_LOST`); re-establish the reference before continuing |

**Consumers request or consume positioning; they never own it.** The acoustic subsystem (§9.1) is called after the spoke is at the acoustic station; the runout subsystem (§10.1) reads after the rim location is at the runout station; the Stage 2 nipple actuator (§7.6) acts after the spoke is at the adjustment station. None of them controls the wheel drive, tracks the wheel index independently, or knows a station's angle.

### 10A.3 Stations are configuration, not constants

Station placement on the frame is **not** locked down and may move between mechanical revisions: the acoustic station may end up at a different angle, the runout gauges at another, the nipple actuator on the opposite side; sensing stations may share one location or stay separate; a station may be added. Therefore:

- a positioning request or operator prompt **always names both the feature and the station**;
- station angles live in the machine profile (§11.6) and are loaded, validated and recorded per session (§6.2);
- relocating a station changes the profile or the navigation implementation — **never the workflow logic or a consumer**.

### 10A.4 Actuator-to-wheel mapping is a calibration, not a constant

Never assume "N motor revolutions = M spokes" or "this many roller revolutions = this wheel angle". The relationship depends on unresolved hardware: roller diameter, rim contact diameter, transmission, motor type, microstepping, contact pressure, friction, compliance, slip, encoder placement, tolerances. It is a calibration **owned by the automated implementation**; an implementation whose calibration is unknown MUST refuse with `CALIBRATION_MISSING`, never guess (P2, §11.3.1 "no defaults").

### 10A.5 Position feedback and reference

Commanded motion does not establish absolute wheel position. An automated implementation MUST provide enough information to establish and maintain the wheel's logical angular/index state, permit correction or re-anchoring for slip on a friction drive, and report `FAULT` / `WHEEL_REFERENCE_LOST` when it can no longer vouch for the position. "The motor moved X steps" never entitles the system to conclude indefinitely that "the wheel moved exactly Y radians". How the final system reconciles actuator and wheel domains is a hardware-dependent Stage 2 problem (§16).

### 10A.6 Stage split

**Stage 1 — manual implementation.** The human is the actuator. `request()` answers `PENDING_OPERATOR`; the orchestrator prompts through `WAIT_FOR_OPERATOR` ("position spoke 12 at the acoustic station"); the operator's `CONFIRM_POSITIONED` anchors the rotation state from the confirmed feature and the station's configured angle (`operator_confirmed = true`, `sensor_confirmed = false`). There is no automated wheel-drive requirement for the Stage 1 demonstration, and callers cannot tell that a person moved the wheel.

**Stage 2 — automated implementation, same contract.** Current leading concept: **motor → drive mechanism / rim-contact roller → wheel**, probably beginning with a stepper motor; **expected first development motor driver: TMC2209**. The TMC2209 is a development implementation behind the wheel-drive HAL boundary — **not an architectural dependency and not a committed final driver**; its current capability and compatibility with the eventual motor and mechanical load remain to be validated physically. The C2 → C3 transition is a driver/implementation replacement plus physically validated navigation behaviour, **not** an orchestrator or workflow redesign.

**First development implementation — hybrid reference, open loop** (`navigation_roller`, 2026-09). Recorded so the contract's reading is explicit rather than inferred:

- **Reference by the operator, positioning by the drive.** `establish_reference()` releases the drive (so the wheel turns by hand) and answers `PENDING_OPERATOR` with `CONFIRM_SPOKE0_AT_STATION`; the confirmation re-enables the drive and anchors the rotation state to the drive's step counter. Every `request()` thereafter is `IN_MOTION` → `DONE` / `FAULT`. This is the §10A.2 reading above, not a third mode.
- **Dead reckoning, stated as such.** The rotation state is derived from the pulses the drive actually emitted since the anchor, divided by the calibration — never from the commanded target. `sensor_confirmed` is always false. Slip is not detected; the only re-anchoring is re-establishing the reference. This satisfies §10A.5's `FAULT` obligations (deadline overrun, latched driver fault, stop mid-move) but **not** its slip-correction goal, which still needs sensing (§10A.10).
- **The calibration is signed** (its sign is the direction of wheel rotation for positive steps), carries the microstep setting it was measured under and a reference to its bench evidence, and is refused as `CALIBRATION_MISSING` while any of those is absent (§10A.4).
- **A move deadline** (planned pulse-train duration × a factor + a margin) is a navigation policy that ends a hung or stalled move in `FAULT`; the orchestrator polls without a bound of its own.
- **Provenance:** as real as its drive — `REAL` over a physical driver, `SYNTHETIC` over the test fake.

It runs only in a dedicated development image on the DevKit and a separate wheel stand; the Stage 1 claims (manual positioning, `CONVERGED_GEOMETRIC_ONLY` ceiling) are unchanged by it.

### 10A.7 Safety

`ABORT` during automated wheel motion: bring the drive to a controlled stop, then abort — never mid-motion (§12.3). After a stop mid-move the reference may be lost and MUST be re-established before positioning resumes. A navigation fault never corrupts measurements already recorded; it only refuses further positioning until a reference exists.

### 10A.8 HAL and board ownership

The navigation implementation owns the wheel-drive and index/reference-sensor pins (board profile `BOARD_WHEEL_DRIVE_*`, `BOARD_INDEX_SENSOR_GPIO`, §4.3). The motor driver sits behind a wheel-drive interface expressed strictly in actuator coordinates (`truing_hal/wheel_drive_if.h`); replacing the TMC2209 with another driver changes that implementation only.

### 10A.9 Testing and simulation

- The manual implementation is host-testable through prompt / confirmation sequences.
- A synthetic automated implementation exists with deterministic wheel position and scriptable slip and faults, so navigation-dependent orchestration runs with zero hardware (§14.5) and the believed-versus-true position distinction is exercised.
- The wheel/station geometry relation has golden tests.
- The open-loop development implementation is host-tested against a scriptable fake drive (latency, stall, latched fault, stop mid-move) and inside a full orchestrator session; its step timing and TMC2209 framing are host-tested as pure code.
- Stage 2 adds calibration, slip-detection, positioning-tolerance and reference-sensor tests against real hardware.

### 10A.10 OPEN — Stage 2 hardware decisions (do not invent)

Drive mechanism; motor; motor driver (TMC2209 suitability); roller geometry, material and contact pressure; transmission; microstepping; index/reference sensor and encoder arrangement and placement; station angles of the final frame; positioning tolerance; actuator-to-wheel calibration procedure; odometry method; slip detection and re-indexing policy; motion-control law; terminal handling of an unrecoverable navigation fault. All are resolved by physical testing, never by a value typed in to make code run.

*Development hardware selected for the first bench prototype (2026-09-24):* a NEMA 17 stepper, a TMC2209, and a direct-drive polyurethane rim-contact roller on a 3D-printed mount and shaft. That selects **what is tested**, not what is validated: every item above stays open until measured, and measurements are recorded with their conditions in `docs/ROLLER_BENCH.md`.

---

## §11 Configuration and Model Artifacts

### 11.1 Wheel class configuration

Per wheel type, not per session:

```
n_spokes                    (32 or 36)
n_cross                     (3)
rim_diameter                (622 mm ISO)
spoke_diameter              (uniform round, operator-declared)
spoke_material_modulus
asymmetric                  (bool — see note below)
hub geometry                (flange diameters/widths, per side)
c_side_a                    (N/rev)
c_side_b                    (N/rev)
indexing_origin             (which physical spoke is 0; its class)
symmetry_angle_tolerance    (rad — asymmetric classification, below)
expected_influence_fingerprint      (independent binding — §11.4;
                                     the SOLE artifact compatibility authority)
compatible_model_assumptions        (spoke gauge, material, geometry facts a
                                     tension-model profile must agree with —
                                     §11.3.1; NOT a single approved profile)
```

Side A / Side B per §6.4.1 (Side A = cassette or rotor side).

**On `asymmetric` — determined by GEOMETRY, not by measured tension.**

> An earlier revision said to determine asymmetry by measuring per-spoke tension and looking for a side-to-side difference. **That is the wrong authority.** A side-to-side tension difference is confounded: it also arises from a poor build, uneven tensioning, current truing error, or measurement error. It cannot distinguish "this wheel is geometrically asymmetric" from "this wheel is badly built."

`asymmetric` follows from **configured and measured hub geometry** — per-side flange diameters and widths, hence bracing angles (§§6.4.1, 8.4). Do not default it to `false` for front wheels, and do not infer it from the presence of a rotor.

**Near-symmetric geometry needs an explicit rule — do not leave "close enough" to judgment.** `asymmetric: false` unlocks the symmetry shortcuts this document treats as dangerous when misapplied (single rotated displacement curve, Φ_t mirroring, arithmetic-mean projection, scalar `c`). Classification is conservative and **evidence-based**:

```
asymmetric = false   REQUIRES BOTH:
    | bracing_angle_A − bracing_angle_B |  ≤  symmetry_angle_tolerance
    AND the generated artifact PASSES the symmetric structural property
        tests of §14.3 (n_mt ≈ [1,…,1]/√n_spokes; Φ_t mirror relations)

otherwise asymmetric = true
```

**Demonstration, not declaration:** an artifact claiming `asymmetric: false` whose generated columns fail the symmetric property tests is `ARTIFACT_INVALID`. Classifying a marginal wheel as `asymmetric: true` is always safe; the converse is not.

**Three distinct characterization operations — keep them separate:**

| Operation | Method | Determines |
|---|---|---|
| **Geometry classification** | Measure hub flange positions and diameters; compute bracing angles | `asymmetric`, hub geometry fields |
| **Target ratio validation** | Measure per-side tension on a well-built wheel; compare against the ratio the geometry predicts | Confirms (does **not** establish) the expected operating ratio |
| **`c` calibration** | **Apply a known, bounded, safe per-side nipple rotation; measure the resulting change in that side's mean tension. `c_side = Δ(mean tension) / Δ(rotation)`** | `c_side_a`, `c_side_b` |

> **`c` cannot be derived from a static tension snapshot.** `c` is a *response* coefficient — change in mean tension per revolution of nipple rotation (N/rev). A measurement of existing tensions gives a state, not a derivative. Characterizing it **requires a known adjustment and a before/after comparison** (§8.4). An earlier revision wrongly folded this into the static measurement pass.

The reference method's symmetry-based simplifications do not apply when `asymmetric` is `true`.

### 11.2 Solver configuration

```
tol_lateral                 (mm — normalization AND acceptance tolerance)
tol_radial                  (mm)
tol_tension                 (N — NORMALIZATION scale only, §8.6)
tol_tension_cv              (dimensionless — per-side non-uniformity acceptance)
tol_mean_tension_error      (N — mean-target acceptance, §8.6)
tol_angular                 (rad — column localization test, §6.5)
max_condition_number        (per-layout conditioning limit, §8.11 R3)
rank_tolerance              (effective-rank determination threshold)
n_mt_displacement_tolerance (condition (i): ‖[Φ_u;Φ_v]·n_mt‖ threshold)
n_mt_tension_tolerance      (condition (ii): agreement with T_target_assumed)
n_mt_uniqueness_threshold   (separation margin required inside the
                             displacement null space to call n_mt unique)
max_adjustment_revolutions  (safe bound; exceeding ⇒ ABORT_UNSAFE_ADJUSTMENT)
partial_state_remeasure_attempts   (§7.3.1)
trust_radial                (dimensionless; reference relative magnitude 0.5)
trust_tension               (dimensionless; re-derive — see §8.3.2)
N_lat                       (6 reference)
N_rad                       (13 reference)
n_rim_angles                (`n_spokes`, Stage 1)
target_tension              (N — overall MEAN tension scale; configurable
                             between sessions, immutable within one.
                             Per-side targets are DERIVED, never set here)
tension_model_profile_id    (session-fixed selector; §11.3.1, §12.3.1)
adjustment_deadband         (revolutions; below this, skip the spoke)
lateral_deadband            (mm; closed-loop stop band — Stage 2, §7.6)
max_cycles
min_relative_improvement    (§7.5.1 — derive from measured J repeatability)
consecutive_non_improving_cycles   (§7.5.1 — ≥ 2 until derived)
measurement_retry_count     (bounded; §7.4)
excitation_settle_ms        (ring-down delay before re-excitation; §7.4)
```

**Displacement tolerances serve double duty:** `tol_lateral` and `tol_radial` normalize their channels (§8.3.1) *and* define acceptance, which keeps the optimizer's scaling tied to engineering significance.

**The tension channel does NOT work that way — three separate values:**

| Value | Units | Role |
|---|---|---|
| `tol_tension` | N | **Normalization only.** Never an acceptance criterion. |
| `tol_tension_cv` | dimensionless | Per-side non-uniformity acceptance |
| `tol_mean_tension_error` | N | Per-side mean-target acceptance |

An earlier revision said all tolerances serve double duty and compared non-uniformity against `tol_tension`, which is dimensionally incoherent (a CV against a newton value).

**Exact definitions, so host and firmware cannot differ:**

```
target(S)          = mean( target[i] : spokes i on side S )
mean(T_S)          = arithmetic mean of measured tensions on side S
stdev(T_S)         = POPULATION standard deviation (divisor N, not N−1)
non_uniformity(S)  = stdev(T_S) / mean(T_S)
mean_error(S)      = | mean(T_S) − target(S) |
```

**Population standard deviation (divisor `N`) is specified deliberately** — sample-versus-population is exactly the kind of unstated convention that makes a host-side check and a firmware-side check disagree on a borderline wheel.

> **This does NOT make `J ≈ 1` mean "at tolerance."** An earlier revision claimed it did; §8.6 corrected that. J is a *mean* squared normalized residual used only for progress and stall detection — averaging can hide a single badly out-of-spec location. **Convergence is governed solely by the explicit per-channel tests of §8.6:** `max lateral ≤ tol_lateral`, `max radial ≤ tol_radial`, per-side non-uniformity ≤ `tol_tension_cv`, per-side mean error ≤ `tol_mean_tension_error`, and all contributing tension rows verification-grade (§8.6.3). **Not `tol_tension`, which is normalization only.**

**On target tension — one scale, per-side values derived.**

An asymmetric wheel operates at different tensions per side (§8.4 Part 2), so a naive scalar target is insufficient. But exposing `target_tension_side_a` and `target_tension_side_b` as independent runtime fields is **unsafe**, because the artifact's `n_mt` was identified against a specific operating tension distribution `T_target_assumed` (§§8.4 Part 1, 11.4):

> If the artifact identified `n_mt` for a Side A : Side B ratio of 1200 : 800, and runtime configuration is changed to 1000 : 1000, the operating ratio no longer matches the one `n_mt` was derived from — and the stored common-mode direction is silently invalid.

**Contract: `target_tension` is an overall SCALE — configurable between sessions, immutable within a session (§12.3.1). Per-side targets are derived from the distribution recorded in the artifact's `T_target_assumed`.**

**Normalization convention — MANDATORY, because a ratio alone has arbitrary scale.** `1.5:1`, `1.2:0.8`, and `0.6:0.4` express the same ratio but yield different absolute targets when multiplied out. One convention is fixed here so host and firmware cannot interpret the artifact differently:

> **`T_target_assumed` is stored normalized so its mean across spokes is exactly 1.0.**

Consequently:

```
target[i] = target_tension × T_target_assumed_normalized[i]
```

and **`target_tension` means the desired overall MEAN spoke tension** — a directly interpretable engineering quantity.

For a symmetric wheel the normalized distribution is `[1, …, 1]`, so this reduces exactly to the scalar case and one representation covers both. **The artifact records the normalization convention explicitly** (§11.4); a generator emitting an un-normalized distribution is a bug, not a variant.

**This makes the unsafe state unrepresentable** rather than merely discouraged — the operator can change how tight the wheel is, but cannot change the side ratio, which is a geometric property of the hub and not a tuning knob. **Changing the ratio requires artifact regeneration and re-identification of `n_mt`.**

**The ratio itself must come from validated geometry or empirical characterization — never an invented default** (§16).

**`adjustment_deadband`:** adjustments too small to perform reliably are skipped when the wheel state at that spoke is already within tolerance.

### 11.3 Acoustic chain profile

A **versioned** artifact describing the acquisition chain:

```
chain_id
transducer type
sample rate, bit depth
noise floor characterization
preflight thresholds
f1 band limits
window_ms, gate_start_ms
```

**Rationale:** noise characteristics are specific to a transducer + interface + environment combination. Thresholds tuned for one chain do not transfer to another.

**`chain_profile_id` is recorded in the session header only (§6.2), not per measurement.** The acoustic chain is fixed hardware — transducer, interface, gain — and cannot change mid-session. Duplicating a session-constant field onto every measurement adds no traceability. If a future configuration ever allows the chain to change within a session, promote it to per-measurement provenance at that point and state why.

### 11.3.1 Tension model profile

**`model_name` alone is insufficient provenance.** Two `TensionEstimate`s carrying the same `model_name` can mean materially different things if their parameters differ. The physics model needs its own versioned artifact:

```
tension_model_profile
    profile_id              (unique; referenced by the session header)
    model_name              (which candidate — §9.2)
    model_version
    L_eff                   (effective vibrating length — research-derived
                             parameter, §9.5; NOT an operator-entered value)
    L_eff_provenance        (how it was established, or "unestablished")
    linear_density          (μ, kg/m — with the material/gauge assumptions used)
    stiffness_parameters    (EI or equivalent, for stiffness-corrected models)
    empirical_coefficients  (for empirical models)
```

**General rule: every parameter REQUIRED BY THE SELECTED MODEL must be established.** Not every model uses every field — an empirical model may not need `L_eff` at all, and an ideal-string model needs no stiffness parameters. The profile declares which fields its model requires.

**No defaults, ever.** If a required parameter is unestablished, the profile is incomplete and tension estimation MUST report `unavailable` with `CALIBRATION_MISSING` — never substitute a nominal free-span `L_eff` or an assumed density (§9.5).

**Profiles are selectable BETWEEN sessions; the wheel binds compatibility, not identity.**

> An earlier revision put `expected_tension_model_profile_id` in the wheel configuration *and* made `tension_model_profile_id` session-selectable. Those cannot both hold — a wheel permanently expecting profile A makes profile B unselectable, so the profile was not actually session-selectable at all.

**Resolution: the wheel configuration records `compatible_model_assumptions`, not an approved profile ID.** Selecting a profile validates that its assumptions (spoke gauge, material, geometry) agree with the configured wheel; any profile that agrees is legal.

**Loading and validation happen before `START_TRUING`:** the profile is loaded, its required-field set is checked complete (below), and its assumptions are checked against `compatible_model_assumptions`. **Changing profile between sessions is legal and expected** — comparing candidate tension models on the same wheel is a stated research goal (§9.2). Within a session it is immutable (§12.3.1).

**Normal session admission:** if the selected tension-model profile is incomplete or incompatible, `START_TRUING` MUST be refused with `CALIBRATION_MISSING` or the applicable configuration error. Do not deliberately begin a normal session with a known-incomplete profile merely to collect `n_spokes` unavailable estimates. `TensionEstimate.status = unavailable` with `CALIBRATION_MISSING` remains required defensive behaviour if the acoustic API is invoked despite normal admission having been bypassed.

### 11.4 Influence matrix artifact

```
schema_version
artifact_id                 (unique identifier for THIS artifact)
generating_fingerprint      (PROVENANCE: hash over all parameters that
                             SHOULD have generated this artifact)
content_hash                (INTEGRITY: hash over the stored coefficients
                             and pseudoinverses themselves)
source                      (most provisional of the per-block values)
source_u, source_v, source_t   {literature_seeded, analytical,
                                coarse_measured, full_measured}  — §8.10.1
n_fit_samples_lat           (fitting observations, lateral — §8.9)
n_fit_samples_rad           (fitting observations, radial)
generating_parameters       (full wheel spec used to produce it)
generator_version           (tool + version, e.g. bike-wheel-calc commit)
units                       (explicit; see §6.4)
sign_conventions            (explicit; see §6.4)
indexing_origin             (see §6.5)
n_rim_angles, N_lat, N_rad
Phi_u, Phi_v, Phi_t         (compact Fourier form; per-spoke/per-class if asymmetric)
Phi_t_class_map             (explicit: index → (side, lead/trail))
common_mode_direction       (n_mt — §8.4 Part 1, satisfies BOTH conditions)
    identified              (bool — false ⇒ mean-tension targeting refused)
    vector                  (n_mt itself; MAY be null when identified=false)
    normalization           ("unit L2 norm")
    sign_convention         ("positive component sum" — fixes the ± ambiguity)
    cond_i_residual         (‖[Φ_u;Φ_v]·n_mt‖ — displacement invariance)
    cond_i_tolerance        (threshold applied)
    cond_ii_tolerance       (threshold applied to cond_ii_residual)
    uniqueness_margin       (separation from the next candidate direction
                             inside the displacement null space)
    uniqueness_threshold    (margin required to call it unique)
    T_target_assumed        (operating per-spoke tension distribution used
                             for condition (ii); runtime per-side targets are
                             derived from it — §11.2)
    T_target_normalization  ("mean=1.0" — MANDATORY convention, §11.2;
                             any other value is an invalid artifact)
    condition_ii_residual   (agreement of Phi_t·n_mt with T_target_assumed)
    displacement_null_dim   (dim of null([Phi_u; Phi_v]))
weights_used                (tolerances + dimensionless trust weights)
c_side_a, c_side_b          (N/rev, as used)
layouts[]                   (per §8.7.1, each with:)
    layout_id               (FULL | TENSION_ABSENT | ...)
    row_mask                (which rows this Phi_dagger was computed for)
    Phi_pseudoinverse
    effective_condition_number
                            (exactly effective_cond(L) from §8.11 R3;
                             computed host-side and checked at load; never the
                             ordinary full-matrix condition number for a
                             rank-deficient layout)
    effective_rank          (measured, not assumed — §8.5)
    expected_null_dim       (TENSION_ABSENT: expected; FULL: normally 0)
```

**`Phi_t_class_map` is mandatory, not optional.** The four tension classes must be mapped explicitly to physical (side, lead/trail) pairs. Relying on an implicit numbered convention shared between generator and consumer is exactly the kind of assumption that survives review and fails silently — a class permutation produces a well-formed matrix that assigns the wrong influence function to every spoke.

**`identified` must be REPRODUCIBLE, not just asserted.** The boolean gates whether mean-tension behaviour is permitted at all, so the generator must not be free to interpret "unique enough" differently across implementations or versions. Every residual, every threshold applied, and the uniqueness margin are recorded. However, the numerical identification procedure itself remains an explicit open model-preparation item (§8.4 Part 1); until an exact justified procedure is documented, the generator may emit `identified=false` and MUST NOT invent candidate-selection or uniqueness logic.

**When `identified = false`, `vector`, `T_target_assumed`, and the residual fields MAY be null or absent.** Do not fabricate placeholder values to satisfy serialization — a null here is the honest representation of "not established," and a placeholder is indistinguishable from a real result.

**`common_mode_direction` is mandatory.** It is what makes shape truing correct on an asymmetric wheel (§8.4 Part 1), and it must satisfy **both** conditions — displacement invariance *and* preservation of the operating tension distribution. The displacement null space alone does not determine it, and condition (ii) is **not** "equal tension change in every spoke" (§8.4 Part 1). `T_target_assumed` records the distribution used, because that assumption is exactly what makes the result reproducible — and the side-to-side ratio it encodes is itself an open item (§16). For a symmetric wheel it must come out as `[1,…,1]/√n_spokes`; anything else for a symmetric configuration is a generator bug. If `identified` is false, the solver refuses mean-tension targeting rather than substituting the arithmetic mean.

**Every `Phi_pseudoinverse` is bound to its `row_mask`.** The solver MUST verify the `active_row_set` exactly matches the mask before use (§8.7.1). There is no valid path by which a pseudoinverse is applied to a different row set.

**`n_rim_angles` is a model-preparation parameter, NOT a runtime-variable one.** Compact Fourier storage means the influence *curves* can be expanded onto any angular grid — but every shipped Φ† is computed for one specific grid and row layout. **Changing `n_rim_angles` (or `n_spokes`, `N_lat`, `N_rad`, weights, or tolerances) invalidates every layout and requires regenerating the artifact host-side.** The firmware verifies the artifact's recorded values against active configuration at load and aborts with `ARTIFACT_INVALID` on mismatch (§8.7). "Genuine parameter" means *configurable at model prep*, not *changeable at runtime*.

**Storage format:** store the **compact generative form** — Fourier coefficient sets — rather than expanded matrices. Expansion happens at initialization.

**The number and organization of coefficient sets follows the generated geometry, not a fixed template.** An earlier revision specified "one lateral curve, one radial curve, four tension class curves," which silently reimposes the symmetric-wheel compression corrected in §8.8. The schema MUST be able to represent per-spoke or per-class displacement influence functions:

| Configuration | Displacement sets | Tension sets |
|---|---|---|
| `asymmetric: false` | **MAY** collapse to 1 lateral + 1 radial rotated per column, **once rotational equivalence has been verified** on the generated columns; per-class or per-spoke storage is equally valid | 4 classes, 2 mirror pairs |
| `asymmetric: true` | **Per class or per spoke, as generated** | 4 independent classes |

**Single-curve storage for symmetric wheels is an optimization, not the required schema.** §8.8 treats rotational equivalence as something the generated columns *demonstrate*, not something the generator assumes — so collapsing to one curve before verifying equivalence would reinstate the assumption by the back door. The schema MUST permit per-class and per-spoke storage in every configuration.

The artifact records how many sets exist and how columns map onto them. Compactness is preserved either way — a per-class asymmetric artifact is a small constant factor larger, not an expanded matrix.

### 11.5 Persistence

| Data | Location | Survives reboot |
|---|---|---|
| Wheel class config | ESP32 NVS | Yes |
| Solver config | ESP32 NVS | Yes |
| Chain profile | ESP32 NVS | Yes |
| Influence matrix artifact | ESP32 flash | Yes |
| Tension model profile (§11.3.1) | ESP32 flash | Yes |
| Machine profile — station geometry (§11.6) | ESP32 NVS | Yes |
| Session logs | Host | Host-side |
| Current wheel state | RAM | No |

---

### 11.6 Machine profile — station geometry

A **versioned** artifact describing the physical machine the wheel is mounted in (§10A.3):

```
machine_profile
    profile_id                      (unique; recorded in the session header)
    stations[]                      one entry per physical station:
        station_id                  {acoustic, runout, adjustment, reference}
        present                     (bool — stations may be absent or coincide)
        angle_rad                   machine-frame angle at which the station sits, [0, 2π)
        positioning_tolerance_rad   C3 acceptable feature-to-station error (OPEN, hardware)
    reference_station               where the wheel reference (spoke 0) is established (§6.5)
```

- `acoustic`, `runout` and `adjustment` MUST be declared (they may share an angle); `reference` is present only if the machine has a separate index/reference sensor, otherwise `reference_station` names one of the others.
- Loaded and validated before `START_TRUING`; `profile_id` is recorded in the session header (§6.2).
- Like the chain profile (§11.3), its contents are profile-lifecycle data: **not individually mutable through `SET_PARAMETER`** (§12.3.1). A relocated station is a new profile, loaded between sessions.
- Nothing about the wheel drive (roller diameter, steps per revolution, gearing) belongs here; that is the automated navigation implementation's own calibration (§10A.4) and is a Stage 2 artifact.

---

## §12 Communication and Observability

### 12.1 Transport

**Primary: WiFi in AP mode.** The ESP32 hosts its own network and serves a web UI. The host connects with a browser — no host-side program to install.

**Fallback: USB CDC** with a host-side bridge serving the same UI.

**The fallback is cheap because the UI is a web application talking to a JSON/websocket endpoint either way.** Only the transport changes; the frontend does not.

**Debug channel: separate port where the board provides one.**

On the **DevKitC-1 development configuration**, the dual USB ports let debug (PlatformIO serial monitor) run without contending with the UI transport. **This is a property of the development board, not a permanent architectural guarantee** — the Nano ESP32 target has native USB only and cannot preserve the physical separation (§4.2). The architectural requirement is that debug is a *logically distinct channel* from operator intents (§12.5); which physical port carries it is a board-profile concern. On a single-USB target, debug shares the link or moves to UART-over-GPIO.

### 12.2 Telemetry — one-way, best-effort

**Telemetry is observational, not functional.** The system's correctness does not depend on it.

Requirements:
- **Never blocks the control path.** Non-blocking writes into a ring buffer, drained by a low-priority task, drop-on-full.
- **Event stream mirroring the state machine** — every state transition and every measurement result emits an event. The UI is a state mirror, not a data processor.
- **No delivery guarantees, no reconnect recovery protocol, no event replay, no host-side reconstruction of firmware state.** The lightweight current-state query defined below is required and does not violate this — it reports present state, it does not recover past state.

**Host-disconnect requirement — precisely scoped:**

**Autonomous operations do not depend on the host; operator-input steps do.** An earlier revision claimed no control-path operation depends on the host, which predates the Stage 1 measurement-phase operator waits (§7.3).

| Host-INDEPENDENT (completes with host absent) | Host-DEPENDENT in Stage 1 |
|---|---|
| Acoustic excitation, capture, DSP, estimation once started | Manual wheel positioning (`POSITION`) |
| Solver computation | Manual runout entry (`READ_RUNOUT`) |
| Verification computation | Adjustment confirmation |
| Autonomous state transitions *between* operator waits | |

**If the host disconnects while an autonomous operation is in flight, that operation completes normally.** The machine then advances to the next `WAIT_FOR_OPERATOR` and waits. **A Stage 1 measurement cycle cannot run to completion with the host absent** — it will stop at the first manual positioning or runout-entry wait.

**What IS affected by disconnection:**
- Telemetry stops. Events emitted during the gap are lost (best-effort, by design).
- The UI goes blank until reconnection.
- Host-side session logging has a gap.
- **Operator input becomes unavailable.** In Stage 1 the host is the only input source (§12.4), so the system will run to the next `WAIT_FOR_OPERATOR` state and wait there until an input source reconnects.

**Waiting is correct behaviour, not a failure.** No *event replay* and no reconstruction of firmware state from the host side are required — the ESP32 remains authoritative throughout.

**But one lightweight synchronization IS required, and an earlier revision wrongly claimed zero.** Because operator commands must echo the active `wait_id` (§7.3), and the event that announced that wait may have been lost during the gap, a reconnecting UI cannot otherwise learn what to send.

**Required: a current-state query.** On connect or reconnect, the UI issues one request and the firmware answers with a snapshot of *present* state:

```
current_state           (state machine state)
active_wait             (null if not waiting, else:)
    wait_id
    prompt                  (a positioning prompt names the wheel feature AND its station, §10A)
    expected_intent
cycle_index
last_known_result       (optional, for display continuity)
```

**Separate query: `GET_CURRENT_CYCLE_PROVENANCE`.** P6 (§3) requires the authoritative material explaining the *current* adjustment to be queryable, and the state snapshot above does not carry it. This second query returns the ESP32's in-RAM authoritative record:

```
influence_artifact_id, generating_fingerprint
tension_model_profile_id
chain_profile_id
active_layout, active_row_set
weights and tolerances applied
per-measurement status + reason codes contributing to this cycle
current WheelState summary
current AdjustmentPlan (per-spoke turns, suspect-row count, policy reasons)
wheel position authority snapshot (rotation state, reference established,
                                   operator/sensor confirmation — §10A) and machine_profile_id
```

**This is present-state inspection, not event replay.** Telemetry stays best-effort and may lose history (§12.2); what may not be lost is the material needed to explain the adjustment currently on offer.

**This is a snapshot, not a replay.** It carries current state only — no event history, no missed telemetry, no reconstruction. It is idempotent and safe to issue at any time. Without it, a reconnecting operator holds no valid `wait_id` and cannot confirm anything, which would strand the machine at a wait indefinitely.

**Demonstration property, stated precisely:** disconnect the host **during an autonomous acoustic acquisition/DSP operation** and observe that operation complete. That is direct evidence the ESP32 performs the work. Do not stage the demonstration across a manual-positioning or runout-entry wait, where stopping is the correct behaviour. In Stage 2 an on-device input source removes the residual dependency entirely.

### 12.3 Commands — admissible states are per intent

> **An earlier revision stated that commands are only acted upon at `WAIT_FOR_OPERATOR`. That is wrong for at least two intents** — `START_TRUING` is accepted in `READY`, and an `ABORT` that only works while the machine is already stopped is useless. Admissibility is defined **per intent**, not by one global rule.

| Intent | Admissible states | Notes |
|---|---|---|
| `START_TRUING` | `READY` only | Rejected elsewhere |
| `SET_PARAMETER` | Admissibility depends on **both** state and parameter class — see §12.3.1 | `READY`: session-mutable **and** session-fixed. `WAIT_FOR_OPERATOR`: session-mutable only. Autonomous states: none. Artifact-bound: always refused. |
| `CONFIRM_POSITIONED` | The matching positioning wait (§7.3) | Operator has rotated to the requested spoke/index |
| `SUBMIT_RUNOUT(lateral, radial)` | The matching runout wait (§7.3) | Manual `read_snapshot()` values for the current rim index |
| `CONFIRM_ADJUSTMENT_DONE` | The matching adjustment wait | Operator has applied the displayed turns |
| *(all confirmations above)* | The **matching** `WAIT_FOR_OPERATOR` instance only | Must carry the active `wait_id` |
| `ABORT` | **All operational states** | Safety intent — see below |
| Debug intents | Per §12.5, development only | Never in the demo script |

**Confirmations are the case the original rule was written for**, and for them it still holds: they are accepted only at the wait they answer, the system is already stopped there, and a slow or post-reconnect confirmation cannot corrupt an in-flight operation.

**`ABORT` is a safety intent and must be accepted whenever the machine is operating.** It is *accepted* immediately and *effected at the next safe point*, which the state machine defines per state:

| State class | Safe-point behaviour |
|---|---|
| Measurement (`MEASURE_SPOKE_TENSION` active) | **Cancellation accepted immediately; the acoustic subsystem returns at its next internally defined safe point** (§7.4) and the incomplete measurement is discarded. The orchestrator gains no visibility of internal stages. |
| Solve / compute | Abort immediately; discard the result |
| `WAIT_FOR_OPERATOR` | Abort immediately |
| Actuation (Stage 2) | Bring the actuator to a controlled stop, then abort — never mid-motion |
| Wheel navigation in motion (Stage 2, §10A) | Bring the wheel drive to a controlled stop, then abort — never mid-motion; the wheel reference may need re-establishing afterwards |

**Wire-protocol requirement — `wait_id` (§7.3).** Any command answering a `WAIT_FOR_OPERATOR` MUST carry the `wait_id` from the prompt or from the current-state snapshot (§12.2). The firmware **rejects and logs** commands whose `wait_id` does not match the active wait, using `STALE_INTENT`. This is part of the wire contract, not an internal state-machine detail: a client that omits or fabricates `wait_id` must be rejected, not accommodated.

`ABORT` and `START_TRUING` carry no `wait_id` — they are not answers to a wait.

#### 12.3.1 Parameter mutability classes

**Not every configuration field can be changed at runtime, because some are baked into the influence-matrix artifact and every shipped pseudoinverse (§11.4).** Changing one while the old artifact stays loaded produces a configuration that disagrees with the solver's own matrices — internally consistent, silently wrong.

**Admissibility is a function of state AND class.** An earlier revision said `SET_PARAMETER` was accepted in `READY`/`WAIT_FOR_OPERATOR` "only for session-mutable parameters," then stated that session-fixed values are settable in `READY` — leaving no legal path to set them at all.

| State | Session-mutable | Session-fixed | Artifact-bound |
|---|---|---|---|
| `READY` (before `START_TRUING`) | ✔ accept | **✔ accept** | ✘ `REQUIRES_ARTIFACT_REGENERATION` |
| `WAIT_FOR_OPERATOR` (session active) | ✔ accept | ✘ reject — session in progress | ✘ `REQUIRES_ARTIFACT_REGENERATION` |
| Autonomous operation | ✘ reject | ✘ reject | ✘ reject |

**Three classes:**

| Class | Fields | `SET_PARAMETER` behaviour |
|---|---|---|
**Every runtime-selectable configuration or binding field belongs to exactly one class. No runtime-selectable field is unclassified.** Host-only model-preparation parameters and artifact-internal values are not addressable through `SET_PARAMETER`.

| Class | Fields | `SET_PARAMETER` behaviour |
|---|---|---|
| **Session-mutable** (cycle control only) | `max_cycles`, `measurement_retry_count`, `excitation_settle_ms`, `min_relative_improvement`, `consecutive_non_improving_cycles`, `partial_state_remeasure_attempts` | Accepted in admissible states |
| **Session-fixed** | `tension_model_profile_id`, `target_tension`, **`adjustment_deadband`**, **`lateral_deadband`**, **`max_adjustment_revolutions`** | `READY` before `START_TRUING` only; **immutable once a session begins** |
| **Artifact-bound firmware configuration/binding** | `n_spokes`, `n_cross`, `n_rim_angles`, `N_lat`, `N_rad`, `tol_lateral`, `tol_radial`, `tol_tension`, `tol_tension_cv`, `tol_mean_tension_error`, `tol_angular`, `max_condition_number`, `rank_tolerance`, `n_mt_displacement_tolerance`, `n_mt_tension_tolerance`, `n_mt_uniqueness_threshold`, `symmetry_angle_tolerance`, `trust_radial`, `trust_tension`, hub geometry, `asymmetric`, spoke geometry, `indexing_origin` | **Refuse** with `REQUIRES_ARTIFACT_REGENERATION` |

**Host-only values are outside the command surface.** Values such as `n_fit_samples_lat`, `n_fit_samples_rad`, and artifact-internal `T_target_assumed` remain artifact-bound provenance, but they are not firmware runtime parameters and MUST NOT be accepted as `SET_PARAMETER` targets. Changing one requires host-side artifact regeneration and reload, not a runtime command that attempts to mutate that value.

**Why plan-generation and safety values are session-fixed, not mutable.** `adjustment_deadband`, `lateral_deadband`, and `max_adjustment_revolutions` all shape a computed `AdjustmentPlan`. Changing one while a plan is pending strands that plan exactly as a mid-session `target_tension` change would — the operator could then confirm and apply a plan generated under different limits. **The Stage 1 rule is the simple one: plan-generation and safety parameters are session-fixed.** Supporting mid-session change would require explicit pending-plan invalidation and recomputation, which is deliberately not in scope.

**Machine-profile contents (§11.6) follow the profile-lifecycle rule**, exactly like the chain profile: station angles and the reference station are loaded and validated as a versioned profile before `START_TRUING` and are not addressable through `SET_PARAMETER`. Wheel-drive calibration (§10A.4) is internal to the automated navigation implementation and is likewise outside the command surface.

**Wheel-class and binding fields (§11.1) are ALL artifact-bound.** `rim_diameter`, hub geometry, spoke geometry and material, `asymmetric`, `indexing_origin`, `symmetry_angle_tolerance`, `c_side_a` / `c_side_b`, `expected_influence_fingerprint`, and `compatible_model_assumptions` every one enters the generating fingerprint or the artifact-admission decision (§11.4). Changing any of them at runtime would invalidate the loaded artifact.

**No runtime-selectable configuration or binding field is unclassified.** Every such field in §11.1–§11.3.1 belongs to exactly one row above. The internal contents of persisted chain and tension-model profiles are versioned artifact data loaded and validated through their profile lifecycle; they are not individually mutable through arbitrary `SET_PARAMETER` calls. Where a runtime field group is named rather than each field listed, the group is exhaustive for that section. An implementation encountering a runtime-selectable field it cannot classify must treat it as **artifact-bound** — the conservative default — and report the omission as a specification bug rather than making a judgment call.

**Why acceptance tolerances and numerical thresholds are artifact-bound.** `tol_*` values enter Φ̃ normalization and therefore every precomputed Φ† (§8.7.1). `max_condition_number`, `rank_tolerance`, `n_mt_displacement_tolerance`, `n_mt_tension_tolerance`, `n_mt_uniqueness_threshold`, and `symmetry_angle_tolerance` govern **artifact admission and `n_mt` identification** — changing them at runtime would change whether an already-loaded artifact should have been accepted.

**Selection is by PROFILE, not by model name.** The authoritative session-fixed field is `tension_model_profile_id` (§11.3.1). `model_name` is metadata *inside* the selected profile, and remains on each `TensionEstimate` as defensive provenance — it is not the selector.

**Session-fixed — the tension model must not change mid-run.** §9.2 calls it a *session-selected* model and the session header records one value. Allowing it to change mid-session would let measurements within a single run use different physics models while the session provenance claims a single one. **Changing the model requires ending the session and starting a new one.**

`TensionEstimate.model_name` is still recorded per measurement (§6.3) as defensive provenance — it catches a bug if this rule is ever violated — but mid-session switching is **not a supported capability** in Stage 1.

**Artifact-bound `SET_PARAMETER` MUST be refused, not applied.** The firmware must not update configuration while keeping the previously generated artifact loaded. Regeneration host-side and reload must both complete before `START_TRUING` is admissible again.

**Two entries that look like ordinary knobs and are not:**
- **Tolerances and trust weights** are artifact-bound — they enter Φ̃ and therefore every precomputed Φ† (§8.7.1).
- **The side tension ratio** is artifact-bound — `n_mt` was identified against it (§11.2). `target_tension` is configurable between sessions and immutable after `START_TRUING`.

**Session-fixed — `target_tension` too, and resetting J is not sufficient.** An earlier revision made `target_tension` session-mutable with a progress-baseline reset. That under-solves the problem: an `AdjustmentPlan` may already have been computed against the old target, and the operator could change the target while the machine waits and then confirm the **stale plan**. The plan is invalid, not merely the cost history.

**Stage 1 contract:** `target_tension` is settable in `READY` before `START_TRUING` and immutable thereafter. Changing it requires a new session and recomputation from the beginning.

If mid-session retargeting is ever supported, it requires **explicit invalidation of any pending `AdjustmentPlan` and recomputation before adjustment may proceed** — not just a J baseline reset.

### 12.4 Operator intent is an event, not a transport

**The state machine subscribes to operator intents. What produces them is a driver.**

| Source | Phase |
|---|---|
| Laptop web UI | Stage 1 |
| On-device touchscreen | Stage 2 |
| Physical buttons | Optional |

Adding the touchscreen is a new driver, not a state machine change. Touchscreen hardware is unspecified; this pattern absorbs that.

### 12.5 Debug channel scope

The host may issue **operator intents plus an explicitly-marked debug channel** (force state, inject values, dump internals). Development and bring-up need this.

**Convention: the Stage 1 demonstration script uses operator-intent commands only.** A demo flow that depends on a debug command is a flow the final machine cannot perform. Marking the debug channel explicitly makes this checkable.

### 12.6 Audio streaming — if ever implemented

Streaming raw audio or live spectra **during** capture is sustained high-bandwidth transmission concurrent with the sensitive operation. **If built, it goes on USB, not WiFi.**

Post-capture one-shot payloads (extracted frequency, candidate peaks, SNR, a single spectrum snapshot) are small, sent after the capture window closes, and are fine over any transport. This is the normal telemetry path.

---

## §13 Error Model

### 13.1 Failure response classes

| Class | Response |
|---|---|
| Transient measurement failure | Retry (bounded), then mark by cause (§7.4), **continue collection** |
| Single spoke unmeasurable | Mark with reason, **continue collection** — see below |
| Sensor unavailable | Mark `unavailable`, degrade if permitted (§8.5, §8.11 R4), else abort |
| **Unexpected** rank / conditioning / artifact-structure violation | **Abort loudly** — see below |
| Aliasing or dimension mismatch | **Abort loudly** (`ARTIFACT_INVALID`) |
| Missing/invalid model artifact | `ABORT_NO_MODEL` |
| Adjustment outside safe bounds | `ABORT_UNSAFE_ADJUSTMENT` |
| Host disconnect | In-flight operation completes; then see below |
| Cost non-decrease | `ABORT_NO_PROGRESS` |
| Wheel navigation fault / reference lost (§10A) | Stop wheel motion; refuse further positioning until the reference is re-established. Recorded measurements are untouched. Terminal handling of an unrecoverable fault is OPEN (§16) |

**"Continue" means continue COLLECTING — it does not imply the result is solvable.** Mark the measurement, finish the measurement pass, then construct the `active_row_set` and pass it through the §8.11 admission rules. Under the fixed-layout policy (§8.7.1):

| Situation | Outcome |
|---|---|
| One rejected runout row | Matches no shipped layout → **refuse** `PARTIAL_WHEEL_STATE` |
| One rejected tension row | Matches neither `FULL` nor `TENSION_ABSENT` → **refuse** |
| **All** tension rows rejected/absent | Legally selects `TENSION_ABSENT` → proceed, geometric-only semantics (§8.6.2) |

Continuing collection after a failure and *being able to solve* are different questions, decided at different stages.

**Rank deficiency is not automatically a violation.** `TENSION_ABSENT` is structurally rank-deficient **by design** (§8.5); its expected nullity is a normal property, not an error. What aborts is an **unexpected** structural result:

- measured effective rank disagrees with the artifact's recorded value;
- null-space dimension differs from the layout's expected value;
- condition number exceeds configured limits;
- `FULL` proves unusably rank-deficient or ill-conditioned;
- matrix dimensions or structural metadata do not match configuration.

**Host disconnect — precisely (§12.2):** any control-path operation in flight **completes normally**; telemetry and host-side logging may be lost; and when the machine reaches a state requiring operator input, **it waits** until an input source reconnects. It does not run blindly through operator-dependent states.

### 13.2 Two distinct enumerations

**Terminal/session result codes and measurement-level reason codes are SEPARATE types. Do not merge them.**

**Terminal result codes** — how a session ended, one per run:

```
CONVERGED                     CONVERGED_GEOMETRIC_ONLY
ABORT_NO_PROGRESS             ABORT_MAX_CYCLES
ABORT_UNSAFE_ADJUSTMENT       ABORT_NO_MODEL
ABORT_OPERATOR                ABORT_PARTIAL_STATE
```

**These are the §8.6.2 names, used verbatim.** An earlier revision introduced `COMPLETE` / `COMPLETE_GEOMETRIC_ONLY` as a second vocabulary for identical semantics — there is only one set, and it is this one. `CONVERGED_GEOMETRIC_ONLY` means geometry verified, but tension compliance was **NOT ESTABLISHED using verification-grade evidence**. It does not imply tension was absent, unmeasured, or unevaluated, and it MUST NOT be surfaced as generic successful truing.

**Reason codes** — why an individual measurement, command, or artifact operation was not `valid`, attached per record. An enumeration, not free text. Minimum set:

```
NO_ONSET_DETECTED       ONSET_COUNT_MISMATCH
LOW_SNR                 AMBIGUOUS_PEAK
FREQ_OUT_OF_RANGE       NO_F2_PARTNER
PROVISIONAL_MODE_ID     MODEL_REJECTED
SENSOR_TIMEOUT          VALUE_OUT_OF_RANGE
NOT_IMPLEMENTED         CALIBRATION_MISSING
STALE_MEASUREMENT       PARTIAL_WHEEL_STATE
ARTIFACT_INVALID        STALE_INTENT
REQUIRES_ARTIFACT_REGENERATION
MEAN_TENSION_MODEL_UNAVAILABLE
TENSION_NOT_VERIFICATION_GRADE
```

**`REQUIRES_ARTIFACT_REGENERATION`** refuses a `SET_PARAMETER` targeting an artifact-bound field (§12.3.1). The configuration is left unchanged; the caller must regenerate and reload the artifact host-side.

**`STALE_INTENT`** logs an operator intent discarded because its `wait_id` did not match the active wait (§7.3). It is a normal occurrence on a flaky link, not an error condition — but it must be logged, because silently dropped confirmations look identical to an operator who never confirmed.

**`PROVISIONAL_MODE_ID`** accompanies every `TensionEstimate` produced while layer 3 is an interim implementation (§4.4.1). It is not an error — it states accurately that the estimate's **selected frequency** came from an unvalidated rule and that its mode identity is `presumed_fundamental`, not established (§6.3).

**`TENSION_NOT_VERIFICATION_GRADE`** accompanies `CONVERGED_GEOMETRIC_ONLY` when tension measurements were solver-admissible but not verification-grade (§8.6.3).

**`MEAN_TENSION_MODEL_UNAVAILABLE`** is **non-terminal**: mean-tension targeting is refused, geometric truing continues (§8.4 Part 1).

### 13.3 Never do this

- **Never substitute a default value for a failed measurement.** See P2.
- **Never proceed past aliasing, dimension mismatch, unexpected rank or nullity, or conditioning outside configured limits.** These produce plausible wrong answers. **Expected structural nullity is not a violation** — `TENSION_ABSENT` is rank-deficient by design (§8.5), and a null space matching the artifact's recorded dimension is a normal property. What aborts is *disagreement with the artifact metadata*, not deficiency itself.
- **Never let telemetry failure affect control flow.**

---

## §14 Testing and Verification

### 14.1 Levels

| Level | Scope |
|---|---|
| Unit | Individual algorithms, framework-free core |
| Integration | Interface contracts between subsystems |
| Simulation | Fake/recorded implementations of hardware interfaces |
| Replay | Recorded measurement data through current algorithms |
| Hardware-in-loop | Real hardware, controlled software |
| End-to-end | Full Stage 1 workflow |

### 14.2 Golden fixtures — required

**Every core algorithm has recorded inputs and expected outputs, checked into the repository.**

This is what makes the eventual host→firmware port verifiable rather than a leap of faith: the C implementation must reproduce the Python implementation's output on identical inputs within tolerance.

Recorded acoustic measurement campaigns in the acoustic repo serve this role for the DSP path. Copy the fixtures in (§1.2); do not reference them across repos.

### 14.3 Truing subsystem — heavier regime

Because this subsystem fails silently (§8), it requires verification the others do not:

**1. Synthetic round-trip (primary correctness gate) — DEFINED PER LAYOUT.** Generate a random adjustment vector **d_applied**; compute the resulting wheel state via Eq. 2; add representative noise; solve. **Requires no hardware.** Runs in CI.

> **What "recovery" means depends on the layout's rank, and an earlier revision got this wrong.** It required recovering an arbitrary **d_applied** for every layout. That is invalid for a rank-deficient layout: any component of **d_applied** lying in the displacement null space is **unobservable**, and the pseudoinverse correctly returns the minimum-norm identifiable solution rather than the original vector. The old test would fail a correct implementation.

| Layout | Assertion |
|---|---|
| `FULL` (when the generated artifact has full effective column rank) | Recover **d_applied** within tolerance and noise expectations |
| `TENSION_ABSENT` and any rank-deficient layout | Compare against the **row-space (minimum-norm) projection** of **d_applied** — never against its unobservable null-space component |

**Sign convention:**

```
synthetic state:      Y = Y_b + Φ · d_applied      (the DISTURBANCE)
solve reconstructs:   d_ls ≈ d_applied             (the disturbance, recovered)
```

Asserting that the corrective command equals the recovered disturbance is a sign error this test exists to catch.

> **`d_adj ≈ −d_applied` is NOT a general promise of the two-part solver, and asserting it universally would fail a correct implementation.** The algorithm recovers `d_ls`, **removes** the `n_mt` component for the shape solve, and constructs `d_cm` **independently** from `c` and the target. The removed component returns only if Part 2 happens to restore exactly what Part 1 stripped — which is true only for synthetic cases deliberately built that way.

**Four separate tests, not one:**

| Test | Assertion |
|---|---|
| **1a. LS inversion** | `FULL` / full effective rank: `d_ls ≈ d_applied`. Rank-deficient layout: `d_ls ≈` row-space (minimum-norm) projection of `d_applied` — never its unobservable null-space component. |
| **1b. Shape solve** | Commanded shape correction ≈ the negative disturbance **with the `n_mt` component removed**. For `TENSION_ABSENT`: the minimum-norm displacement solution, **no projection applied** (§8.4 Part 1 fallback). |
| **1c. Mean-tension (Part 2)** | `d_cm` tested **independently** against known `target_tension` and `c` values, symmetric case only. Asymmetric remains refused until validated (§8.4 Part 2). |
| **1d. Combined end-to-end** | `d_adj ≈ −d_applied` **only** for synthetic cases explicitly constructed so that Part 2 restores what Part 1 removed. Not a general assertion. |

**2. Structural property tests.** These check the properties the math depends on, not just outputs:
   - **General:** when `n_mt` is identified, `[Φ_u ; Φ_v] · n_mt ≈ 0` within configured tolerance.
   - **Symmetric wheels only (`asymmetric: false`):** `n_mt ≈ [1,…,1]/√n_spokes`, and therefore a common-mode (equal) adjustment produces no displacement change. **Do NOT assert that equal spoke-tension increments are displacement-invariant on an asymmetric wheel** — that is the assumption removed in §8.4 Part 1, and re-asserting it here would reintroduce it through the test suite.
   - `TENSION_ABSENT` exhibits its expected structural null space, of the dimension recorded in the artifact; `FULL` is checked against its **measured** artifact rank and conditioning, with **no universal deficiency assumption** (§8.5).
   - Φ_t class structure: `circshift(Φ_t[:,i], 4) ≈ Φ_t[:,i+4]` and `circshift(Φ_t[:,i], 1) ≉ Φ_t[:,i+1]` (§8.8 — both directions required).
   - For a symmetric wheel, the mirror relationships hold.
   - The sampling assertion (§8.9) fires when violated.
   - **Column localization (§6.5):** for every spoke *i*, the extremum of the detrended lateral influence column falls within `tol_angular` (circular distance) of spoke *i*'s rim angle.

**3. Sign and unit convention test (§6.4.2).** Must fail if any coordinate flip is omitted or double-applied. Magnitude-only assertions do not catch this.

**The assertions MUST be the physical ground-truth statements in §6.4.2**, not observations of the generator's output. A test derived from the generator's own behaviour passes with the sign inverted and permanently conceals the error. Specifically: positive tightening → negative radial displacement at that spoke; positive tightening of a Side A spoke → negative lateral displacement at that spoke; positive tightening → positive tension change in that spoke.

**4. Reference reproduction.** The reference work publishes before/after wheel states for real experiments. Loading a corresponding Φ and reproducing those published results is external validation available nowhere else in the project.

**5. Host/firmware numerical parity.** Shared fixtures, both implementations, tolerance-bounded comparison.

### 14.4 Acoustic subsystem

- Replay recorded campaign WAVs; verify extracted frequencies match expected.
- **Capture-under-WiFi-load stress test (§9.4)** — mandatory before the subsystem is considered complete.
- Verify format enforcement: a source delivering the wrong sample rate is rejected, not silently resampled.

### 14.5 Simulation requirement

**Every hardware-touching interface has at least one non-hardware implementation.** The full Stage 1 workflow must be runnable end-to-end with zero hardware attached. This is required for CI, for AI-assisted development, and for debugging. This includes Wheel Navigation / Positioning (§10A): a synthetic automated implementation with deterministic wheel position and scriptable slip/faults exists alongside the manual implementation, and the wheel-drive interface has a fake.

---

## §15 Development Phases

Complexity ratings indicate **coding difficulty only** — not research difficulty or physical build effort. Use them to allocate appropriate models and review depth.

**L1** = boilerplate · **L2** = normal application logic · **L3** = subtle domain logic or nontrivial algorithms · **L4** = hard real-time / embedded / numerical

| Phase | Scope | Complexity | Depends on |
|---|---|---|---|
| **1a** | Scaffolding: interfaces, data model, config, status/provenance, stubs, `platformio.ini` both envs; Wheel Navigation contract with manual + synthetic implementations, wheel-drive contract with stub/fake, machine profile (§10A, §11.6) | **L1–L2** | — |
| **1b** | Host-side model prep: `bike-wheel-calc` wrapper, artifact generation, pseudo-inverse per layout, **§14.3 host-side gate** | **L3–L4** | 1a |
| **1c** | Truing calculation (firmware): artifact loading, matrix-vector application, two-part solve, cost function. **Gate includes host/firmware truing numerical parity** on shared fixtures | **L1–L2** | 1a, 1b |
| **1d** | Orchestrator: state machine, both loops, operator-wait states; `POSITION` through the navigation contract by outcome only (§10A) | **L2** | 1a |
| **1e** | Comms: telemetry event stream, command handling, web UI | **L2** | 1a |
| **1f** | Acoustic subsystem: four seams; I2S front end; **port layers 2 and 4 to C**; interim layer 3 | **L3–L4** | 1a |
| **1g** | Runout: manual entry implementation, interface | **L1** | 1a |
| **1h** | End-to-end Stage 1 integration | **L2** | all above |
| **2** | Audio capture hardening: DMA sizing, task/deadline separation, WiFi-load stress test | **L4** | 1f |
| **3** | Layer 3 (mode ID) replacement when research settles, plus the acoustic regression/parity testing that replacement requires | **L3–L4** | 2 |
| **4a** | Automated runout sensing | **L3** | hardware selection |
| **4n** | Automated wheel navigation (§10A): wheel-drive development driver (expected first: TMC2209, behind the wheel-drive HAL), index/reference sensing, actuator-to-wheel calibration, slip / re-index policy, positioning tolerance, station-geometry validation on the real frame | **L3–L4** | hardware selection; 1d *(lettered "4n" to avoid renumbering)* |
| **4b** | Actuation + closed-loop lateral feedback | **L4** | 4a, 4n |
| **4c** | On-device UI, full standalone | **L2–L3** | 4b |

**Phase gate rule — dependency-ordered, not numerically ordered.**

> **Each phase must pass its gate before any phase that DEPENDS ON IT begins.** The dependency column above is authoritative. Phases 1b–1g depend only on 1a and may proceed in parallel; an unresolved research-dependent phase (e.g. Phase 3) must not block unrelated work whose own dependencies are satisfied.

**Phase 1b's gate is the host-side subset of §14.3, not all of it.** §14.3 includes host/firmware numerical parity, and firmware truing does not exist until 1c — so requiring 1b to pass it is impossible. Split:

| Gate | When | Contents |
|---|---|---|
| **1b host gate** | Before 1c consumes the artifact | Artifact generation and schema validation · synthetic host-side solve (per layout, §14.3) · structural Φ tests · rank / conditioning / null-space tests · sign and unit tests · reference reproduction where available |
| **Truing parity gate** | **Part of 1c's gate**, as soon as firmware truing exists | Host/firmware numerical parity on shared golden fixtures |

**1c MUST NOT consume an artifact that failed the 1b host gate.** But parity cannot precede 1c, and the phase rule must not pretend otherwise.

#### Phase 1h is not the Stage 1 release gate

| Milestone | Meaning |
|---|---|
| **Phase 1h complete** | First integrated end-to-end Stage 1 system: all subsystems wired, workflow runs |
| **Stage 1 release/demo gate** | **Phase 1h PLUS the Phase 2 acoustic hardening**: DMA sizing, deadline separation, and the mandatory WiFi-load capture stress test (§9.4, §14.4) |

§14.4 makes the stress test a precondition for considering the acoustic subsystem complete, so **an integrated system that has not passed it is not demonstration-ready.** Phase numbering and dependency order are unchanged; this is stated only to prevent reading "1h complete" as "Stage 1 complete."

**Truing parity belongs to 1c, NOT to Phase 3.** It is the protection for the initial firmware port and must run as soon as that port exists. An earlier revision listed it alongside Phase 3's mode-ID replacement, which would have made verification of the truing port wait on unrelated, unresolved acoustic research. Phase 3 carries only the acoustic regression testing that a layer-3 replacement itself requires.

**Note the inversion:** the project's most important algorithm (truing) has one of its *lowest* firmware complexity ratings, because the pseudo-inverse moved offline (§8.7). The difficulty is concentrated in host-side Python (1b), where it is testable.

---

## §16 Deliberately Open

These are **intentionally unspecified**. Do not fill them with invented implementations. If work requires one of these to be resolved, ask.

| Item | Why open | What is specified |
|---|---|---|
| Acoustic mode-identification rule | Active research | The seam (§9.2 layer 3) |
| Effective vibrating length (`L_eff`) | Research output; differs from nominal free span in a laced wheel | It is a **research-derived model parameter and provenance value** once established — **not** a config constant for an operator or agent to fill in (§9.5). It MUST NOT receive an invented default or an arbitrary entered value merely to make the model run. A wrong value silently corrupts every tension estimate. |
| Default tension model | Multiple candidates, none validated | Model selection as config; `model_name` on every estimate |
| DSP window/gate constants | Research output | That they are config, not literals |
| Damping mechanism | May prove unnecessary | Owned internally by acoustic subsystem either way |
| Runout sensor hardware | Not selected | Snapshot interface; streaming contract |
| Streaming sample rate feasibility | Hardware-dependent | That it gates Stage 2 actuation |
| Touchscreen hardware | Not selected | Operator intent as event; driver swappable |
| Rim cross-section properties | Not measured | Estimated for C2, recorded in artifact |
| Analytical vs. measured Φ accuracy | Unvalidated by design | `source` field + generating parameters enable later comparison |
| Cycles to convergence | Empirical, per wheel | `max_cycles` config, abort conditions |
| Convergence thresholds (§7.5.1) | J's noise floor unmeasured | Structure specified; values config; derive from measured repeatability |
| Closed-loop lateral deadband (§7.6) | Depends on unselected runout sensor | That it is required config, distinct from `adjustment_deadband` |
| Unreachable-target behaviour (§7.6) | Not addressed by the reference work | Resolve with actuation hardware, not beforehand |
| Dished-wheel method validity | Reference validates symmetric only | Per-side `c`; four independent Φ_t curves |
| Asymmetric mean-tension compensation (§8.4 Part 2) | Not derived in the reference | `d_cm` stays vector-valued. Until the asymmetric compensation is **validated**, requested mean-tension targeting MUST be **REFUSED** — a warning or confidence flag is not authorization to execute an unvalidated compensation. Shape-only / `TENSION_ABSENT` geometric truing remains available where §8.11 permits it. |
| Side-to-side target tension ratio (§8.4) | Follows from bracing geometry in principle; not measured | Stored in the artifact as the normalized `T_target_assumed` distribution (mean = 1.0) once established, and **artifact-bound**. Runtime exposes only a session-fixed overall `target_tension` scale; per-side targets are derived (§11.2). **Independently tunable per-side ratios are NOT exposed as runtime knobs.** If the distribution cannot be established, mean-tension targeting is refused. |
| Common-mode direction, asymmetric (§8.4 Part 1) | Existence/uniqueness of `n_mt` not established | Both defining conditions specified; refuse mean-tension targeting if not identified |
| General on-device reduced solve (§8.7.1) | Timing, memory, numerical behaviour unmeasured | Fixed layouts for C2; evaluation named as a deferred task |
| Wheel-drive mechanism, motor and motor driver (§10A.6) | Not selected; rim-contact roller + stepper is the leading concept, TMC2209 the expected *development* driver only | The navigation contract and the actuator-coordinate wheel-drive boundary; replaceability without touching higher layers |
| Station geometry of the final frame (§10A.3, §11.6) | Frame not finalized; stations may move, merge or be added | Machine profile as data; consumers never compile offsets in; prompts/requests name feature + station |
| Actuator-to-wheel calibration, odometry, index/encoder sensing and placement, positioning tolerance, slip detection / re-index policy, motion-control law, unrecoverable-fault terminal handling (§10A.4, §10A.5, §10A.7) | Hardware-dependent; unmeasured | That they belong inside the automated navigation implementation, are validated physically, and are refused (`CALIBRATION_MISSING`) rather than guessed |

---

## §17 Agent Instructions

### 17.1 Before writing code

1. **Read the acoustic repo** for context on the DSP pipeline, data formats, and current research state. Note its `AI_guide.md` governs that repo only (§1.2).
2. **Verify `bike-wheel-calc` runs** (§8.10) before building the model-prep path on it. Report if it does not.
3. **Confirm the PlatformIO + ESP-IDF toolchain builds** both environments with a trivial program.

### 17.2 When this document is silent

- If it is in §16, **ask** — do not invent.
- If it is a trivial implementation detail with no downstream consequence, **decide and note the decision.**
- If it would materially change how something is implemented, **ask.**

### 17.3 Highest-risk items — re-read before implementing

These are the places where a reasonable default produces a silent, plausible, wrong result:

1. **Pseudoinverse ↔ row set binding** (§8.7.1) — never apply Φ† to a row set it wasn't computed for
2. **Common-mode projection** (§8.4 Part 1) — arithmetic mean is symmetric-only
3. **Φ_u/Φ_v single-curve reduction** (§8.8) — symmetric-only; generate per spoke/class otherwise
4. **Φ_t generation** (§8.8) — different generator than Φ_u/Φ_v; four classes, mirroring symmetric-only
5. **Weighting** (§8.3) — dimensionless, tolerance-normalized; reference numbers NOT transferable
6. **Two-part solve** (§8.4) — separation mandatory, backed by a documented negative result
7. **`d_cm` is a vector** (§8.4) — scalar is symmetric-only; asymmetric derivation is OPEN
8. **Tension-absent ≠ zeroed residuals** (§8.11 R4) — requires its own shipped layout
9. **Convergence is per-channel, not J** (§8.6) — J is stall detection only, and must be row-count normalized
10. **Sign conventions** (§6.4) — the radial flip between `bike-wheel-calc` and this system
11. **Indexing origin** (§6.5) — an offset shifts every matrix column
12. **Matrix dimensions** (§8.7) — derive from config; a mismatch aborts, never reshapes
13. **`Phi_t_class_map`** (§11.4) — explicit, never implicit
14. **Sampling floor** (§8.9) — applies at generation, not runtime; above it is necessary, not sufficient
15. **Retry re-excites** (§7.4) — analysis-only retry is not the default
16. **Wait-instance correlation** (§7.3) — stale intents must not satisfy a later wait
17. **I2S DMA is driver-owned** (§9.4.1) — verify placement for the IDF version in use; `dma_frame_num` must be a multiple of 3 at 24-bit
18. **Layer 4 model must be configured** (§9.2) — no validated default exists; never hardcode a fallback
19. **`wait_id` is a wire-protocol requirement** (§12.3) — reject mismatched IDs, don't accommodate clients that omit it
20. **Command admissibility is per intent** (§12.3) — not one global rule; `ABORT` must work during operations
21. **Artifact storage must not re-compress asymmetric displacement curves** (§11.4)
22. **`n_rim_angles` is model-prep, not runtime** (§11.4) — changing it invalidates every shipped layout
23. **Artifact-bound `SET_PARAMETER` must refuse** (§12.3.1) — tolerances and trust weights are artifact-bound, not tuning knobs
24. **Asymmetry comes from geometry, `c` from a response experiment** (§11.1) — never from a static tension snapshot
25. **Side tension ratio is artifact-bound** (§11.2) — only the overall scale is mutable; `n_mt` was identified against the ratio
26. **Tension model and `target_tension` are session-fixed** (§12.3.1) — a mid-session target change strands a stale `AdjustmentPlan`, not just the cost history
27. **`T_target_assumed` is normalized to mean = 1.0** (§11.2) — a ratio alone has arbitrary scale
28. **Synthetic round-trip is per layout** (§14.3) — rank-deficient layouts recover the row-space projection, not the injected vector
29. **"Continue" after a rejected measurement means continue COLLECTING** (§13.1) — solvability is decided separately by §8.11
30. **Expected nullity is not a violation** (§13.3) — abort on *disagreement with artifact metadata*, not on deficiency itself
31. **One terminal vocabulary** (§13.2) — `CONVERGED` / `CONVERGED_GEOMETRIC_ONLY`, never a second set of names
32. **Weights apply as √w, not w** (§8.3.1) — using `w` gives an effective `w²`
33. **Verification-grade ≠ solver-admissible** (§8.6.3) — `suspect` tension data cannot support `CONVERGED`; C2's ceiling is `CONVERGED_GEOMETRIC_ONLY`
34. **Active row set ≠ availability** (§8.11) — policy may exclude a whole channel whose measurements succeeded
35. **`MEAN_TENSION_MODEL_UNAVAILABLE` is non-terminal** (§8.4) — not `ABORT_NO_MODEL`
36. **Artifact compatibility is a fingerprint, not dimensions** (§11.4) — same-shape matrices can be physically foreign
37. **`n_fit_samples` ≠ `n_rim_angles`** (§8.9) — the Fourier floor governs fitting, not the solver grid
38. **Φ† dimensions are per layout** (§8.7) — `n_layout_rows`, not `n_full_rows`
39. **C2 `POSITION` and `READ_RUNOUT` contain operator waits** (§7.3) — measurement is not fully autonomous in Stage 1
40. **Orchestrator never sequences acoustic sub-stages** (§7.4, §9.1) — one coarse call; retry and cancellation live at that boundary
41. **Tension residual is `T − s·T_norm`** (§8.3.1) — `T − T̄` is symmetric-only
42. **`n_active_rows`, not `n_valid_rows`** (§8.11) — policy exclusions change the problem, not the data
43. **Artifact compatibility needs an INDEPENDENT expected fingerprint** (§11.4) — a self-consistent artifact for the wrong wheel would otherwise pass
44. **Effective conditioning over the retained subspace** (§8.11 R3) — ordinary `cond()` rejects every legal `TENSION_ABSENT`
45. **`d_adj ≈ −d_applied` is not a general assertion** (§14.3) — only for deliberately constructed synthetic cases
46. **Plan-generation and safety parameters are session-fixed** (§12.3.1) — changing them strands a pending plan
47. **P6 needs authoritative on-device state** (P6, §12.2) — telemetry is best-effort and cannot satisfy it
48. **`d_adj = −d_ls` is the REFERENCE one-part method** (§8.2) — never the project runtime algorithm; use §8.4's two-part solve
49. **Per-side targets are DERIVED, never configured** (§8.4, §11.2) — one overall scale × the artifact's normalized distribution
50. **`n_rim_angles = n_spokes`** (§8.9) — 36-spoke wheels use 36 angles, not 32
51. **One profile authority** (§11.3.1) — wheel binds *compatibility*, not an approved profile ID
52. **`SET_PARAMETER` admissibility is state × class** (§12.3.1) — session-fixed values are settable in `READY` and nowhere else
53. **Phase 1h ≠ Stage 1 release** (§15) — the WiFi-load stress test is part of the release gate
54. **Placeholders return status** (P2) — never neutral values
55. **Linearity is an assumption** (§8.1) — validate, don't presume
56. **Wheel navigation is a capability with a manual C2 implementation** (§10A) — `POSITION` means "feature X at station S"; the orchestrator acts on outcomes only; the human is the C2 actuator, not a permanent assumption
57. **Three coordinate domains stay separate** (§10A.1) — station angles are machine-profile data (§11.6), actuator coordinates never leave the navigation implementation, and no steps-to-angle ratio is a constant; an unknown calibration is refused, not guessed

### 17.4 Definition of done, per phase

- All specified tests pass, including the structural property tests where applicable.
- **Within the phase's implemented dependency closure**, the workflow runs with simulated hardware implementations. Running the *full* end-to-end workflow is a **Phase 1h** requirement — it is not achievable at 1a, before the orchestrator, solver, acoustic, and runout subsystems exist.
- Both PlatformIO environments build.
- Golden fixtures exist for every algorithm added.
- No placeholder returns a neutral value.

---
### Specification Consistency Guardrail

This specification is the authoritative implementation contract, but it is a large document and may contain isolated stale wording, duplicated explanations, obsolete examples, or minor cross-section inconsistencies.

**The existence of an inconsistency does NOT by itself mean the architecture is invalid or that implementation must stop. Treat inconsistencies as local specification defects unless they demonstrably affect a larger contract.**

When two statements appear inconsistent, apply the following rules.

#### 1. Do not silently invent material behavior

If resolving the inconsistency would require choosing or changing any of the following, **do not decide it yourself**:

* mathematical algorithm or solver semantics;
* sign, unit, indexing, or matrix convention;
* safety behavior or adjustment limits;
* state-machine transitions;
* solver admission or degradation policy;
* artifact compatibility or persistence semantics;
* session/configuration lifecycle;
* subsystem ownership or interface boundaries;
* hardware behavior;
* convergence or abort semantics;
* test expectations where different interpretations would require different runtime behavior.

Report the conflict precisely and identify the minimum decision required.

Continue unrelated implementation whose behavior is unaffected.

#### 2. Minor reconciliation is allowed when the intended rule is unambiguous

The agent MAY resolve an inconsistency without asking when it is clearly a stale or non-material propagation error and no meaningful implementation choice exists.

Examples:

* an old variable name survives after a field was renamed everywhere else;
* an example says `32` where the authoritative rule clearly says `n_spokes`;
* prose says “valid rows” while the surrounding normative algorithm and data model consistently use `active_row_set`;
* an appendix or summary omits a caveat that the authoritative section explicitly requires;
* duplicated explanatory prose uses obsolete terminology but does not define different behavior.

In these cases:

1. follow the clearly authoritative rule;
2. do not alter architecture;
3. record the reconciliation in the implementation notes or change log.

#### 3. Prefer normative and specific rules over summaries

When no explicit precedence statement exists, use this order:

1. explicit `MUST`, `MUST NOT`, required algorithm, state transition, schema, or invariant in the subsystem's authoritative section;
2. detailed section-specific contract;
3. cross-reference or explanatory prose;
4. examples and worked examples;
5. summaries, phase descriptions, agent checklists, and Appendix A.

A lower-precedence statement MUST NOT override a more specific normative contract merely because it appears later in the document.

Where the document explicitly states another authority relationship, that explicit relationship wins.

#### 4. Do not "fix" runtime behavior merely to satisfy a conflicting test

If a prescribed test appears incompatible with the normative runtime algorithm:

* first verify the implementation against the runtime contract;
* do not modify correct runtime behavior solely to make the test pass;
* report the test-contract inconsistency;
* continue work that does not depend on resolving it.

Tests verify the contract; they are not permission to silently redefine it.

#### 5. Do not escalate local inconsistencies into speculative redesign

Finding one contradictory sentence does NOT authorize:

* redesigning the subsystem;
* introducing a more general abstraction;
* replacing an existing algorithm;
* expanding scope;
* changing C2/C3 boundaries;
* adding infrastructure "to make the specification cleaner."

Apply the smallest correction necessary to preserve the already-defined architecture.

#### 6. Material ambiguity blocks only the affected decision

When a genuine material ambiguity is found, stop only the implementation path that depends on that ambiguity.

Do NOT stop the entire project if independent phases or components can proceed under already-settled contracts.

Report:

* the conflicting statements;
* why they lead to materially different implementations;
* which work item is blocked;
* which work may safely continue;
* the smallest clarification needed.

#### 7. Do not overstate specification defects

Use precise language.

Prefer:

> "§X and §Y specify different behavior for this case; implementation of this case is blocked pending clarification."

Do NOT claim:

> "The architecture is broken,"
> "the project cannot work," or
> "the specification is fundamentally invalid"

unless the conflict actually demonstrates that conclusion.

The objective is to implement the specified architecture faithfully while surfacing only decisions that genuinely require human authority.





## Appendix A — Reference Equations Summary

> **This appendix is a navigation aid, not an authority.** Where it disagrees with the body, **the body wins**. It is written to be safe to read in isolation because an agent will use it as a cheat sheet — every entry below therefore carries its asymmetric/per-layout caveats inline rather than relying on the reader having read §8 first.

```
DIMENSIONS  (per layout — §8.7)
    n_full_rows      = 2·n_rim_angles + n_spokes
    Φ                : n_full_rows × n_spokes            (FULL matrix only)
    n_layout_rows(L) = popcount(row_mask(L))
    Φ†(L)            : n_spokes × n_layout_rows(L)       (per layout, NOT n_full_rows)

INFLUENCE MATRIX
    Φ = [Φ_u ; Φ_v ; Φ_t]
    Φ_u, Φ_v : one rotated curve ONLY if asymmetric=false and equivalence
               verified; otherwise per class / per spoke        (§8.8)
    Φ_t      : 4 classes (side × lead/trail); mirroring is symmetric-only

PREDICTION
    Ŷ = Y_b + Φ·d

NORMALIZED RESIDUALS  (dimensionless — §8.3.1)
    û = (u−u₀)/tol_lat     v̂ = (v−v₀)/tol_rad
    T_norm = artifact T_target_assumed, normalized to mean 1.0
    s      = (T · T_norm)/(T_norm · T_norm)      current overall scale
    T̂      = (T − s·T_norm)/tol_ten
    ⚠ (T−T̄) is the SYMMETRIC special case (T_norm = [1,…,1]). Not general.

WEIGHTING — apply √w, NOT w                                     (§8.3.1)
    Φ̃ = [Φ_u_norm ; Φ_v_norm·√w_v ; Φ_t_norm·√w_t]
    Ỹ = [û        ; v̂·√w_v        ; T̂·√w_t       ]
    ‖√w·r‖² = w‖r‖².  Using w directly gives an effective w² — WRONG.

LEAST SQUARES
    d_ls = Φ̃†(L)·Ỹ        where L is the ACTIVE layout, and the active row
                          set must exactly match row_mask(L)    (§8.7.1, §8.11)
    ⚠ d_adj = −d_ls is the REFERENCE ONE-PART method, NOT this project's
      runtime algorithm. Use the two-part solve below.          (§8.2, §8.4)

SHAPE TRUING  (Part 1 — §8.4)
    d = −( d_ls − proj_{n_mt}(d_ls) )
    n_mt satisfies BOTH:  [Φ_u;Φ_v]·n_mt = 0   AND   Φ_t·n_mt ∝ T_target
    symmetric special case: n_mt = [1,…,1]/√n_spokes
    ⚠ The arithmetic mean is the SYMMETRIC case only. Do not use it generally.
    ⚠ If n_mt is unidentified → TENSION_ABSENT, no projection, geometry only.

MEAN TENSION SHIFT  (Part 2 — §8.4)
    symmetric:   d_cm[i] = (target_tension − mean(T))/c       for all i
    asymmetric:  d_cm[i] = f(ΔT_side(i), side(i), c_a, c_b)   ⚠ OPEN — REFUSE
    ⚠ d_cm is a VECTOR. Scalar broadcast is the symmetric case only.

COMBINED
    d_adj[i] = d[i] + d_cm[i]                       (element-wise)

TARGET TENSION  (§11.2)
    T_target_assumed normalized so mean = 1.0
    target[i] = target_tension × T_target_assumed_normalized[i]
    target_tension = desired overall MEAN spoke tension
                     configurable between sessions, immutable within one
    ⚠ Per-side targets are DERIVED. Independently configurable side
      targets do not exist — they would change the ratio n_mt was
      identified against.

GRID  (§8.9)
    n_rim_angles = n_spokes      (32-spoke → 32 angles; 36-spoke → 36)

COST — stall detection ONLY, never convergence     (§8.6)
    J(L) = ‖r̃_active(L)‖² / n_active_rows
    FULL:            numerator = ‖û‖² + w_v‖v̂‖² + w_t‖T̂‖²
    TENSION_ABSENT:  numerator = ‖û‖² + w_v‖v̂‖²
    ⚠ Excluded channels contribute neither rows nor residual energy.
    ⚠ n_ACTIVE_rows (§8.11), not available rows.
    ⚠ Comparable only WITHIN one layout.
    ⚠ J ≈ 1 does NOT mean "at tolerance".

CONVERGENCE — per channel, and verification-grade   (§8.6, §8.6.3)
    geometric := max|u−u₀| ≤ tol_lateral AND max|v−v₀| ≤ tol_radial
    per side S:  non_uniformity(S) = stdev(T_S)/mean(T_S)      [CV]
                 mean_error(S)     = |mean(T_S) − target(S)|   [N]
    tension_compliant := ∀S: CV ≤ tol_tension_cv
                         AND mean_error ≤ tol_mean_tension_error
                         AND all contributing tension rows are VALID
                             (suspect ⇒ not verification-grade)
    ⚠ Stage 1 cannot reach CONVERGED — provisional mode ID makes every
      TensionEstimate suspect. Best result: CONVERGED_GEOMETRIC_ONLY.
    ⚠ CONVERGED_GEOMETRIC_ONLY = compliance NOT ESTABLISHED with
      verification-grade evidence. It does NOT imply tension was
      unmeasured or unevaluated.

ORCHESTRATOR BOUNDARY  (§7, §9.1)
    orchestrator sees:  POSITION → MEASURE_SPOKE_TENSION
    excite/capture/analyze/estimate are acoustic-subsystem INTERNAL
    ⚠ Never sequence acoustic sub-stages from the orchestrator.

WHEEL NAVIGATION  (§10A)
    request:  feature (spoke i | rim index k | rim angle θ)  at  station S
    wheel coords θ  ·  station angle φ_S (machine profile §11.6)  ·  actuator coords (internal only)
    rotation state R:  feature θ sits at machine angle wrap(θ + R);   R' = wrap(φ_S − θ)
    C2: manual implementation → PENDING_OPERATOR → WAIT_FOR_OPERATOR
    C3: automated implementation → IN_MOTION → poll → DONE | FAULT (WHEEL_REFERENCE_LOST)
    ⚠ No universal station. No fixed steps↔angle ratio. Consumers never own wheel motion.

FOURIER  (model prep — §8.9)

    y(θ) = a₀ + Σ(aₙ cos nθ + bₙ sin nθ)
    n_fit_samples ≥ 2·max(N_lat, N_rad) + 1
                                    ⚠ applies to FITTING samples,
                                      NOT to n_rim_angles (the solver grid)

CONDITIONING  (§8.11 R3)
    retained = { σ_k : σ_k > rank_tolerance · σ₁ }
    effective_cond = σ₁ / min(retained)     ⚠ ordinary cond() is INFINITE
                                              for TENSION_ABSENT
```

**Reference values — that wheel only, NOT universal, NOT transferable to the dimensionless formulation:**
μ_v = 0.5 · μ_t = 10⁻⁵ mm/N · c = 473 N/rev · N_lat = 6 · N_rad = 13 · 64 fitting samples
Reference tolerances: ±0.1 mm lateral · ±0.05 mm radial · 1000 ± 100 N

**Stage 1 defaults:**
n_rim_angles = n_spokes (32 or 36) · 48 kHz / 24-bit · tolerances, trust weights and acceptance limits from this project's own criteria
