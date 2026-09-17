# Phase B, re-planned — two-station solenoid integration and excitation/acquisition characterization

Approved in Plan mode on 2026-09-14 and committed here so the pre-registered rules, outcome
vocabulary and acceptance criteria it fixes are a citable, diffable artifact rather than session
state. `docs/SOLENOID_CAMPAIGN.md` is the running record of what was actually done against this
plan; this file is the plan itself and does not change as stages close. If the plan changes, that
change is a new commit to this file, with the reason in the commit message.

## Amendment 1 (2026-09-17) — mounted stations, rotor side, four spoke classes

Approved 2026-09-17. C3–C10 below are also applied inline in the stage sections, the provenance
table and the schedule, so the rest of this file already reads in amended form. Where a
number changed, the original is noted inline as "(was …)".

### What the operator reported

**Mounting**
- Both solenoids have been mounted since before B0.
- Each solenoid sits on its own cut 4040 extrusion, which slides along its own tower. The tower height sets the strike point along the spoke's free span.
- A 3D-printed fitting sets the plunger-to-spoke standoff.
- Both strike points were eyeballed to roughly mid free span.

**Labels**
- Stations and solenoids are already labelled LEFT/RIGHT.

**Donor wheel**
- It is a front disc wheel, with the rotor on the RIGHT side.

**Spoke classes**
- There are four spoke classes: side × leading/trailing.
- Strike geometry repeats within a class.
- Leading spokes present roughly vertical to the plunger; trailing spokes present diagonally.
- Standoff depends on class:
  - **LEFT:** leading spokes are closer, trailing spokes farther.
  - **RIGHT:** leading spokes are closer, trailing spokes farther.
  - **(Corrected 2026-09-17, after B2-M3's LEFT data):** the operator's original description had
    LEFT reversed (trailing closer). Both sides actually have the same ordering — leading closer,
    trailing farther — not mirrored as first described. See `SOLENOID_CAMPAIGN.md`'s B2-M3 entry.
- Each fitting is set to reach the farther class on its side (trailing, on both sides).

### Facts already in the repo that this lines up with
- **Sides.** SPEC §6.4.1 defines Side A as the rotor side. On this rig, therefore, **RIGHT = Side A and LEFT = Side B**.
  - This is recorded as an *observed correspondence for this wheel in this orientation*.
  - A3's decoupling stays, because the correspondence flips if the wheel is mounted the other way round.
- **Asymmetry.** The spec already names the donor wheel as a front disc wheel, *expected asymmetric, unverified*.
  - The two sides may therefore sit at different tensions, and so at different reported-peak ranges.
  - No campaign rule assumes the two sides are equal: consistency bands are per spoke, and the shared DSP config covering both sides is already G-analysis's job.
- **Solver classes.** The solver's spoke classes (`model_prep/truing_model_prep/conventions.py:21-26`) already use a period-4 pattern: (B, lead), (A, lead), (B, trail), (A, trail).
- **Spoke 0.** SPEC §6.5 lets spoke 0 be any class.
  - Its class is declared, and the generator applies an offset.
  - It must be physically marked relative to the valve stem.
  - It must sit at the reference station; A2 already sets that to `ACOUSTIC_LEFT`.

### Unchanged
- A1–A10, and all of the B1a/B1b firmware scope.
  - Spoke class is rig and campaign data.
  - The firmware needs it only if G-class (C4) trips.
- The even→LEFT convention.
- The outcome vocabulary.
- The claim ceiling.

### Changes

**C1 — S0 and the counting direction.**
- **The direction comes from SPEC §6.4, not from preference.**
  - Rim angle increases counter-clockwise as viewed from Side A, which is the rotor face (the RIGHT side).
  - Spoke i sits at 2π·i/n (`wheel_geometry.h:32`).
  - Seen from the rotor side with the valve at the top, the index therefore increases **leftward**.
- **The operator's clockwise turning agrees with this.**
  - The operator turns the wheel clockwise from that view. This is a habit, and is recorded as one.
  - Clockwise turning brings spokes to a fixed plunger in the order i → i+1: the next spoke to arrive is the one that was sitting counter-clockwise of the current one.
- **S0 is the LEFT-leading spoke immediately counter-clockwise of the valve** — the first spoke to the valve's left, seen from the rotor side.
  - This keeps S0 on LEFT, which the compiled convention (`acoustic_stub.c`), `reference_station = ACOUSTIC_LEFT` and the PRESENT precondition in `board_nano_esp32.h` all assume.
  - Choosing the RIGHT-trailing spoke to the valve's right instead would move S0 to RIGHT (a firmware change) and would count against the spec's direction.
- **What S0 decides:** the physical spoke that every campaign index counts from, and therefore the class map in C2.
  - It changes no campaign result's pass/fail.
  - It is marked per wheel, so nothing carries over to future wheels.

**C2 — class map: declared now, verified at B2-M8.**

The declared map, as seen from the rotor side:

| i mod 4 | class |
|---|---|
| 0 | LEFT-leading |
| 1 | RIGHT-leading |
| 2 | LEFT-trailing |
| 3 | RIGHT-trailing |

- **It matches the solver.** With RIGHT = Side A, this is exactly the solver's generator order at **offset 0**.
  - A future physical truing session would declare `indexing_origin = (Side B, LEADING)`.
  - Known limitation 1 then comes down to that declaration plus the M8 check.
  - The limitation stays recorded as open until M8 confirms the map and a physical session actually uses it.
- **Verification at M8:** the operator confirms that S0..S3 are L-lead, R-lead, L-trail, R-trail.
  - If they aren't, **halt**: the map is wrong, and so is the offset-0 match.
- **Stratification:** every trial manifest carries the spoke's class, and every stage report is stratified by class.

**C3 — B2 becomes class-aware.** Applied inline under B2.
- M3 runs first.
- M1, M2, M3, M5 and M6 are per class.
- The station bracket is the intersection of the two class brackets.
- The station rig_id gains the fitting id, the tower height and both per-class standoffs.

**C4 — new gate, G-class (a user decision).**
- **One pulse per station is a hypothesis, not a known fact.**
  - *For it:* each fitting is set so that one plunger reaches both classes.
  - *Against it:* in the gate-control bench investigation (`tools/bench/README.md`), the tuning stopped working once the travel distance changed — and the two classes differ in exactly that distance.
- **How it is tested:** first B2-M3, cheaply, with the bench tool and no firmware; then B3.2, on acoustic outcomes.
- **Why per-class excitation isn't built in advance:** it needs the C2 class map in firmware, plus a change to the profile schema and digest. EXPERIMENT_METHOD rule 1 says not to build that before the evidence asks for it.
- **Trigger** — either of:
  - the two classes' M3 brackets don't overlap;
  - in B3.2, every level that satisfies the constraints leaves one class's worst spoke more than 10 pp below the other class's.
- **On trigger: halt, and put these options to the user:**
  - re-seat the fitting and re-run M2–M3;
  - restrict demo and acceptance scope to the working class at that station, and say so;
  - authorize per-class excitation, whose class map C2 already provides.

**C5 — B3.1: class-balanced spoke sets.** Chosen by the operator.
- Per station: 4 exploration spokes (2 per class), 4 held-out (2 per class) and 2 reserve (1 per class).

**C6 — B3.2.**
- 6 strikes per level per spoke, over 4 exploration spokes per station.
- Exploration gate: worst spoke ≥5/6.
- The pulse rule also checks each class's worst spoke, and feeds G-class.

**C7 — B3.4.**
- **Loaded arm:** 4 held-out spokes per station × 6 strikes = 48 trials.
- **Unchanged thresholds:** ≥40/48, and no station below 18/24.
- **Per-spoke floor:** 5/6.
- **New class floor:** no class below 10/12. The demo covers every class, so one weak class would break Rung 3.
- **Quiet arm:** 8 spokes × 3 strikes = 24.

**C8 — the rotor.**
- It is a RIGHT-only structural resonator.
- Air shots can't excite it, so it can only appear in strike captures.
- Blind review therefore gains the category "metallic / rotor ring audible", stratified by station.

**C9 — the ±2 Hz band stays.**
- Its basis came from Phase A, which used a different setup.
- If a class's repeat spread is wider than the band, that shows up as inconsistent clears in exploration, which halts at the constraints.
- The band is never widened after the fact.

**C10 — provenance.** Applied inline in the provenance table.
- The fitting id and tower height are explicit rig_id fields.
- The S0 marking and class map invalidate class-stratified results, and a change to them means re-running M8.

**C11 — record and schedule.** Applied inline in the schedule.
- B0 closes on 2026-09-17, pending the S0 mark (the first operator step of B2).
- B2 is unblocked and runs in parallel with B1a, on the bench tools.
- Later stages move one day later, absorbing the 09-20 reserve day. The demo date is unchanged.

## Context

**Where things stand.** Phase A and the interim items (I1–I3) are closed; they are recorded in the repo and in `docs/IMPLEMENTATION_NOTES.md`, with a summary at the end. Hand plucking cannot support fitting the DSP constants. The chain is ~26–32 dB noisier than the gear the constants came from. The only acceptance rule that means anything is discrimination against no-pluck controls. The Darlington approach is dropped.

**Hardware.** Three **IRLB4132** logic-level MOSFETs and two JF-0530B push solenoids.

**Intended physical configuration (this demo and campaign; later designs are not constrained):**
- Two actuators, each at **its own acoustic station**, one on each side of the wheel.
- Mount height is adjustable.
- Each spoke is reachable only by the actuator on its own flange's side.
- The flange side alternates with spoke index.

**Convention, local to the acoustic subsystem.** **Spoke 0 is assigned to the LEFT actuator**, and assignments alternate from there. "LEFT" and "RIGHT" are rig labels. The convention is deliberately **not** tied to Side A/B, cassette or rotor side, or any wider mechanical side designation. Those belong to a separate mechanical-design campaign.

**What this campaign is not.** It does not identify the fundamental or any vibration mode, and it does not relate frequency to tension. That work is a separate campaign, which needs a calibration rig and an absolute-tension reference, and may live outside this repo. Here the firmware's `f1_hz` is only a **reported peak frequency** (SPEC §6.3). Nothing in this campaign labels any peak "correct" or "the fundamental". The claim ceiling is unchanged: `suspect` / `PROVISIONAL_MODE_ID`, and `CONVERGED_GEOMETRIC_ONLY` at best.

**Goal.** Establish a trustworthy excitation/acquisition configuration before the **demo on 2026-09-26**, working up toward all 32 spokes.

**Hand plucking is removed, not kept as a fallback.**
- A session on a real-front-end image is **refused** unless every acoustic station has an actuator.
- A failed fire during an attempt is a **rejected attempt**, never an invitation to pluck by hand.
- The ARMED hand-pluck cue, the pluck lead and the HAND excitation path are deleted from the firmware (A10).

### Known limitations (must be cleared before a real physical truing session)

1. **The acoustic spoke identity is not reconciled with the solver's.**
   - This campaign's convention is: spoke 0 = the spoke the LEFT actuator strikes, alternating from there.
   - It is deliberately **not** reconciled with the solver artifact's `indexing_origin` (§6.5, artifact-bound), which is what maps spoke indices to influence-matrix classes and to the nipple-adjustment instructions.
   - That is safe for this demo, because the fast demo's runout and solve are synthetic.
   - **Before any real physical full truing session** — real runout, real adjustments on a real wheel — the two identities must be reconciled. That work belongs to the later mechanical-design campaign.
   - Until then, every stage report and the demo narrative must say so.
   - **Amendment 1 update:** on the current donor wheel, the operator's reported spoke-class layout matches the solver's generator order at offset 0 (`indexing_origin = (Side B, LEADING)`) — see Amendment 1, C1–C2. This is an *observed match for this wheel*, not a general reconciliation rule, and stays open until B2-M8 confirms the class map and a physical session actually declares it.
2. **Sessions need both actuators.** If one actuator is broken, no session can run (by design; see A10). Campaign trials at the working station still can, because they run outside a session.

**What "trustworthy" means, defined operationally:**
1. **Validity**
   - no clears on controls (**false clears**)
   - no clears that disagree with the same spoke's repeated measurements (**inconsistent clears**)
   - no excitation by the wrong actuator
2. **Reliability** — clears are frequent enough that the demo acquires the spokes it claims within its bounded retries.

## Outcome vocabulary (pre-registered; the claim boundary lives here)

| term | definition | deliberately not |
|---|---|---|
| **clear** | the acoustic call returns an estimate: status `suspect` / `PROVISIONAL_MODE_ID` with a reported peak frequency | a correct frequency |
| **false clear** | a clear on a no-fire or air-shot control | — |
| **consistency band** | ±2 Hz around the median reported peak of that spoke's clears under the same condition, needing ≥3 clears. Basis: the spread of Phase-A clears was ±0.2 Hz, and competing components sat ≥5 Hz apart. | the spoke's true pitch |
| **inconsistent clear** | a clear outside its spoke's consistency band: the measurement disagreeing with its own repetition | a "wrong mode" |
| **consistent clear rate** | consistent clears ÷ fixed-n strikes | accuracy |
| **selection shift** (offline DSP candidates only) | on the **same captures**, a candidate reports a peak >2 Hz from the baseline pipeline's peak where both clear. Candidates that shift selection are **out of scope** here — changing which component is picked is mode-ID work. | an improvement |

## Architecture for two stations, derived from the existing design

The spec already supports this:
- §9.1's one-call contract `measure_spoke_tension(spoke_id, wheel_geometry)` gives the acoustic composite ownership of excitation (`acoustic_if.h:48-50`).
- Stations are machine-profile data, and positioning prompts name both the feature and the station (§10A.3, §11.6).
- §10A.3 anticipates "a station may be added". §16 lists station geometry as open.

What is single-actuator today:
- one pluck pointer (`acoustic_real.h:76`)
- one `pluck_commanded` bool, which flows through telemetry, the wire, capture.json, the UI and fixtures
- one GPIO macro per board
- one `TRUING_STATION_ACOUSTIC`, hard-coded as the target for every spoke (`orchestrator.c:182`)
- fast-demo navigation that is synthetic, so the spoke it names is not the physical spoke under a plunger

| # | decision | why |
|---|---|---|
| **A1 Convention and selection live in the acoustic subsystem** | One pure contract function, `truing_acoustic_station_for_spoke(spoke_id)`, in `truing_hal/acoustic_if.h`: even spokes go to `ACOUSTIC_LEFT`, odd spokes to `ACOUSTIC_RIGHT`. **The orchestrator asks it** where to position a spoke. **`acoustic_real` uses the same function** to pick the actuator installed at that station. `acoustic_real` holds an actuator table keyed by acoustic station. `pluck_if.h` is unchanged: one instance per physical actuator. At most one fire per measurement. A missing entry or a failed fire is handled by A10. | One source of truth for the convention. The orchestrator never learns the alternation rule (the user's locality requirement), and no orchestrator sequencing of acoustic sub-stages is added (§17.3 #40). §10A.2 still holds: the acoustic subsystem names a station, never an angle. **This moves the "which station" decision from a hard-coded orchestrator constant to the acoustic subsystem**, so it is flagged as a decision-ownership change for approval and spec-reviewer. |
| **A2 Two acoustic stations** | Replace the station-id enum value `ACOUSTIC` with `ACOUSTIC_LEFT` and `ACOUSTIC_RIGHT` in `config.h`. The machine profile declares both. Validation requires both. `reference_station` is `ACOUSTIC_LEFT`, since spoke 0 is confirmed there. B2 measures both station angles, then the machine profile is updated; per §11.6 that is a new profile. Until then the fixture angles are **explicitly labelled placeholders**. | Matches the rig. Extends §11.6's station list and resolves the §16 station-geometry item for this configuration. Recorded as a spec reconciliation in `IMPLEMENTATION_NOTES`. |
| **A3 Identity convention, and what it is not coupled to** | The operator marks as **S0** a spoke that the LEFT station's actuator can strike; S1 must then belong to RIGHT. Stations and solenoids carry LEFT/RIGHT labels. **No coupling** to §6.4.1 Side A/B or the solver's `indexing_origin`. **Cross-campaign dependency (recorded, not solved):** before any *physical* truing session — real runout and adjustment on a real wheel, not the fast demo's synthetic runout — the physical S0 used here must be reconciled with the solver's artifact-bound `indexing_origin`. That reconciliation belongs to the mechanical-designation campaign. | Keeps concerns separated as requested. The fast demo's solve runs on synthetic runout, so the demo is not affected. Flagging the dependency prevents a silent spoke-mapping error later. |
| **A4 Excitation separated from the chain profile, per actuator** | New `truing_excitation_profile_t { pulse_ms[ACOUSTIC_LEFT], pulse_ms[ACOUSTIC_RIGHT] }` with its own digest, starting **identical (20/20)**. `excitation_pulse_ms` leaves `truing_chain_profile_t`, which becomes `chain_profile/2` (transducer, capture and DSP only). Captures record both digests and the applied pulse. | SPEC §11.3 lists no excitation field; the code added one. Whether pulses differ per actuator is for the evidence to decide. Separating now means a per-actuator pulse change never invalidates DSP evidence or fixtures, and there is no schema change mid-campaign. |
| **A5 DSP config shared across stations** | One chain profile. **G-analysis** escalates if no shared candidate meets the constraints at both stations. | There is no evidence yet that they need to differ, and evidence is partitioned by station so a later split loses nothing. |
| **A6 Physical spoke identity wherever an actuator fires** | In the fast-demo acoustic image, SPOKE targets at either acoustic station are positioned by the operator (manual navigation); rim targets stay synthetic. This is one composite navigation implementation with an honest descriptor. Campaign trials carry a physical spoke index declared by the runner, and the station and actuator are derived from it. | With synthetic navigation, logical spokes would alternate stations while nothing physically moves, so the wrong actuator would fire at whatever spoke happens to be there. That is a correctness bug. It touches navigation authority and provenance (§10A, §6.2), so spec-reviewer sees it before merge. |
| **A7 Provenance and observability** | **Session:** the acoustic descriptor names installed actuators, e.g. `acoustic_real+pluck(L,R)`. Sessions without both are refused (A10). **Telemetry:** phase frames carry `actuator` (`LEFT`/`RIGHT`/`NONE`) and `fired`; these replace `excitation` and `pluck_commanded`. **capture.json:** gains `station`, `selection` (`derived`, `forced` or `no_fire`), `fired`, `excitation_digest`, applied pulse. **UI:** positioning and pluck wording name the station. `truing_tension_estimate_t` is unchanged, because the station can be derived from the spoke (§6.2). | Which seam fired and whether it failed cannot be derived, so they go to diagnostics. Anything derivable stays out of the measurement record. |
| **A8 Board profiles** | `BOARD_PLUCK_ACTUATOR_LEFT_GPIO`/`_PRESENT` and `BOARD_PLUCK_ACTUATOR_RIGHT_GPIO`/`_PRESENT`. Nano: LEFT is GPIO 9 (D6), RIGHT is GPIO 10 (D7, free). DevKit: 15 and a pin verified to be free. | Pins stay only in `truing_board` (§4.3). PRESENT per actuator allows sequential bring-up. |
| **A9 Bring-up** | Probe each *present* actuator once; nothing fires when PRESENT is 0. When an actuator is absent, its live bring-up measurement reports NOT PERFORMED. | Fixes the currently false invariant "PRESENT=0 ⇒ no actuation". |
| **A10 No hand-pluck fallback** | **Readiness at admission:** a new additive contract call, `truing_acoustic_ready(self, &reason)`. `acoustic_real` returns false unless every acoustic station has a non-NULL seam whose `available()` is true; synthetic and recorded implementations return true. `session_admission` (`orchestrator.c:335`) calls it and refuses START with the reason. The orchestrator still knows nothing about actuators. **Per attempt:** a NULL seam or a failed `fire()` returns early as `rejected` / **`EXCITATION_UNAVAILABLE`**, a new reason code, with no capture. The previous capture survives, per I1. Retry re-excites (§7.4). **Removed:** the ARMED phase, `pluck_lead_ms` and its setter, HAND excitation, and the hand-pluck UI cues. **Kept:** debug no-fire controls stay possible, as an explicit campaign override recorded as `selection=no_fire`. | This is the operator's decision. Degrading quietly to hand plucks would put a different excitation into the evidence and the demo without an explicit choice. Keeping readiness in the acoustic contract keeps the orchestrator actuator-agnostic, as with A1. A new reason code avoids overloading `CALIBRATION_MISSING` or `SENSOR_TIMEOUT` with a meaning they don't carry. It extends the §13.2 reason vocabulary, which gets recorded as a spec reconciliation. Consequence: images with no actuators (`*_mic` today, `s3_devkit_mic`) refuse sessions. That is intended. |

## How we get going — execution order after approval

**Approval does not mean code first.** B0 is physical and doesn't wait on any firmware. It is validated with the standalone bench sketch (`tools/bench/solenoid_smoke`), which is already updated for both channels and builds. Code and hardware run **in parallel** from the start:

| step | who | what | waits on |
|---|---|---|---|
| 1 | **operator** | B0: wire both channels, label LEFT/RIGHT, mark S0, run the pre-power checks | nothing |
| 1′ | agent | B1a in parallel: host tests first → implementation → T2 → builds → spec-reviewer → atomic commits (no Claude stamps) | nothing |
| 2 | operator + agent | **B0 smoke test.** Operator says "wired". Agent flashes the bench sketch and captures serial. Operator watches LEFT hold → RIGHT hold → alternating shots, and reads V_DS. | step 1 |
| 3 | agent + operator | B1a T6 on target: pulse load check, routing, A10 refusals | steps 1′ and 2 |
| 4 | agent | B1b (navigation composite) starts | step 3 |
| 5 | operator + agent | B2 per station → PRESENT flips + station angles | step 3 |
| … | | B3.0 → B4 as scheduled | |

**Approving the plan authorizes the agent to:** write and commit B1a/B1b code, docs and tools; run T0–T6; flash either board; drive bench tools.

**It still stops and asks at:**
- any physical work (B0/B2 hands-on steps)
- flipping PRESENT only after that station passes B2
- the B3.3 constant freeze
- G-analysis and G-demo
- any spec reconciliation (§11.6 stations, §13.2 reason code, §9.1 contract additions) — summarized to the user after spec-reviewer and before commit

## Corrections to the suggested knobs and to earlier text

| suggestion / earlier claim | code | consequence |
|---|---|---|
| "Excitation strength" is a knob | 12 V supply is fixed; the IRLB4132 is fully on at 3.3 V gate (V_GS(th) 1.35–2.35 V) | Strength is set by pulse × standoff. No voltage or PWM path gets added. |
| Pulse duration | Real, but **scheduler-timed**: `vTaskDelay` in `orch_demo` (prio 2, core 0) sits below `httpd` (prio 5) and `wire_tx` (prio 3) | Uncontrolled today. Make it hardware-timed, record the width every trial, then sweep **per actuator**. |
| Actuator-to-spoke distance | Physical; recorded nowhere | Declared per station. Set in B2; swept only if B2 leaves it ambiguous. |
| Fire/capture timing, including this plan's own earlier "sweep firing timing" | `fire()` finishes before `capture()`; the window is **onset-anchored** (`analysis.c:97-101`) | **Retracted.** Observe that the strike lands in the buffer and the onset anchors on it. Sweep gate, window and next-onset margin **offline**. |
| An earlier draft: a phone-app reference, "correct f1" / "wrong-f1" | That framed a repeatability campaign as mode identification | **Retracted.** Replaced by the outcome vocabulary above. |
| An earlier draft: one acoustic station; selection by Side A/B from `indexing_origin` | The rig has one station per actuator, and the user wants the convention kept local | **Retracted.** Replaced by A1–A3. |
| Unnamed but consequential | plunger dwell; spring-return re-impact; spoke and station variation; mount drift; idle-plunger rattle; electrical and structural coupling; 360 Hz mains harmonic inside the SNR annulus gap; pulse jitter under load; **wrong-actuator excitation** | Each is controlled, observed or gated below. |

## Variables

| class | variables |
|---|---|
| **Eliminated first** | pulse-end jitter; boot actuation when PRESENT=0; station/actuator choice driven by synthetic navigation; hand-pluck fallback (A10) |
| **Swept, physical** | pulse width **per actuator**, coarse, inside that actuator's B2 bracket; standoff only if B2 is ambiguous |
| **Swept, offline** | `gate_start_ms`, `window_ms`, `prominence_db`; next-onset margin if truncation is observed; ≤2-field grid; selection-shift candidates excluded |
| **Controlled, declared per station** | solenoid, mount, MOSFET channel, standoff, strike point, station angle |
| **Controlled, declared shared** | supply voltage, mic position, board power (power bank), build |
| **Observed per trial** | station/actuator and selection mode, measured pulse µs, strike offset, pre-roll delivered, overruns and worst read gap, onset count, truncation, status, reported peak, SNR, load arm, drift checks |
| **Blocked or randomized** | station; spoke (exploration / held-out / reserve per station); condition order randomized within a spoke; controls every 4th trial |

## Campaign stages (B0–B4 names kept)

- **The record.** `docs/SOLENOID_CAMPAIGN.md` holds: the rig registry per station, the S0 and LEFT/RIGHT convention, the pre-registered rules and vocabulary, and the stage log.
- **Bundles** go under `captures/_campaign/<stage>/` with a trial manifest.
- **Evidence key:** (station, chain digest, excitation digest, station rig_id).
- **Retention:** nothing is deleted; exclusions only under pre-declared codes.
- **Sequencing:** each station may move through the B2–B3.4 **campaign trials** on its own if its hardware lags, with identical protocol and rules. These trials run outside a session. **Session-path work** — Rung 2, Rung 3, the demo — needs both actuators, per A10.

### B0 — both driver channels + physical mounting + labelling *(halt-and-ask: physical)*

**Channels.** LEFT and RIGHT, built identically on IRLB4132; the third MOSFET is a spare. Source: [Infineon IRLB4132 datasheet](https://www.infineon.com/dgdl/Infineon-IRLB4132-DataSheet-v01_00-EN.pdf?fileId=5546d4626cb27db2016cd545a6182ff3).

Per channel:
- 100–150 Ω series gate resistor from that channel's GPIO
- **10 kΩ gate→source** resistor (mandatory)
- flyback diode across the coil (mandatory; V_DSS is 30 V)
- coil capacitor

Shared:
- **Supply and ground:** one supply, star ground; the Source returns and the ESP32 return meet only at supply −.
- **Tab:** it is the drain and electrically live, so keep it off the extrusion.
- **Handling:** ESD-safe.
- **Board power:** power bank during blocks.
- **Unattended blocks:** a ≤1 A polyfuse is required first; otherwise the operator stays present.

**Mounting (operator; done, Amendment 1).** Each solenoid on its own cut 4040 extrusion, sliding
along its own tower (sets strike point along the spoke's free span), with a 3D-printed fitting
between extrusion and solenoid (sets plunger-to-spoke standoff). Both eyeballed to roughly mid
free span. Per Amendment 1's C3, each fitting reaches the farther of its side's two spoke
classes — trailing, on both sides (corrected 2026-09-17; LEFT was first described backwards) —
the closer class then sits within reach as well; M3 tests whether one pulse serves both.

**Labelling (operator; done except S0, Amendment 1):**
1. Label the stations and solenoids LEFT/RIGHT. **Done.**
2. Mark as S0 the LEFT-leading spoke immediately counter-clockwise of the valve stem, viewed
   from the rotor side (RIGHT) — SPEC §6.4's rim-angle direction, C1.
3. Confirm S1 is reachable only from RIGHT.
4. Record all of it in the campaign record, including the donor wheel's disc-rotor side (RIGHT)
   and the observed RIGHT = Side A correspondence (C1, specific to this wheel's orientation).

### B1 — two-station firmware + attributable captures *(agent)*

- **B1a:** A1, A2, A4, A5, A7–A9, plus the instrumentation list under *Implementation*.
- **B1b:** A6, the navigation composite. It starts right after B1a and is on the critical path for the session-path rungs.

**Exit (T6):**
- **Pulse:** width distributions quiet vs under `socket_pressure.py` + PCM fetches, before and after the fix. After the fix, |measured − commanded| ≤ 0.5 ms on **both** channels.
- **Selection:** declared spokes 0..3 fire LEFT, RIGHT, LEFT, RIGHT. Never two fires in one measurement.
- **A10 on target:** with one PRESENT=0, `START_TRUING` is refused with `EXCITATION_UNAVAILABLE`, and `MEASURE_ONCE` at the absent station is rejected with the same reason. A failed fire yields a rejected attempt with no ARMED frame.
- **Positioning:** prompts name the correct station.
- **Capture fields:** present, with 409 until complete.
- **Debug:** `MEASURE_ONCE` is refused during a session.
- **Boot:** quiet with PRESENT=0.

**Supports:** "instrumentation, positioning routing and selection are trustworthy". Nothing acoustic.

### B2 — actuator bench, per station *(operator observes and handles; agent drives and logs)*

**Question:** does each actuator's electrically successful pulse produce one clean strike on the spoke positioned at its station, and on nothing else?

**Order (Amendment 1, C4):** M3 runs first — it is the cheap, no-firmware test of whether one
pulse per station serves both spoke classes, and G-class depends on it.

Per station:
- **E1** Shared parts: coil ≈40 Ω, rail at 12 V, diode orientation, grounds. (The Darlington's no-fire was never explained, so these are checked first.)
- **E2** 500 ms hold: V_DS < 50 mV.
- **E3** 200 fires quiet and 200 under load, measured widths logged, no heating.
- **E4** Quiet boot.
- **M3 (run first)** Pulse bracket, **per class** (Amendment 1, C3–C4): `p_reach` = strikes 10/10; `p_dwell` = longest pulse before dwell or double hit. The station's bracket is the **intersection** of its two classes' brackets — `p_reach` is the max over classes (farther class governs), `p_dwell` is the min over classes (closer class governs). An empty intersection triggers **G-class** (see below).
- **M1** Strike geometry, **per class**: mid free span, single contact, retracts, no rattle. Trailing (diagonal-presenting) spokes are checked for glancing or sliding contact; on RIGHT, plunger clearance to the rotor is checked for both classes.
- **M2** Standoff, **per class**: record mm, fitting id and tower height. "Contact late in the stroke" is required for the farther class only; the closer class's contact point is recorded as observed.
- **M4** 200 strikes: drift ≤0.5 mm, bracket unchanged. The 3D-printed fitting and the extrusion clamp are the named creep suspects.
- **M5** **Station angle and reach, per class:** leading and trailing spokes need different rotations to land under the plunger; both are recorded. Manual positioning in Stage 1 is unaffected by this — it is recorded for future navigation.

Two-station checks:
- **M6 Wrong-actuator hazard, all four classes:** with a spoke positioned at its own station, record what sits under the *other* station's plunger. If another spoke is there, a wrong-actuator fire would excite a different spoke without warning. That makes the per-trial attribution checks mandatory; it is not a reason to redesign.
- **M7 Idle rattle:** when one actuator fires, does the other's plunger rattle? The operator listens. If it does, fix the retention or carry it as an observed variable.
- **M8 Attribution end to end:** runner positions S0..S3 at their stations. The observed plunger must match the logged `actuator` on every step: LEFT, RIGHT, LEFT, RIGHT. **Also verifies the Amendment 1 C2 class map:** S0..S3 must be L-lead, R-lead, L-trail, R-trail — if not, halt, the map is wrong.

**G-class (Amendment 1, C4) — a user decision, triggered by an empty M3 intersection at either station, or by the equivalent B3.2 condition:**
- A per-class pulse would require the acoustic subsystem to know spoke class — an architecture change (excitation keyed by class), not a campaign parameter, so it is not built ahead of the evidence.
- Options put to the user on trigger: re-seat the fitting and re-run M2–M3; restrict demo/acceptance scope to the working class at that station; or authorize per-class excitation (the C2 class map already supports it).

**Exit:**
- Each station that passes → flip its PRESENT to 1 (board-profile change, T5).
- Update the measured station angles in the machine profile (a new profile, T5).
- **Station rig_id** (Amendment 1, C10) now includes the fitting id, the tower height and both per-class standoffs; changing any of them invalidates that station's confirmation.

**Supports:** mechanical/electrical delivery and attribution per station. **Not:** acoustics.

### B3.0 — controls and coupling *(automated; operator rotates the wheel)*

**Question:** does either actuator path inject anything the DSP could clear?

- **Controls:** 20 no-fire + 10 **air shots per station**, interleaved. An air shot fires with the wheel rotated so the plunger lands between spokes; the rig is unchanged.
- **Automated spectra:** in-band peaks, and any coherent line within ±5 Hz of 360 Hz.
- **Pass:** 0 false clears in 40, and no coherent in-band line in ≥50 % of controls.
- **If it fails:** one discriminating block (the offending solenoid decoupled on foam vs mounted), fix, rerun.

**Supports:** non-spoke emissions don't produce clears. **Not:** anything about strikes.

### B3.1 — spoke sets *(agent, before any strike data)*

- **Rule, declared up front (Amendment 1, C5 — class-balanced, operator's choice):** draw per station 4 exploration spokes (2 per class), 4 held-out (2 per class), 2 reserve (1 per class), spread around the wheel within each class. Record the seed.
- **No reference measurements.** Consistency is judged against each spoke's own repeated clears, so no outside frequency reference is needed.

### Pre-registration *(agent; committed before any B3.2 data)*

Commit to `docs/SOLENOID_CAMPAIGN.md`: the vocabulary, the selection rules, the shared-vs-per-actuator rule and all acceptance criteria. The commit hash is cited in every stage report.

### B3.2 — exploration *(automated blocks; operator rotates the wheel and does drift checks)*

**Question:** which per-actuator pulse and shared DSP config gives discriminated, consistent clears robustly across spokes at both stations?

**Physical:**
- Per station, 3 pulse levels at 25/50/75 % of that bracket, plus 20 ms if it falls inside.
- **(Amendment 1, C6)** 6 strikes × level × 4 exploration spokes per station (2 per class), randomized; a no-fire control every 4th trial.
- ≈144–192 captures (3–4 levels), plus controls.

**Offline:**
- `native_sweep` over these captures, the B3.0 controls, historical pass-C and `nano_ambient_ambiguous`.
- Then the ≤2-field grid.
- Selection-shift candidates are excluded.

**Selection rules (pre-registered):**
1. **Constraints:** 0 false clears on every control set; 0 inconsistent clears; 100 % attribution; no selection shift.
2. **DSP (shared):** maximize the worst spoke's consistent clear rate **across both stations**.
3. **Pulse:** shared if one level lies inside both stations' constraint-satisfying plateaus, with each station's worst spoke within 10 pp of that station's best. Otherwise per actuator, each chosen by its own station's worst spoke.
4. **Tie-breaks:** plateau interior, then fewest fields changed from baseline, then shorter pulse.
5. **Refinement:** at most one extra pulse level per station, only at a bracket edge or a >25 pp cliff.
6. **(Amendment 1, C4/C6) Class check:** the pulse rule also evaluates each class's worst spoke separately. If every constraint-satisfying level leaves one class's worst spoke more than 10 pp below the other class's at a station, that feeds **G-class** (defined under B2), not this rule's ordinary tie-break.

**Blind human review:** ~24 stratified trials across both stations, recording strike audible / ring in analysed window / second impact or rattle / clean excitation **/ metallic or rotor ring audible (Amendment 1, C8 — RIGHT station only; the rotor is a RIGHT-only structural resonator that air shots can't excite, so it can only appear in strike captures)**. No pitch judgments. The review **does not select** anything. A systematic "clear without ring" stops the stage.

**Gates:**
- **Exploration gate (Amendment 1, C6: was ≥6/8):** each station needs a candidate with worst spoke **≥5/6**. If not, one bounded mechanical lever (mic distance or strike point) and a reduced rerun. Still failing → stop, report the ceiling, and give that station's status to the user.
- **G-analysis:** if no shared DSP candidate meets the constraints at both stations, halt. A per-station analysis profile is an architecture decision.
- **G-class (Amendment 1, C4):** defined under B2; also triggered here if the class check above (rule 6) fails at every level for a station.

**Supports:** "config X is the best candidate on exploration data". **Not:** reliability.

### B3.3 — freeze *(halt-and-ask: constant changes)*

1. Agent presents the selection report.
2. After approval, commit the chain profile DSP fields and the per-actuator excitation profile in `fixtures.c`. Digests move.
3. Build and flash the campaign and demo images.

### B3.4 — Rung 1: held-out confirmation, both stations, under integrated load *(automated; operator positions spokes, UI open)*

**Question:** does the frozen config meet acceptance on unseen spokes at both stations, under realistic firmware load?

**Loaded arm (acceptance):**
- **(Amendment 1, C7: was 3 spokes × 8 strikes)** 4 held-out spokes per station (2 per class) × 6 `MEASURE_ONCE` strikes: runner-declared physical spoke, derived station and actuator, split across ≥2 separate blocks. 48 trials.
- Browser UI streaming and `socket_pressure.py` running throughout.
- ≥30 no-fire controls under the same load.

**Quiet arm (mechanism comparison only):** 3 strikes per held-out spoke, 24 trials. UI closed, no traffic, and the runner waits for each harvest before firing. Interleaved with the loaded arm per spoke.

**Acceptance (pre-registered):**
- 0 false clears; 0 inconsistent clears (band from each spoke's loaded-arm clears).
- Attribution: `actuator` = the derived station of the declared spoke on 100 % of trials.
- Consistent clears ≥ **40/48** (Wilson 95 % lower bound 0.70). No station below 18/24.
- **Per-spoke floor (Amendment 1, C7: was 6/8): no spoke below 5/6.**
- **Class floor (Amendment 1, C7, new): no class below 10/12.** The demo covers every class, so one weak class would break Rung 3.
- Board == host replay parity on 100 % of trials; overruns only ever surface as `CAPTURE_OVERRUN`.
- Blind review of 20 stratified trials: 0 machine-clears the operator hears as no strike or no ring.
- **Reported, not gated:** each held-out spoke's median shift between blocks.

**Load verdict:**
- **Harmless:** 0 unflagged corruption; pulse within 0.5 ms; worst read gap < 100 ms; each spoke's quiet-arm and loaded-arm medians within the consistency band; strike offset consistent.
- **Invalidating:** a parity mismatch; an unflagged overrun; a pulse outside the bound; a loaded-only median shift.

**If it fails:** no tuning on this data. One return to B3.2, then fresh confirmation on the reserves plus new spokes. A second failure → stop and report the ceiling per station.

**Supports:** "on this rig, wheel and room, config X yields discriminated, self-consistent readings on held-out spokes at both stations at ≥ the observed rate, under UI/network load, with no false or wrong-actuator excitations (n stated)". **Not:** that any reported peak is the fundamental or any particular mode; tension accuracy; other wheels or rooms.

### Rung 2 — session path, both stations *(needs B1b)*

**Question:** does the orchestrator session path route spokes to their stations, select the right actuator, and acquire?

- **Run:** 2 sessions × N = 8 bounded spokes (alternating stations), under load. `session_capture.py --auto` answers the non-positioning waits.
- **Pass:**
  - 0 inconsistent clears (band across each spoke's clears in both sessions)
  - ≥15/16 spoke tasks succeed within budget
  - the prompted station matches the derived station, and `actuator` matches the positioned spoke, on 100 % of attempts (joined with the positioning log)
  - both actuators are exercised

### Rung 3 — full wheel *(demo rehearsal)*

- **Run:** N = 32 × 2 passes via the same path.
- **Pass:** 0 inconsistent clears; ≥63/64 spoke passes; every spoke succeeds in ≥1 pass; 100 % attribution.
- **Supports:** full-wheel demo scope, as direct rehearsal evidence.

### Gates (user decisions, with evidence)

- **G-analysis (B3.2):** shared DSP config, or a per-station analysis profile.
- **G-demo (after the highest rung reached):** demo N = 1 / 8 / 32.
  - **Descope floor if B1b slips:** N = 1, with physical S0 placed at the LEFT station before START. Logical spoke 0 is then physical S0 on its correct actuator, and the two-station evidence is shown from the campaign report. Both actuators must still be present for admission (A10).
  - **If one actuator is broken at demo time:** no session can run. The fallback is to present the campaign evidence without a live session, since a demo never depends on debug intents. The alternative is for the user to explicitly revisit A10 — never a silent downgrade.
  - A demo never depends on debug intents (§12.5).

### B4 — re-baseline fixtures *(agent; on-target check)*

- **Promote** from confirmation, per station: a consistent clear and a rejection; plus a no-fire control, and an overrun if one occurred.
- **Retire** `nano_handpluck_provisional`.
- **Golden/ambient fixtures:** they replay under the build's profile, so the retune makes their expectations stale. Re-baseline under the committed profile with an on-target replay check, or document their recorded-profile scope. This is a pre-existing consequence of any retune, handled here and not widened.
- **Docs:** README schema, `IMPLEMENTATION_NOTES` findings.
- **Smoke:** demo-image session with the debug channel off, labelled smoke.

## Acceptance derived from the demo

Per-attempt consistent-clear probability p, 3 attempts per spoke: s = 1 − (1 − p)³.

For all N spokes to succeed with 90 % probability:

| N | p ≥ |
|---|---|
| 1 | 0.54 |
| 3 | 0.675 |
| 8 | 0.76 |
| 32 | 0.85 |

This assumes identical, independent spokes, which is false: the worst spoke and the worse station dominate. That is why Rung 1 states a bound and Rungs 2–3 run coverage directly.

**Validity** is mandatory: 0 false clears, 0 inconsistent clears, 0 wrong-actuator. **Reliability** only sets how many spokes the demo may claim.

## Shared observed reality (human ⇄ machine)

- **Review packet** (`tools/review_packet.py`, reusing `capture_wav.py`):
  - `full.wav` + `window.wav`, cut at the board's own `expect_window_*`
  - raw PCM24 plus a normalized copy labelled with its gain
  - WAV INFO chunk: trial_id, bundle, seq, `pcm_sha256`, window, both digests, station, rig_id, variant
  - shuffled order; machine verdict sealed; categorical, pitch-free response sheet keyed by trial_id
- **Join:** `--join` re-hashes and **refuses on any mismatch**; emits an agreement matrix; disagreements are kept.
- **Capture view:** gains `complete`. `/debug/capture.*` returns 409 until that seq's outcome is recorded, which closes the ~3 s window where new PCM carried the previous verdict. Fetch tools require `complete` and a matching seq.

## Provenance: what invalidates what

| change | tracked by | invalidates |
|---|---|---|
| chain-profile field (DSP, capture) | `chain_digest` (per-capture snapshot) | fixtures; confirmation at **both** stations |
| excitation pulse for one actuator | `excitation_digest` + `station` | that **station's** confirmation only |
| pulse mechanism, DSP algorithm code, selection-rule version | build rev / version; parity replay | exploration comparability if changed mid-B3.2; confirmation if changed after B3.4 |
| one station's solenoid, mount, MOSFET channel, standoff, strike point | **declared station rig_id only — no digest sees these** | that station's confirmation |
| **(Amendment 1, C10) fitting id or tower height, per station** | **declared station rig_id** | that station's confirmation, both classes |
| station angles | `machine_profile_id` (session header) | positioning/attribution evidence at that station |
| supply V, mic position, board power | declared shared rig registry | both stations' confirmation |
| physical S0 marking, LEFT/RIGHT labels, board station↔pin assignment | rig registry, build | all spoke-indexed attribution; rerun B2 M8 |
| **(Amendment 1, C10) the C2 class map (S0's class, or the declared i mod 4 pattern)** | rig registry (campaign record) | all class-stratified results; rerun B2 M8 |
| I2S/drain config, task priorities, network stack | build rev | the B3.4 load verdict |
| room / weather | interleaved controls | nothing, unless controls start clearing |

## Schedule, descope, stop

**(Amendment 1, C11)** B0's mounting turned out to already be done; only the S0 mark and campaign-record update were outstanding, closed 2026-09-17. B1a and its T6 (the hardware-timed pulse) closed 09-16 as planned. B2 was blocked on mounting until 09-17 — one day later than the original schedule — so everything from B3.0 onward absorbs the 09-20 reserve day to keep the same demo date.

| date | work | gate |
|---|---|---|
| 09-14→15 | B0 (channels + mounting, discovered done) ‖ B1a | — |
| 09-16 | B1a T6 (done) | pulse-timing evidence |
| 09-17 | B0 close (S0 mark); B2 LEFT and RIGHT, **M3 first** | PRESENT per station; station angles; G-class check |
| 09-18 | B3.0, B3.1, pre-registration ‖ B1b | controls pass |
| 09-19 | B3.2 + offline sweep + selection ‖ B1b | exploration gate, G-analysis, G-class, B3.3 approval |
| 09-20 | B3.4 Rung 1 ‖ B1b T6 + spec review (absorbs the former reserve day) | acceptance |
| 09-21 | Rung 2 | |
| 09-22 | Rung 3 | G-demo |
| 09-23 | B4, docs, smoke — **last campaign data day** | |
| 09-24→25 | buffer / demo prep | |

**Descope order:** Rung 3, then Rung 2. N falls to the evidence reached, with N = 1 as the floor. If one station's hardware or acceptance fails, continue that station's campaign trials only and say so. Session rungs and the demo session need both stations (A10).

**Never dropped:** controls, pre-registration, held-out confirmation, parity, attribution checks.

**Stop:** the campaign ends at B4 at the highest rung reached.

**Out of scope or deferred:**
- **Separate campaigns:**
  - fundamental / mode identification, the frequency↔tension relation, `L_eff`, an absolute-tension reference and calibration rig (possibly outside this repo)
  - the mounting-side designation (Side A/B, hub/flange/centering characterization) and reconciling physical S0 with the solver's `indexing_origin`
- **Not now:** averaging / `sigma_n`, preflight gate, `zero_pad_factor`, magnetic pickup, per-spoke tuning, other wheels or rooms, long-term drift, more than two actuators, the DSP core split (§4.5 Phase 2), and a per-station analysis profile unless G-analysis opens one.

## Implementation

### B1a firmware (`autonomous_truing_machine/`)

1. **Convention and routing**
   - `lib/truing_hal/include/truing_hal/acoustic_if.h`: `truing_acoustic_station_for_spoke()`.
   - `lib/truing_orchestrator/src/orchestrator.c` `next_measurement_target()`: use it instead of `TRUING_STATION_ACOUSTIC`.
2. **Stations**
   - `lib/truing_core/include/truing/config.h` + `config.c` + `config_blob.c`: `ACOUSTIC_LEFT` / `ACOUSTIC_RIGHT` replace `ACOUSTIC`, strings, validation requiring both, blob schema bump.
   - `lib/truing_fixtures/src/fixtures.c`: labelled placeholder angles and `reference_station = ACOUSTIC_LEFT`.
3. **Excitation profile:** `truing_excitation_profile_t` (validate, digest, blob — same bump); remove `excitation_pulse_ms` from the chain profile (`chain_profile/2`); fixtures provide 20/20.
4. **Actuator table** (`lib/truing_hal/src/acoustic_real.c` + `.h`): keyed by acoustic station; init takes the table plus the excitation profile.
   - `measure_run` derives the station, fires only that seam.
   - Records station, selection mode, measured pulse, fire timestamps, digest snapshots and `complete`.
   - Per-call override for the debug path only (pulse, no-fire, forced station for air shots), restored before return.
5. **Hardware-timed pulse**
   - `src/pluck_gpio.c`: pulse end from a hardware timer (gptimer one-shot ISR or RMT); `fire()` still blocks; measured µs report.
   - `pluck_if.h`: additive optional fire report.
   - `pluck_fake.c`: deterministic report.
6. **Audio capture report** (`audio_source_if.h` + `src/audio_i2s.c`): pre-roll delivered, ring-head µs, overruns, worst read gap. Fields a source doesn't provide are marked unavailable (§17.3 #54).
7. **Telemetry, wire and UI**
   - `telemetry_if.h`, `telemetry_ring.c`, `lib/truing_proto/src/wire_encode.c`: `actuator` field; new station strings.
   - `src/web_ui.h`: station names in positioning and pluck wording, tolerant of the absent field.
8. **Capture endpoint** (`src/net_transport.c`): capture.json schema `/2`, new fields, 409 while incomplete.
9. **Board and composition root**
   - Board headers: LEFT/RIGHT GPIO and PRESENT macros.
   - `src/orch_demo.c`: wire two `pluck_gpio` instances under PRESENT, the descriptor naming them, banner, remove the stale `plucks_commanded` log.
   - `src/bringup_acoustic.c`: per-actuator probe, PRESENT-gated.
10. **Campaign debug channel**
    - `src/build_mode.h`: `TRUING_CAMPAIGN_DEBUG`, legal only with FAST_DEMO+REAL_FRONT_END; mode string `+campaign`; `debug_channel_enabled` set in `orch_demo.c`.
    - `MEASURE_ONCE`: declared spoke, per-actuator pulse override, no-fire, forced station. Packed into the existing `debug{code,arg}` payload and recorded unpacked.
    - `operator_intent.c`: DEBUG admitted only in READY with no session.
    - `platformio.ini`: `nano_esp32_fastdemo_mic_campaign`. The demo image never enables it.
11. **Parity:** `test/test_acoustic_sweep` gets a `baseline` row that checks board == host over a directory.
12. **A10 — remove the hand-pluck path**
    - **Readiness contract:** `truing_acoustic_ready()` in `acoustic_if.h`, implemented by real, synthetic and recorded acoustic.
    - **Admission:** `session_admission` calls it.
    - **Reason code:** `TRUING_REASON_EXCITATION_UNAVAILABLE` in `lib/truing_core/include/truing/status.h`, plus its string, wire string and UI rejection phrase.
    - **Delete:** the ARMED phase and `pluck_lead_ms` / `truing_acoustic_real_set_pluck_lead` (`acoustic_real.c/.h`, `orch_demo.c:431`, `pluck_lead_delay`); `TRUING_EXCITATION_HAND`, with telemetry keeping `actuator` plus a `fired` flag in place of `excitation` / `pluck_commanded` (`telemetry_if.h`, `telemetry_ring.c`, `wire_encode.c`); the ARMED/hand cues in `src/web_ui.h`.
    - **Update:** the `build_mode.h` image descriptions; the board header comment; the I3 honesty tests in `test_acoustic_real`, rewritten so a failed fire leads to a rejected attempt; `test_proto_wire`; `ui_check.js`.
    - **Recapture:** `test/fixtures/ui/session_fastdemo_auto.json` from the **synthetic** `nano_esp32_fastdemo` image, whose fake actuators are attached at both stations, so no hardware is needed. Update its README.
13. **Fixture migration (3 checked-in fixtures)**
    - The pre-change commit's green T2 proves each stored `/1` digest matches the unchanged DSP values.
    - Write `chain_digest` `/2` from those same values.
    - Keep the original as `chain_digest_v1` (recorded history).
    - Documented in the captures README.

### B1b firmware

- **Navigation composite** (`lib/truing_hal/src/navigation_*`): SPOKE targets at either acoustic station → manual; RIM targets → synthetic. One authority object; rotation re-anchored at each operator confirmation; the descriptor records the mix.
- **Wiring:** `src/orch_demo.c` `wire_acquisition(automatic)` for the fast-demo acoustic image; `TRUING_ACOUSTIC_DEMO_SPOKES` bound chosen at G-demo.
- **Review:** spec-reviewer (§10A, §6.2).

### Tools

- **New**
  - `tools/campaign_runner.py`: drives blocks, prompts the operator, writes a manifest (physical spoke, station, rig_id, arm, drift checks), harvests. Reuses `campaign_recorder.py` / `capture_fetch.py`.
  - `tools/campaign_report.py`: consistency bands, stage metrics, selection rules, acceptance, attribution join, exclusion ledger.
  - `tools/review_packet.py`.
- **Updated**
  - `capture_wav.py`: INFO chunk.
  - `capture_fetch.py`: `complete` and new fields.
  - `tools/nano_serial.py`: survives USB re-enumeration.
  - `probe/session_capture.py`: prints actuator and station.
  - `ui_check.js`: station wording, plus replay of the old fixture.
- **Committed:** `tools/bench/solenoid_smoke`, with a selectable GPIO and serial-selectable widths.

### Docs

- **New:** `docs/SOLENOID_CAMPAIGN.md` and `docs/EXPERIMENT_METHOD.md`.
- **Updated:**
  - `docs/DEV_NOTES.md`: Working-loops router line.
  - `docs/BUILD.md`: campaign env, per-actuator PRESENT.
  - `IMPLEMENTATION_NOTES.md`: A1–A10 decisions; the §11.6/§16 station reconciliation; the §13.2 `EXCITATION_UNAVAILABLE` reconciliation; and, under open items, **Known limitation 1** (S0 vs `indexing_origin`, which must be reconciled before any physical truing session).

**Reuse, not rebuild:** `capture_wav.py`, `native_sweep`, `campaign_recorder.py` pairing and seq checks, `session_capture.py --auto`, `probe/socket_pressure.py`, the `test_acoustic_replay` digest gate, `pluck_fake` failure injection, machine-profile station records and station-naming prompts (§10A.3), manual/synthetic navigation.

## Rules for `docs/EXPERIMENT_METHOD.md` (generalized)

1. Verify the mechanism before designing a sweep; a knob the architecture makes moot is not swept.
2. Remove cheaply removable uncontrolled variables before characterizing them.
3. Classify every variable: physical, replayable, controlled/declared, observed, or eliminated. Never spend hardware time on what replay can answer.
4. Record observed variables at the source, per trial; "assumed" is not an observation.
5. Derive acceptance from downstream use, keeping validity (no wrong answers) separate from reliability (how often).
6. Discriminate against interleaved controls, not pass rate.
7. Use fixed n per condition when estimating rates; outcome-dependent stopping biases them.
8. Separate exploration, selection and confirmation. Pre-register rules before the data that judges them. Keep held-out units. Never tune on confirmation data. Allow one bounded return, then stop.
9. **The claim boundary lives in the vocabulary.** Define outcomes operationally (discriminated, self-consistent). Never name a metric after a property the campaign cannot establish ("correct", "fundamental"). Reject candidates that would answer another campaign's question.
10. References must be independent of what is selected, or the campaign must not need one. Prefer internal-consistency criteria over borrowed references that smuggle in claims.
11. Shared observed reality: human artifacts are hash-linked to the exact bytes and window the machine judged, variants labelled, review blind, judgments keyed by trial. A mismatch refuses the join; disagreements are kept.
12. Humans for perception, physical action and judgment; the agent for repetition. Human review validates machine categories and does not select.
13. Validity is layered: electrical success ≠ mechanical effect ≠ correct attribution ≠ valid measurement. Each layer needs its own evidence.
14. Integrated load: justify the failure mode from the architecture first. Compare mechanism metrics and outcomes with interleaved quiet/loaded runs. Bound "harmless" using earlier stages. Prove determinism claims (e.g. parity) rather than assuming them.
15. Negative evidence is data; exclude only under pre-declared codes.
16. Anomaly triage: does it threaten validity, the metric, or an assumption? If not, log and defer. If so, run the smallest discriminating experiment.
17. Coarse-to-fine is bounded: one refinement round per axis, with a stated trigger.
18. Know what provenance can't see. Digests cover configuration, not physical rigs, so declare rig identity per independently changeable unit.
19. Partition provenance by what a change affects (analysis config / actuator config / physical rig / unit identity) so a change invalidates only the evidence that depends on it.
20. Design for the intended physical configuration from the outset, even when characterizing one unit at a time; key evidence by the unit that can differ.
21. **Keep identity conventions local to the subsystem that needs them.** Don't couple them to broader designations owned by other campaigns, even when they are probably equivalent. Record the unexamined relation as an explicit cross-campaign dependency.
22. A synthetic stand-in must never drive a physical action. Simulated state that selects what hardware does is a correctness bug; verify attribution end to end on every trial.
23. Write stopping criteria, descope order and the out-of-scope list (including adjacent campaigns) before starting. Claims name their scope (rig, unit, n) and their status ceiling.

## Verification

**T2** — `pio test -d "$env:USERPROFILE\truing_ws" -e native`, with new tests:
- **Stations and selection**
  - `truing_acoustic_station_for_spoke` alternates LEFT/RIGHT from spoke 0
  - the orchestrator positions each spoke at that station, and the wait prompt names it
  - a spoke assigned to LEFT fires only the LEFT seam; RIGHT likewise
  - **A10:** `truing_acoustic_ready` is false when either seam is NULL or unavailable; START is refused with `EXCITATION_UNAVAILABLE`; a NULL seam or failed fire yields `rejected/EXCITATION_UNAVAILABLE` with no capture, and the previous capture is preserved; no ARMED phase is ever emitted
  - a failed fire at one station doesn't affect the other
  - never two fires in one measurement
  - per-actuator pulse applied
- **Config**
  - validation requires both acoustic stations; blob round-trip
- **Digests**
  - chain digest unchanged by an excitation change, and vice versa
  - the snapshot is unaffected by later changes
- **Debug path**
  - override restored
  - DEBUG refused outside READY / during a session / when disabled
- **Capture and wire**
  - capture view incomplete between capture and outcome
  - unavailable audio-report fields marked
  - wire encodes `actuator` and the new station strings
  - `test_acoustic_replay` green after migration

**Other tiers**
- T1 — `ui_check.js`: station wording, old-fixture replay.
- T0 — new build flag.
- T5 — `platformio.ini` env, board macros, station enum/blob, PRESENT flips, machine-profile angle update.

**Spec-reviewer**
- B1a: decision ownership of the station, station reconciliation, profile split, debug channel, capture schema.
- B1b: navigation authority.

**T6 via bench-verifier**
- pulse load check on both channels, before and after the fix
- spokes 0..3 routed to LEFT/RIGHT/LEFT/RIGHT with the matching actuator
- one-absent behaviour
- 409 gating, `MEASURE_ONCE`, quiet boot

**Tools**
- `review_packet.py --join` refuses a stale WAV.
- `campaign_report.py` reproduces a hand-computed consistency band, an inconsistent clear and an attribution mismatch on a small manifest.

**Stage reports** cite the pre-registration commit, per-station rig_ids, build rev, bundle hashes, the attribution join and the exclusion ledger.

## Prior state (closed; details in repo)

- **Phase A:** cue inversion `3a99bfa`; WAV export `22c85df`; A7 closed hand-pluck fitting.
- **Interim:**
  - `b0c6a4c` / `454b28b`: lossless harvest
  - `ca6ebfb`: ambient SNR rejected
  - `fe9ac26` / `cb6cc02`: actuator seam gated on PRESENT
  - `0629aa1`: UI replay
- **Decisions carried forward:**
  - Regression fixtures come only from the final excitation.
  - `prominence_db=12` and window placement are candidates, not changes.
  - `max_peak_depth_db=20` is rejected: it produced clears at components 40–70 Hz away from the spoke's repeated cluster.
  - The 12 dB SNR gate sits in the measured control/clear gap.
- **Darlington:** abandoned; its unexplained no-fire is addressed only by B2-E1.
