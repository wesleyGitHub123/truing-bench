# Solenoid campaign record

The rig registry, the S0/LEFT convention, and the stage log for Phase B's two-station solenoid
integration. Pre-registered rules, outcome vocabulary and acceptance criteria live in
`SOLENOID_CAMPAIGN_PLAN.md`, the committed plan that governs this campaign; this file is the
running record of what was actually done and found, kept current as each stage closes.

Bench-tool findings that are exploratory rather than part of the campaign proper — the gate-
control (release ramp / brake / catch pulse) investigation into retraction noise — are recorded
separately in `tools/bench/README.md`, with a pointer back here for how that route was closed
out. Not duplicated in both places.

## Convention

**Spoke 0 is assigned to the LEFT actuator; assignment alternates from there** (spoke 1 → RIGHT,
spoke 2 → LEFT, ...). "LEFT" and "RIGHT" are rig labels only — this convention is deliberately
**not** coupled to Side A/B, cassette side, rotor side, or any other mechanical side designation.

**Amendment 1 (2026-09-17), observed correspondence for this wheel:** the donor wheel is a
**front disc wheel with the rotor on the RIGHT side**, so per SPEC §6.4.1 (Side A = rotor side),
**RIGHT = Side A and LEFT = Side B on this rig, in this orientation**. This is a rig fact, not a
convention change — it would flip if the wheel were mounted the other way round, and A3's
decoupling from Side A/B stands.

**Spoke classes.** Spokes alternate leading/trailing as well as side, giving four physical
classes. Counting counter-clockwise from the valve as viewed from the rotor (RIGHT) side — the
SPEC §6.4 rim-angle direction, which matches the operator's clockwise turning habit — the
declared class map is:

| i mod 4 | class |
|---|---|
| 0 (S0) | LEFT-leading |
| 1 (S1) | RIGHT-leading |
| 2 (S2) | LEFT-trailing |
| 3 (S3) | RIGHT-trailing |

This matches the solver's generator order at **offset 0**: `indexing_origin = (Side B, LEADING)`.
**Verified by B2-M8 (2026-09-18):** S0..S3 landed at LEFT, RIGHT, LEFT, RIGHT as declared, for this wheel in this orientation.
See `SOLENOID_CAMPAIGN_PLAN.md`'s Amendment 1 (C1–C2) for the full reasoning, and
`IMPLEMENTATION_NOTES.md`'s Known Limitations for what this does and does not settle for a real
physical truing session.

## Rig registry

**Rig IDs (Amendment 2, 2026-09-18).** Each station carries a versioned token, `<STATION>/rig-<n>`,
covering exactly the fields the plan's provenance table already says only the rig registry
tracks: solenoid, mount, standoff fitting, strike point. Current: **LEFT/rig-1**, **RIGHT/rig-1**.

- **This registry is append-only from this point on.** A change to any of those fields at a
  station gets a new dated block below the table, under a bumped token (`LEFT/rig-2`, ...); the
  old block is left as-is, not edited in place. The one exception is fixing a plain transcription
  error (e.g. the 2026-09-17 standoff-order correction below), which stays a same-token edit and
  says so inline, same as that one does.
- **Trial manifests should stamp this token per trial** (a field alongside `station`), so B3.2 and
  later evidence can be grouped by physical rig version without re-deriving it from dates. This is
  a note for whichever tool writes the manifest; `tools/campaign_runner.py` does so as of
  2026-09-18 (`campaign_rig_id`, plus `campaign_station`), rather than something this doc enforces
  on its own.
- **Why now, not later:** cheap to add before B3.2 data exists; expensive to retrofit once trials
  reference a rig state only by "whatever the table said that day."

| | LEFT | RIGHT |
|---|---|---|
| **rig_id (current)** | **LEFT/rig-1** | **RIGHT/rig-1** |
| board GPIO | 9 (D6) | 10 (D7) |
| board macro | `BOARD_PLUCK_ACTUATOR_LEFT_GPIO` | `BOARD_PLUCK_ACTUATOR_RIGHT_GPIO` |
| PRESENT (current) | **1** (set 2026-09-18, after B2; `16c38e2`) | **1** (same) |
| MOSFET | IRLB4132 (logic-level, TO-220) | IRLB4132 |
| solenoid | JF-0530B, 12 V / 300 mA / 5 N / 10 mm stroke | JF-0530B |
| gate resistor | 100–150 Ω series | 100–150 Ω series |
| gate–source resistor | 10 kΩ (mandatory) | 10 kΩ (mandatory) |
| flyback diode | present (mandatory; V_DSS 30 V) | present (mandatory) |
| mount | own cut 4040 extrusion on its own tower (sets strike point along free span) | own cut 4040 extrusion on its own tower |
| standoff fitting | 3D-printed, set to reach the farther class (trailing) | 3D-printed, set to reach the farther class (trailing) |
| strike point (current) | eyeballed, roughly mid free span | eyeballed, roughly mid free span |
| per-class standoff order | leading closer, trailing farther **(corrected 2026-09-17 — was reversed)** | leading closer, trailing farther |
| station angle | not yet measured (B2-M5, per class) | not yet measured (B2-M5, per class) |
| pulse bracket (B2-M3, 2026-09-17) | leading [40, 85] ms · trailing [40, 110] ms · **station [40, 85] ms** | leading [50, 135] ms · trailing [60, 95] ms · **station [60, 95] ms** |
| Side (observed, this wheel) | B | A (rotor side) |
| S0/S1 physical marking | S0 marked per C1; **confirmed at LEFT by B2-M8 (2026-09-18)** | S1 confirmed at RIGHT by B2-M8 |

**Donor wheel:** front disc wheel, rotor on the RIGHT side. Expected asymmetric per SPEC §972,
unverified.

**Shared, declared:** one 12 V / 8 A wall supply, one bulk electrolytic capacitor (470–1000 µF,
≥25 V) across the 12 V rail at the point it splits to both solenoids, star ground (source
returns and the ESP32 return meet only at supply −), Nano powered from a power bank during
bench blocks.

**PRESENT is flipped to 1 only after that station passes B2** (per the plan's execution-order
rule). Both stations passed and both are now 1 on the Nano (`16c38e2`); the DevKit has no
actuators installed and stays 0. Until that flip, every `*_mic` and `*_fastdemo_mic` image
correctly refused sessions (A10). Nothing has yet been run on target with PRESENT=1.

## Stage log

### B0 — driver channels: electrical validation, mounting, labelling (2026-09-14 → 2026-09-17)

**Bench tool used:** `tools/bench/solenoid_smoke` + `tools/bench/solenoid_ctl.py` (standalone,
keystroke-driven — see `tools/bench/README.md` for the full key reference). Not the real
firmware; no acoustic subsystem, no orchestrator, no session.

**What was checked and found:**

1. **Both channels fire independently.** Confirmed with individual `l`/`r` shots and `L`/`R`
   holds on the bench sketch. A loose connection on the RIGHT gate lead was found (isolated with
   a meter — RIGHT worked with the gate manually shorted to 3.3 V but not through the board
   wiring) and fixed; RIGHT fired correctly afterward.
2. **No cross-talk.** LEFT fires only the LEFT solenoid, RIGHT only RIGHT, confirmed by direct
   observation during individual shots and during the alternating run below.
3. **Attribution holds under an alternating run.** The `a` key (10 shots, alternating LEFT/RIGHT,
   3 s apart) was run and matched the logged order — L R L R ... — with no drops.
4. **V_DS confirmed on both channels.** The `A` key (one-person V_DS check: 4 s lead-in
   announcement, then a 4 s hold, 2 LEFT/RIGHT cycles) was used specifically because a solo
   operator can't reliably get a meter onto the right MOSFET and read it inside a keystroke-
   triggered instant shot. Observed on **both channels**: near the 12 V rail while off, dropping
   to near 0 V for the duration of the hold, and back to ~12 V once the hold ended — the expected
   shape for a MOSFET driven fully on by a logic-level gate. No numeric V_DS value was logged by
   the operator; the qualitative shape (large drop to near-zero, not a partial or absent drop)
   is what was confirmed. If a precise number is needed later (e.g. to compare against the
   4.5 mΩ R_DS(on) datasheet figure), rerun `A` and record the meter reading directly.
5. **No heating observed** on either MOSFET or solenoid after repeated firing during this
   session (qualitative, by touch; no thermal measurement taken).

**Amendment 1 (2026-09-17) — mounting and labelling were already done, not reported until now.**
Both solenoids have been mounted since before B0 (each on its own cut 4040 extrusion + a
3D-printed standoff fitting, per-fitting standoff set to reach the farther of that side's two
spoke classes) and the stations/solenoids are already labelled LEFT/RIGHT. This was not visible
in earlier stage-log entries because the operator hadn't yet described the rig in detail; see
`SOLENOID_CAMPAIGN_PLAN.md`'s Amendment 1 for the full physical description and its consequences
(spoke classes, the donor wheel's disc rotor, the S0/indexing-origin correspondence).

**Not done in B0, still pending:**
- Marking S0 (rule declared, Amendment 1 C1: the LEFT-leading spoke immediately
  counter-clockwise of the valve, viewed from the rotor side) and confirming S1 by exclusion.
- A quantified V_DS reading (see note above).
- Unattended-block polyfuse (currently: operator stays present during bench blocks instead).

**Exit assessment:** the driver channels are electrically sound — both fire cleanly, attribute
correctly, show no cross-talk, and switch fully on (V_DS collapses as expected). Mounting and
labelling are done. **B0 closes 2026-09-17**, with the S0 mark carried forward as the first
operator step of B2 rather than a B0 blocker.

### Gate-control (retraction noise) investigation — parked

Explored whether firmware-side gate control (release ramp, constant brake, a timed catch pulse)
could reduce plunger retraction noise. Findings, full detail, and the revisit trigger are in
`tools/bench/README.md`. Summary: a catch pulse worked in free air but its tuning is a function
of travel distance, which the spoke changes — parked rather than pursued further, revisit only
if B3.0's controls show actuator emissions actually reach the measurement. Not part of the B0
exit criteria; recorded here only so the stage log shows where the session's time went.

### B2-M3 — LEFT station pulse bracket (2026-09-17)

**Bench tool used:** `tools/bench/solenoid_smoke` (baseline `b` key, `+`/`-` width), same firmware
family as B0. S0 was marked (LEFT-leading, per Amendment 1 C1) before this ran.

**Method:** manual bisection on the 5 ms `+`/`-` grid. `p_reach` confirmed by 10/10 baseline
shots at the candidate width, all landing under one clean strike; `p_dwell` found by bisecting
upward from `p_reach` until dwell (plunger holds pressed against the spoke past a clean tap) or
a double-hit (spring-return re-strike) first appeared, then narrowing to the grid boundary.
Widths tested that failed `p_reach`'s 10/10 bar are not part of the bracket even if they showed
one or two hits (LEFT-leading's 35 ms hit 2/3 attempts, then missed — disqualified for exactly
this reason).

**Results:**

| class | p_reach | p_dwell | bracket |
|---|---|---|---|
| LEFT-leading | 40 ms (10/10) | 85 ms (clean at 85, dwell at 90) | [40, 85] |
| LEFT-trailing | 40 ms (10/10) | 110 ms (clean at 110, dwell at 115) | [40, 110] |
| **LEFT station** | | | **[40, 85] (intersection)** |

**No G-class trigger for LEFT** — the two classes' brackets overlap with 45 ms of margin, so one
pulse can serve both LEFT classes.

**Finding: the fixture's current default excitation (20 ms, `fixtures.c`) is below both LEFT
classes' `p_reach`.** At this rig's current standoff, a 20 ms pulse does not physically reach
the spoke at all on LEFT — confirmed directly (20 ms and 25 ms both fell short before the
sweep found 40 ms). This does not by itself invalidate anything already committed: B3.2's rule
("3 levels at 25/50/75% of the bracket, plus 20 ms if it falls inside") already excludes 20 ms
here since it falls outside [40, 85]. Flagged so B3.2's level selection doesn't need to
rediscover this.

**Correction to Amendment 1's physical description:** the operator's original description had
LEFT-trailing as the *closer* class; it is actually the **farther** one (leading is closer, by a
small margin) — the reverse of RIGHT was wrong, LEFT and RIGHT both have leading closer /
trailing farther. Corrected in the rig registry above and in `SOLENOID_CAMPAIGN_PLAN.md`.

**Not done yet on LEFT:** M1 (strike geometry per class), M2 (standoff mm per class), M4 (200-
strike drift), M5 (station angle per class).

### B2-M3 — RIGHT station pulse bracket (2026-09-17)

Same method as LEFT (manual bisection on the 5 ms grid, `p_reach` confirmed by a full 10/10
batch, `p_dwell` bracketed by single-shot redo-on-miss checks). RIGHT-trailing needed several
redo cycles near its `p_dwell` boundary (four consecutive misses at 95 ms) — consistent with the
diagonal-presentation alignment difficulty already noted for trailing-class spokes on LEFT, not
a new finding.

**Results:**

| class | p_reach | p_dwell | bracket |
|---|---|---|---|
| RIGHT-leading | 50 ms (10/10) | 135 ms (clean at 135, dwell at 140) | [50, 135] |
| RIGHT-trailing | 60 ms (10/10) | 95 ms (clean at 95, dwell at 100) | [60, 95] |
| **RIGHT station** | | | **[60, 95] (intersection)** |

**No G-class trigger for RIGHT** — 35 ms of margin.

**Reach-search note:** at 40/45/50/55 ms, RIGHT-trailing showed inconsistent hits (a mix of
hits and misses across repeated batches at each width) before 60 ms finally gave a clean 10/10;
50 ms alone scored 9/10 on a recheck batch. This reads as the unclamped-wheel alignment
variance discussed for LEFT-trailing, sitting close enough to the true reach threshold that hand
positioning could tip individual shots either way — not evidence that 60 ms itself is marginal
(60 ms scored 10/10 twice, once on the original search and once implicitly confirmed by the
dwell search starting from it).

**Same finding as LEFT:** the fixture's 20 ms default excitation is below `p_reach` on RIGHT
too (50/60 ms), for the same reason — it simply doesn't push the plunger far enough to arrive.

**M3 is now closed for all four classes, both stations.** Neither station needs `G-class`.

**Not done yet on RIGHT:** M1, M2, M4, M5 (same list as LEFT, above).

### B2-E3 — RIGHT channel, quiet and loaded (2026-09-17)

**Tool used:** `tools/bench/pluck_timing_check`'s `nano_pluck_timing_right` /
`_right_quiet` envs (the same real-driver T6 harness B1a used for LEFT, extended with a quiet
mode — see that tool's own commit for why). Fires the real `src/pluck_gpio.c` on RIGHT's GPIO
(10), 200 times at 20 ms commanded, with and without the two scheduler-pressure load tasks.

**Results (logs committed alongside the tool):**

| condition | fires | worst \|delta\| | mean measured | result |
|---|---|---|---|---|
| quiet | 200/200 | 18 µs | 20007 µs | PASS |
| loaded | 200/200 | 21 µs | 20007 µs | PASS |

Both comfortably inside the 0.5 ms bound; the loaded run's slightly higher worst-case is
consistent with the load tasks doing something, not a sign of trouble. **LEFT's own campaign-
stage E3 was closed out 2026-09-18** — see below; this note's earlier "not yet captured" is
stale as of that entry.

### B2-E3 — LEFT channel, quiet and loaded (2026-09-18)

**Tool used:** `tools/bench/pluck_timing_check`'s `nano_pluck_timing_left` / `_left_quiet` envs,
same harness as RIGHT. Fires the real `src/pluck_gpio.c` on LEFT's GPIO (9), 200 times at 20 ms
commanded, with and without the two scheduler-pressure load tasks. Builds already existed from
09-17 (`nano_pluck_timing_left*`); this run only needed flashing (3-region esptool write, never
`-t upload`, per house rules) and a fresh serial capture per condition.

**Results (logs committed alongside the tool, `e3_left_quiet_2026-09-18.log` /
`e3_left_loaded_2026-09-18.log`):**

| condition | fires | worst \|delta\| | mean measured | result |
|---|---|---|---|---|
| quiet | 200/200 | 17 µs | 20007 µs | PASS |
| loaded | 200/200 | 21 µs | 20007 µs | PASS |

Both comfortably inside the 0.5 ms bound, both channels now symmetric on E3. **B2-E3 marked done,
both channels.** This closes the one remaining gap in B2's electrical/pulse-timing checks — B2
has no open items left blocking either station's PRESENT flip except operator sign-off.

### B2-E4 — quiet boot, both channels (2026-09-17)

**Tool used:** `tools/bench/solenoid_smoke` (the bench sketch itself, not the T6 harness —
`pluck_timing_check` fires automatically by design and is the wrong tool for this check).

**Method:** three independent reset events (one from the flash's own `--after hard_reset`, two
deliberate `esptool --before default_reset --after hard_reset chip_id` no-op resets), operator
watching both plungers live through each one.

**Result:** zero movement on either channel across all three resets. Matches the sketch's own
design ("Fires nothing on boot; every activation is a keystroke") and confirms it holds in
practice, not just by source inspection.

**Scope, corrected 2026-09-19:** this was measured on the **bench sketch**, not on the firmware
images, and the sketch fires nothing at boot whatever `PRESENT` says. So E4 establishes the
*hardware* property — with the gate pull-downs fitted, neither MOSFET conducts through reset and
the ROM/bootloader phase — and it stays valid for that. It says nothing about application-level
firing. The firmware's bring-up (`src/bringup_acoustic.c`) deliberately fires one live measurement
per station whose `PRESENT` is 1 (plan A9), and both are now 1 (`16c38e2`), so **on a firmware image
each boot is expected to strike LEFT and then RIGHT once** — read from the code, not yet observed
on target. This applies to every image with a real front end, the demo image
`nano_esp32_fastdemo_mic` included. An earlier note of mine described E4 as "recorded at
PRESENT=0"; that overstated it — E4 never exercised the firmware at all.

### B2-E1 — shared parts, both channels (2026-09-17)

**Method:** operator multimeter, power applied but not firing.

**Results:**
- **Coil resistance:** ~20 Ω on **both** channels, fluctuating rather than settling to a stable
  value. Expected ≈40 Ω (JF-0530B, 12 V / 300 mA nameplate → R ≈ V/I ≈ 40 Ω). Not yet isolated
  to a specific connection (would need a direct-on-the-solenoid-leads probe to rule out a
  connector/terminal-block contact issue, the same class of fault as B0's loose RIGHT gate
  lead) — **operator decision: not pursued further now**, judged stable enough to proceed.
  **Flagged, not resolved:** if either channel later shows an intermittent no-fire, mis-strike,
  or measured-pulse anomaly during M1/M2/M4/M5/M6–M8, this reading is the first thing to
  revisit.
- **Rail voltage:** 12.2 V (shared supply, one reading covers both channels).
- **Diode orientation:** confirmed correct, both channels.
- **Grounds:** confirmed continuous to the star-ground point, both channels.

**E1 marked done** on the strength of the above, with the coil-resistance note carried forward
as an open, explicitly-deferred item rather than a resolved one.

### B2-E2 — 500 ms hold, V_DS quantified (2026-09-17)

**Tool used:** `solenoid_smoke`'s `A` key (one-person V_DS check — 4 s announced lead-in, then a
4 s hold, cycling LEFT/RIGHT twice). Same tool B0 used for the qualitative pass; this time a
number was actually read off the meter, closing the gap B0 left open.

**Result:** **0 mV on both channels**, read during a live 4 s hold (not estimated) — comfortably
under the 50 mV bound. Matches full turn-on for a logic-level MOSFET at these currents (the
4.5 mΩ R_DS(on) datasheet figure would produce a voltage below most meters' resolution at this
current, so 0 mV is the expected reading, not evidence the meter isn't working).

**E2 marked done, both channels.**

### B2-M1 — strike geometry, all four classes (2026-09-17)

**Tool used:** `solenoid_smoke`'s normal fire (`l`/`r`, not baseline) at a mid-bracket width for
each station — 60 ms for LEFT ([40, 85]), 80 ms for RIGHT ([60, 95]). 3 shots per class, all
observed live.

**Results:**

| class | shots | result |
|---|---|---|
| LEFT-leading | 3/3 | clean — mid free span, single contact, clean retract, no rattle |
| LEFT-trailing | 3/3 | clean — no glancing/sliding contact observed |
| RIGHT-leading | 3/3 | clean, rotor clearance confirmed |
| RIGHT-trailing | 3/3 | clean, rotor clearance confirmed — no glancing/sliding contact |

**Note:** the glancing/sliding risk flagged for trailing classes during M3 (Amendment 1, C3) did
not reproduce here. M1's deliberate, one-at-a-time positioning gave the operator time to align
properly before each shot, unlike M3's rapid back-and-forth width bisection under time pressure.
This is consistent with the mechanical (not pulse-width) explanation already settled on — a
positioning-stability issue that shows up more under a faster cadence, not a property of any
particular width. Out of scope for this campaign to fix (a clamp, or a firmer manual hold during
demo positioning, per the operator).

**M1 marked done, all four classes.**

### B2-M2 — standoff, per class (2026-09-18)

**Method:** caliper measurement, plunger face to spoke at rest, per class. Fitting id and tower
height recorded alongside (rig_id `LEFT/rig-1` / `RIGHT/rig-1`, per the registry above — unchanged
by this measurement, since M2 records the existing geometry rather than adjusting it).

| class | standoff (mm) | note |
|---|---|---|
| LEFT-leading | **6.34** (revised from an initial 7.7) | caliper orientation still clunky in the available space, but a better eyeball than the first pass |
| LEFT-trailing | **8.54** | same caliper-access constraint as leading; a better eyeball, not a firm read |
| RIGHT-leading | **8.38** | eyeball, same caliper-access constraint |
| RIGHT-trailing | **8.66** | eyeball, same caliper-access constraint |

**Contact timing (per M2's per-class rule):**
- LEFT-leading (closer class): not yet checked — no "late in stroke" requirement here, only the
  observed contact point is needed, still pending.
- LEFT-trailing (farther class — the one the fitting was set to reach): **confirmed late.**
  Method: push the plunger by hand to its maximum reach (full extension) and rotate the wheel
  until the spoke touches it, rather than firing and watching a live strike — static and
  repeatable, no timing judgment needed. Result: contact occurs only once the plunger is at, or
  very close to, full extension. The spoke sits well clear of the plunger everywhere short of
  that. **This is consistent with the fitting's farther-class reach working as intended for
  LEFT-trailing.**

RIGHT-trailing (farther class): **confirmed late**, same push-and-rotate method as LEFT-trailing —
contact only at, or very close to, full plunger extension.

**Open:** all four standoff values are eyeball reads, not firm caliper measurements. Both
farther-class (trailing) late-contact checks are closed; both leading (closer-class) checks remain
open with no requirement to close them.

**Dwell asymmetry (M3) — deprioritized, not pursued further.** M3 found RIGHT's closer class
(leading) tolerating a longer pulse before dwell (135 ms) than its farther class (trailing, 95 ms)
— the reverse of LEFT's pattern and of the stated rationale. Operator's own read on this
(2026-09-18): "kinda dwelled" was a borderline call made mid-bisection, the same cadence that
produced sliding/glancing false positives on trailing classes at M3 that M1's slower positioning
then failed to reproduce — so this is judged **low-confidence, not a settled finding.**
**Decision: not chased further now.** It doesn't gate anything — the station bracket rule (min
dwell / max reach across classes) is mechanism-agnostic and both stations already have working,
non-empty brackets; G-class never triggered. If the asymmetry is real, B3.2's per-level SNR and
clear-rate data (Amendment 2) is a much cleaner place to see it than an eyeball call under time
pressure. Logged here per EXPERIMENT_METHOD rule 16 (anomaly triage: log and defer when it
threatens neither validity nor an active gate).

**M2 marked done, both required checks closed for all four classes** — standoff recorded (eyeball
precision, noted throughout) and both farther-class late-contact checks confirmed late (LEFT-
trailing, RIGHT-trailing). The dwell asymmetry raised during M2 is logged as deprioritized, not a
blocker (see above).

### B2-M4 — 200-strike drift check, LEFT (2026-09-18)

**Tool used:** `solenoid_smoke`'s baseline fire (`b`), width set to 60 ms (mid-bracket, same width
used for M1) via the `+` jog. 200 fires, LEFT only. Board-reported: `LEFT fired 200`, 0 failures
flagged, width unchanged at 60 ms throughout — confirms the fix from B1a (hardware-timed pulse
end) held under 200 consecutive fires with no drift in the commanded width itself.

**Standoff drift (operator, caliper):**

| class | M2 (pre-M4) | post-M4 | Δ |
|---|---|---|---|
| LEFT-leading | 6.34 mm | 6.6 mm | +0.26 mm |
| LEFT-trailing | 8.54 mm | 8.5 mm | −0.04 mm |

Opposite-signed, both small — operator attributes this to eyeball measurement noise rather than
directional creep. **Pass** against the ≤0.5 mm criterion either way.

**Bracket-unchanged check:** not re-verified by a fresh reach/dwell sweep. Operator judged the
bracket unchanged from direct observation during the 200 fires, declining a formal 10/10 recheck
at 40 ms. **Recorded as an operator call, not re-measured evidence** — noted explicitly so this
isn't read later as equivalent in strength to the original M3 sweep.

**M4 marked done for LEFT.**

### B2-M4 — 200-strike drift check, RIGHT (2026-09-18)

**Tool used:** `solenoid_smoke`'s baseline fire (`v`), width set to 80 ms (mid-bracket, same width
used for M1) via the `+` jog from LEFT's leftover 60 ms. 200 fires, RIGHT only. Board-reported:
`RIGHT fired 200`, 0 failures flagged, width unchanged at 80 ms throughout — LEFT's counter
(200) unaffected, confirming per-channel fire counts stay independent.

**Standoff drift (operator, caliper):**

| class | M2 (pre-M4) | post-M4 | Δ |
|---|---|---|---|
| RIGHT-leading | 8.38 mm | 9.3 mm | **+0.92 mm** |
| RIGHT-trailing | 8.66 mm | 8.4 mm | −0.26 mm |

RIGHT-leading's raw delta exceeds the ≤0.5 mm pass criterion. **Resolved by a push-and-rotate
qualitative check** (same method as the late-contact check): plunger pushed to full reach,
compared by eye against where it landed at M2. Operator's read: **no visible movement** — the raw
mm delta is put down to human measurement error (cramped caliper access), not real creep.

**Recorded as pass.** The mm figure above is kept as-read rather than corrected, so the record
shows what was measured; the push-and-rotate check is what it's actually being called on.

**M4 marked done for RIGHT. M4 complete, both stations.**

### B2-M5 — station angle, per class: skipped (2026-09-18)

**Decision: skipped, not deferred as an oversight.** M5 records the wheel-rotation angle per
class per station, purely to seed a future automated-navigation machine profile. Positioning in
Stage 1 stays manual regardless (per the plan's own text), and nothing in B2's PRESENT-flip
criteria or any later stage depends on this data — the machine profile's station-angle field
already has an explicitly anticipated placeholder state (A2) for exactly this case. Operator
call, given the near-zero payoff against this project's actual demo path.

**Left open, not settled:** if a future session builds real navigation, this angle data will need
collecting then, from scratch or from whatever's observed at that time — nothing here is assumed
to still hold.

**M5 marked skipped.**

### B2-M6 — wrong-actuator hazard, all four classes (2026-09-18)

**Method:** with a spoke of each class positioned at its own station (as done throughout M1/M3/M4),
operator visually checked the *other* station's plunger for a nearby spoke, per class.

| class positioned | home station | other station's plunger |
|---|---|---|
| LEFT-leading | LEFT | clear |
| LEFT-trailing | LEFT | clear |
| RIGHT-leading | RIGHT | clear |
| RIGHT-trailing | RIGHT | clear |

**No wrong-actuator hazard found on any class.** A fire at one station's plunger has nothing
nearby to strike at the other. **M6 marked done, all four classes.**

### B2-M7 — idle rattle, both directions (2026-09-18)

**Method:** `solenoid_smoke` baseline fire, 5 shots per direction, operator watching the idle
station's plunger throughout.

- **LEFT fires (60 ms, 5 shots)** — RIGHT's idle plunger: clean, no movement.
- **RIGHT fires (80 ms, 5 shots)** — LEFT's idle plunger: clean, no movement.

**Note:** the first LEFT attempt misfired at 5 ms (board width state didn't carry over as
assumed) and produced no real strike — discarded and redone at the correct 60 ms once actual
board state was confirmed via `s` before firing. The 5 recorded LEFT shots above are the retry,
not the void attempt (board's `fired` counter reflects both: 10 total, 5 void + 5 real).

**No idle rattle in either direction. M7 marked done.**

### B2-M8 — end-to-end attribution, S0..S3 (2026-09-18)

**Method:** physical S0 mark confirmed placed (LEFT-leading, immediately counter-clockwise of the
valve as seen from the rotor/RIGHT side, per C1). Wheel walked S0 → S1 → S2 → S3 in order,
operator observing which station's plunger each spoke actually lands under.

| spoke | predicted (Convention section, C2) | observed |
|---|---|---|
| S0 | LEFT (leading) | LEFT — matches |
| S1 | RIGHT (leading) | RIGHT — matches |
| S2 | LEFT (trailing) | LEFT — matches |
| S3 | RIGHT (trailing) | RIGHT — matches |

**All four match. The C2 class map is now verified, not merely declared** — the Convention
section's "Declared, not yet verified" note is superseded by this entry.
`indexing_origin = (Side B, LEADING)` (Known Limitation 1's reconciliation) is confirmed correct
for this wheel, this mounting, as of this M8 run. **M8 marked done.**

## B2 status — all stages closed or deliberately skipped

| stage | LEFT | RIGHT |
|---|---|---|
| E1 | anomaly noted, not chased (deferred, open) | anomaly noted, not chased (deferred, open) |
| E2 | done | done |
| E3 | done 2026-09-18 | done 2026-09-17 |
| E4 | done — hardware boot only, on the bench sketch (see E4 scope note) | same |
| M1 | done | done |
| M2 | done | done |
| M3 | done | done |
| M4 | done | done |
| M5 | **skipped, deliberately** (see above) | **skipped, deliberately** |
| M6 | done | done |
| M7 | done | done |
| M8 | done — verifies both stations' class map at once | done |

**No open blockers to PRESENT flip except operator sign-off.** E1's deferred coil-resistance
anomaly stays open per its original deferral (reopens on intermittent no-fire, mis-strike, or a
measured-pulse anomaly) but was never gating — E2–E4 independently confirm the electrical path.

**Per the standing halt-and-ask rule, flipping PRESENT to 1 is a physical/provenance action this
doc will not do on its own — it needs explicit operator go-ahead, station by station.**

## B3.0 prerequisite — MEASURE_ONCE overrides (2026-09-19)

B3.0's no-fire controls could not run: `MEASURE_ONCE` took only a spoke id. Built and committed
(`f494070`): `arg` packs spoke (bits 0-7), **no_fire** (bit 8) and a one-shot **pulse override** in
ms (bits 16-31); `tools/campaign_runner.py` exposes them as `--no-fire` and `--pulse-ms` and stamps
`campaign_selection` (`derived` / `pulse_override` / `no_fire`) into each manifest. Air shots need no
firmware: rotate the wheel to a gap and fire normally. T2 220/220; campaign and DevKit images build.
**Not yet run on target** — that is the flash-and-mic step below.

**Rules for reading these captures (carry into the pre-registration):**

- **`fired` is the only discriminator between a strike and a no-fire control.** A no-fire capture
  still carries its spoke's `station` and `debug_triggered=true`; `station` alone is not evidence of
  excitation. Any tool or report that groups by station must gate on `fired`.
- **A pulse level is identified by `pulse_ms` together with `station`, never by `excitation_digest`.**
  The override leaves the profile, and so the digest, untouched: captures at 40 and 85 ms share one
  digest.
- `campaign_runner.py` cross-checks the board's record against the request and logs `!!` for a
  control that says `fired=true`, a strike that says `fired=false`, or a level recorded at another
  width. Such a trial is not counted.

**Open, not settled here:**

- `tools/review_packet.py` **fixed 2026-09-19**: its manifest now shows station, `fired`,
  `pulse_ms` and `campaign_selection`, so a no-fire control and a strike no longer list
  identically (checked on one synthetic strike and one control). Committed with the other campaign
  tooling in `a8d2b83`, after the operator cleared it.
- The override's ceiling is the driver's existing 1000 ms stuck-actuator bound, not a thermal or
  dwell limit; the B2 brackets are enforced only by the runner. Whether the debug channel should
  carry a tighter cap is an operator decision.

**To run B3.0 (physical):** connect the mic; I flash `nano_esp32_fastdemo_mic_campaign` and confirm
it boots; position the wheel at a gap once per station for the air shots; keep the room quiet
through the ~5 minutes of automated capture.

### B3.1 — spoke sets (2026-09-19, drawn before any strike data)

Drawn by `tools/draw_spoke_sets.py` with the registered seed **20260919** (the date; chosen before
running, not tuned). Class of spoke i is `i mod 4` (the map B2-M8 verified). Per station and per
class: 2 exploration, 2 held-out, 1 reserve. "Spread around the wheel" is operationalised as: the
two spokes of a role sit at least 3 of the class's 8 ring positions apart (4 = opposite), so a role
never lands on neighbouring spokes. The script asserts the class balance and no double assignment,
and a run over 2000 seeds found no violation of either rule.

| | exploration (4) | held-out (4) | reserve (2) |
|---|---|---|---|
| **LEFT** (LEFT-leading 0 mod 4, LEFT-trailing 2 mod 4) | 0, 10, 12, 22 | 6, 16, 18, 28 | 20, 26 |
| **RIGHT** (RIGHT-leading 1 mod 4, RIGHT-trailing 3 mod 4) | 3, 5, 15, 17 | 9, 11, 21, 23 | 7, 13 |

Unused: 1, 2, 4, 8, 14, 19, 24, 25, 27, 29, 30, 31.

**Rules that go with the draw:**

- The sets above are fixed by the commit that adds them. Held-out spokes are not touched by any
  tuning, selection or exploration step.
- Changing the seed after strike data exists is not allowed. The one exception is a spoke found
  physically unreachable at its station: it is replaced by drawing again with `--seed` set to a
  documented new value, the replacement and the reason are logged here, and only the affected
  role and class are redrawn by hand from the unused spokes of that class.
- The sets are keyed by spoke index, so they depend on the class map and the S0 mark. If either
  changes, the sets are stale and B2-M8 is rerun first (provenance table).
- Two consequences worth knowing, neither a defect: S0 (spoke 0) is an exploration spoke, and the
  draw restricts spacing within a role only, so a held-out spoke can sit next to an exploration
  spoke of the same class (LEFT-leading 28 is adjacent to 0).

## B3.0 pre-registration (2026-09-19, before any B3.0 data)

Registered before a single B3.0 capture exists. The commit that adds this section is the
registration; **every B3.0 stage report cites its hash.** Nothing below may be changed after data
exists except by a new dated amendment that says what changed and why, and then only if the change
would not have altered what the existing data already showed.

**The rule** is `SOLENOID_CAMPAIGN_PLAN.md` B3.0 as amended by **Amendment 3** — the authoritative
text, deliberately not restated here so two copies cannot drift. In one paragraph: 40 controls in
three groups judged **separately** (`no_fire` 20, `air_LEFT` 10, `air_RIGHT` 10); **A** zero false
clears in every group; **B** no coherent line in any group, where a *line* is a strong peak the
firmware detector reports in the f1 band with SNR at or above the chain profile's own
`measurement_min_snr_db`, and it is *coherent* if a peak within ±2 Hz appears in at least half of the
group's captures; FAIL beats INCONCLUSIVE beats PASS, and a short or unreadable group is never a PASS.

**What is fixed here (the inputs and the instruments):**

| what | value |
|---|---|
| trial sequence | `docs/campaign_plans/b30.json`, seed **20260919**, sha256 `65aec227b45562db99eddf9d32d34722f1855d0dce3c39e47ec910afe6c8adbb`; RIGHT block first, then LEFT; 10 air shots + 10 no-fire per block, at most two of one kind in a row |
| instruments | `80838af` — `tools/campaign_sequence.py`, `tools/b30_verdict.py`, and `TRUING_SWEEP_MODE=lines` in `test/test_acoustic_sweep` |
| analysis constants | the fixture chain profile, digest `6328445a925e50b8636c08b43900a1ee2410b2370fca0391051f2d3dac883654` (the same digest the repo's recorded captures carry): f1 band 350–600 Hz, prominence 6 dB, peak depth 30 dB, SNR gate 12 dB, window 500 ms, gate start 40 ms, onset absolute floor −55 dBFS rms (disabled in the analysis only) |
| firmware | `f494070` and later on `nano_esp32_fastdemo_mic_campaign`; the build rev is in every capture |
| rig | `LEFT/rig-1`, `RIGHT/rig-1` |
| exclusion codes | `ACK_REJECTED`, `NO_NEW_CAPTURE`, `FETCH_FAILED`, `CAPTURE_NOT_OK`, `RECORD_CONTRADICTS_REQUEST` — mechanical only; an excluded trial is replaced in place and ledgered |

**What would invalidate this registration:** a change to the chain profile digest, to the detector
or SNR code, to the ±2 Hz tolerance or the 50 % recurrence, to the group sizes, or to the plan file.
The verdict tool refuses a set whose captures carry a different chain digest than the analysis uses,
or more than one plan.

**Limits stated before the data, so they cannot be discovered afterwards:**

- **A PASS is not "nothing is there."** A line below the SNR gate, or one recurring in fewer than
  half of a group, is not detected. 20/10/10 captures bound a false-clear rate only below about 14 %
  and 26 % per group (7 % pooled) at 95 % confidence.
- **The chance rate of a line at the gate is measured on one recorded ambient capture only** (27
  strong peaks in the f1 band, none at or above 12 dB, maximum 11.2 dB). That is a single capture,
  not a rate. **The stage report must state, from the real controls, how many captures had any
  in-band strong peak at or above the gate and how many lines that made**, so a chance-level rate is
  visible rather than assumed away.
- **The window is anchored on the loudest event**, not on a strike, for a quiet control. For an air
  shot that is the plunger impact; for a no-fire capture it is the loudest noise frame and so is
  arbitrary. Stationary lines do not depend on it; a transient one might.
- **The analysis constants are the fixture profile's.** They equal what the recorded captures were
  taken under, and the tool checks each B3.0 capture's own digest against them — but that check
  cannot tell whether the constants are *right*, only that they are the ones in use.
- **A 360 Hz line** is reported per group whatever its strength, alongside the general rule.
- **Not registered here:** B3.2's selection rules and B3.4's acceptance are in the plan (Amendments
  1–2) and are registered by a separate commit before any B3.2 data, as the plan requires.

**Stage-report contents (B3.0):** this section's commit hash; the plan sha256 and the exclusions
ledger; `tools/b30_verdict.py` output verbatim; the per-group table; the 360 Hz report; the count of
in-band strong peaks at or above the gate per group; and the statement above that a PASS is bounded.

## B3.0 stage report - controls and coupling (2026-09-19)

**Verdict: PASS**, under the rule registered in `8aa19d6` and bounded as that section says. Plan
sha256 `65aec227b45562db99eddf9d32d34722f1855d0dce3c39e47ec910afe6c8adbb`, seed 20260919, build
`2511fdf` (clean tree, `nano_esp32_fastdemo_mic_campaign`, 3-region esptool flash, 62/62 bring-up),
run as `tools/campaign_runner.py --plan docs/campaign_plans/b30.json --no-prompt`, 10:43-10:48.
**40 trials kept, 0 excluded.** The final run wrote no exclusions ledger.

`tools/b30_verdict.py --run`, verbatim:

```
B3.0 verdict: PASS

detector settings read from the data: f1 band 350-600 Hz, line = SNR >= 12.0 dB (the chain profile's own gate)
plan sha256: 65aec227b45562db99eddf9d32d34722f1855d0dce3c39e47ec910afe6c8adbb
chain digest: captures 6328445a925e, analysis 6328445a925e

group         n   clears lines>=gate coherent lines 360Hz max SNR
no_fire      20        0       7/20              0      12.8 dB
air_LEFT     10        0       0/10              0       0.5 dB
air_RIGHT    10        0       0/10              0       3.1 dB

0 false clears in 40 controls: false-clear rate <= 7.2% at 95% confidence; per group <= 13.9% (n=20), <= 25.9% (n=10).

PASS means: no false clear, and no line at or above the SNR gate recurring in half of any group.
It does not mean nothing is there: a line below the gate, or recurring in fewer than half of a
group, is not detected by this rule, and 20/10/10 captures cannot rule out a rare one.
```

**In-band lines, counted as the registration requires** (strong peaks the firmware detector reports
in 350-600 Hz before any clear or reject gate):

| group | captures | with a line at or above the gate | lines at or above the gate | strong in-band peaks, any SNR |
|---|---|---|---|---|
| no_fire | 20 | 7 | 11 | 469 |
| air_LEFT | 10 | 0 | 0 | 103 |
| air_RIGHT | 10 | 0 | 0 | 74 |

The 360 Hz report: `no_fire` has one line at 361.2 Hz, 12.8 dB, in one capture (not coherent); the
air groups have none at or above 0.5 dB and 3.1 dB respectively.

**What the counts say, and what they do not.**

- **A line at the SNR gate is not rare in these controls.** The 11 no-fire lines at or above the
  gate sit at 361.2-389.1 Hz and 12.1-15.1 dB, in 7 of 20 captures. All 20 no-fire captures were
  nevertheless rejected: 16 `AMBIGUOUS_PEAK` and 4 `LOW_SNR`. Which rejection stopped those seven
  captures individually was not broken out, and whether the SNR gate alone would have cleared any of
  them was not tested. The plan's carried-forward note that the 12 dB gate
  "sits in the measured control/clear gap" rested on one ambient capture (maximum 11.2 dB); twenty
  quiet captures now reach 15.1 dB. **Nothing is changed on this:** a threshold change needs strike
  evidence, and that is B3.2's job. It does bind B3.2's reading: a strike clear at 12-15 dB near
  360-390 Hz is not separated from room noise by SNR alone, and only the interleaved controls can say
  whether it is discriminated.
- **The lines are not coherent by the registered rule** (a peak within +-2 Hz in at least half of the
  group), but they are not one frequency either: 11 lines spread over 361-389 Hz. The rule detects a
  recurring line; it cannot say this is not a broad low-frequency noise feature.
- **Every control reached spectral analysis.** No capture was stopped at the onset floor (the
  rejections are `AMBIGUOUS_PEAK` and `LOW_SNR`, both post-onset), so the absolute onset floor did
  not filter the quiet controls in this room. The recorded ambient fixture, by contrast, is rejected
  `NO_ONSET_DETECTED` in the bring-up self-check. Why the live controls got further was not
  investigated.
- **PASS is bounded.** 0 false clears in 40 controls bounds the rate at 7.2% pooled, 13.9% for
  no_fire and 25.9% for each air group, at 95%. It says nothing about strikes.

**Process record: two aborted starts before any data, both kept.**

| ledger (in `_campaign/`) | what happened | fix |
|---|---|---|
| `B3.0_aborted-start_exclusions.jsonl` | trial 1, three `FETCH_FAILED` in under a second. **No shot was fired.** On a freshly booted board `/debug/capture.json` answers 404 "nothing captured yet"; the runner's pre-shot sequence read let that 404 escape as a failed trial | `aee250e`; regression test fails without it |
| `B3.0_aborted-httpd-crash_exclusions.jsonl` | trial 1, try 1: one no-fire shot was taken, then the board **panicked and rebooted** (`A stack overflow in task httpd`, serial log) when the runner fetched the finished capture. Discarded, no bundle | `2511fdf`: httpd stack 4 KB -> 8 KB |

The overflow was reproduced with serial capture before the change and did not recur after it. The
backtrace shows only the overflow hook, so the handler is identified by timing and by the measured
margin (3960 bytes free of 8192 on one capture, so the handler used about 4.2 KB, more than the old
4096), not by a stack trace. `MEASURE_ONCE` with a full capture fetch had never run on hardware
before this, so the stack limit was a latent fault in every campaign fetch and in `/debug/capture.json`
generally.

**First on-target evidence for the no-fire override:** 20 of 20 no-fire captures record
`fired=false`, `pulse_ms=0` and carry their spoke's station; 20 of 20 air shots record `fired=true`;
`capture_result` is OK and `n_words` 57600 for all 40. The **pulse override was not exercised** on
target (no strike levels yet); its first use is B3.2, and the runner's cross-check will flag any
capture that records another width.

**Also closed:** a boot at `PRESENT=1` strikes LEFT then RIGHT. Observed by the operator (two
mechanical triggers) and in the bring-up serial log (spoke 0 LEFT 20026 us, spoke 1 RIGHT 20038 us).

**Limits of this data.**

- **Nobody watched the wheel.** The operator was out of the room and stated that both plungers were
  clear. That was not independently verified. All 20 air shots were rejected `LOW_SNR` with a maximum
  of 3.1 dB, which is consistent with a plunger hitting nothing but does not prove it.
- **Ambient conditions were not measured** apart from the captures themselves.
- **Where the evidence lives.** The bundles are under
  `test/fixtures/acoustic/captures/_campaign/`, which is gitignored, so **they exist only on this
  machine**. 40 of the 110 bundles in its `index.txt` are B3.0-stamped (the rest are earlier passes
  and are ignored by the tool because they carry no `campaign_kind`). PCM manifest sha256 over the 40
  (sorted `name:pcm_sha256` lines): `3d03d6022ffdd0627db6dc968634420fb724667ac0969817855933e3050e8b6c`.
  Back them up if they matter beyond this session.
- **The composite navigation is off on this image** (`composite_navigation:false`), as designed.

**Next:** B3.2 is unblocked. Its pre-registration (`docs/campaign_plans/b32.json` and the rules)
must be committed before any B3.2 data.

## B3.2 pre-registration (2026-09-19, before any B3.2 data)

Registered before a single B3.2 strike exists. The commit that adds this section and
`docs/campaign_plans/b32.json` is the registration; **every B3.2 stage report cites its hash.**
Nothing below may change after data exists. A change is an amendment commit that says why, and any
data taken before it is labelled as taken under the earlier text.

**What is fixed elsewhere.** The pulse levels, counts, spoke sets, selection rules 1-6, the SNR
report (rule 2a) and the gates are the plan's (Amendments 1-2), and B3.1's spoke sets are this
record's. This section adds only what those leave open. **Each such choice is marked
(operationalization)**: the plan's wording admits more than one mechanical reading, and it is fixed
here, before data, so it cannot be settled by looking at results.

### The plan

`docs/campaign_plans/b32.json`, seed 20260919, **sha256
`be053f63cef29608abd7a7026ba6d7fa2b1f567b03cdccd688882e5f8e9a0904`**, regenerated from the seed and
found identical. **This supersedes the plan first registered in `fd75723` (sha256 `efd73512a17d...`, never run;
see "Amendment 1 to this registration" below).** 8 strike blocks, one spoke each, levels shuffled
within a spoke, a no-fire control after every 3 strikes, plus one **air block per station**.
**168 strikes, 56 no-fire controls and 42 air shots at the tested widths: 266 trials.**

| station | spokes (strike blocks, in plan order) | levels (ms) | strikes | no-fire controls | air shots |
|---|---|---|---|---|---|
| RIGHT | 17, 5, 3, 15 | 60, 72, 85 | 6 per level per spoke = 72 | 24 | 6 per level = 18 |
| LEFT | 10, 22, 12, 0 | 40, 60, 72, 85 | 6 per level per spoke = 96 | 32 | 6 per level = 24 |

Block order as generated: the four RIGHT strike blocks, then `B3.2-RIGHT-air`, then `B3.2-LEFT-air`,
then the four LEFT strike blocks. The air blocks' positions come from their own seeded stream and were
not chosen; they landed adjacent, mid-session. An air block needs the wheel turned so the plunger lands
between two spokes and held there; it fires the tested widths in random order with nothing interleaved.

Run block by block (the operator positions the named spoke at its plunger and holds it):
`python tools/campaign_runner.py --plan docs/campaign_plans/b32.json --no-prompt --start-block N --end-block N+1`.
Every capture is stamped with the plan hash, the rig_id (`LEFT/rig-1`, `RIGHT/rig-1`), the station,
the requested level and the build. **All B3.2 data comes from one build rev**, the one the first
block reports; a different rev, or a bumped rig_id, is reported and analysed apart.

### Vocabulary, made operational

- **Condition** (operationalization): (station, pulse level, DSP candidate). Clears are judged within
  a condition, never pooled across levels.
- **Clear:** status `suspect` (PROVISIONAL_MODE_ID) under the candidate being evaluated. **False
  clear:** a clear on a no-fire control or an air shot.
- **Consistency band** (plan: +-2 Hz around the median, at least 3 clears): the median is over that
  spoke's clears in the condition, **including** the clear under test. **A spoke with fewer than 3
  clears in a condition is unjudged** (operationalization): its clears count as neither consistent
  nor inconsistent, they add nothing to its rate, and the count of unjudged clears is reported.
- **Consistent clear rate** of a spoke in a condition = consistent clears / 6. Excluded trials are
  replaced in place, so n stays 6.
- **W(c, s, l)**, the worst spoke: the minimum of that rate over the station's 4 exploration spokes,
  for candidate c, station s, level l. The class-wise worst is the minimum over that class's 2 spokes.
- **Cell** (c, s, l) **satisfies the cell constraints** if it has 0 inconsistent clears, **0 false
  clears among the 6 air shots fired at that station and width under c**, and 100 % attribution: every
  strike record has `fired=true`, the station derived from the declared spoke,
  `capture_result` OK and `pulse_ms` equal to the requested level (0.01 ms). Attribution rests on the
  board's own record plus B2-M8's physical verification; the runner's per-trial cross-check enforces
  the record and the operator's positioning is not independently observed.

### Candidate set (operationalization)

The plan's offline sweep is over `gate_start_ms`, `window_ms` and `prominence_db`, at most two fields
away from baseline (gate_start_ms 40, window_ms 500, prominence_db 6; the fixture profile, chain
digest `6328445a925e`).

- Values: **gate_start_ms** 100, 200, 300, 500, 800; **window_ms** 250, 750; **prominence_db** 9, 12.
  These are the harness's existing one-axis rows. They are not chosen from B3.2 data.
- Candidates, in this order for tie-breaking: baseline; the 9 single-field rows (gate ascending, then
  window, then prominence); the 24 two-field rows (gate x window, then gate x prominence, then
  window x prominence). **34 candidates.**
- **Not candidates:** `max_peak_depth_db` (20 was rejected in Phase A: it cleared at components 40-70
  Hz from the spoke's repeated cluster), `measurement_min_snr_db` and the search band (not in the
  plan's swept list; B3.0 also found in-band lines at the 12 dB gate in quiet controls, so lowering it
  is not a knob this campaign turns). The harness prints rows for them; they select nothing. A
  next-onset margin candidate is added only if truncation is observed, by amendment.

**Found when the two-field rows were built (2026-09-19, before any strike): 7 of the 34 cannot run.** The config
validator refuses a chain profile whose `gate_start_ms` + `window_ms` does not fit the capture. The harness prints
these as `OUT_OF_RANGE`: gate 800 alone, and the pairs gate 300 + window 750, gate 500 + window 750, gate 800 +
window 250, gate 800 + window 750, gate 800 + prominence 9, gate 800 + prominence 12 (sums of 1050 or more; the
largest sum that runs is 1000). They stay in the registered list and nothing about the capture length changes. A
candidate that cannot run has no clears, so its worst-spoke rate is 0: it cannot be chosen over an evaluable
candidate and cannot meet the exploration gate. 27 candidates are evaluable.

### Selection, made operational

Applied in this order, to the data as it stands when all 224 trials are kept. The control sets are:
B3.0's 40 controls, B3.2's 56 no-fire controls, the 18 Phase A pass-C no-pluck controls (`C_*` in the
campaign index) and `nano_ambient_ambiguous`, each replayed through `native_sweep` with the onset
floor as it is (what the firmware would do), not the lines-mode override.

1. **Candidate constraints** (plan rule 1). Candidate c is **admissible** only if: (i) 0 false clears
   across all the control sets above; (ii) **no selection shift**: no strike capture where both the
   baseline and c clear and their f1 differ by more than 2 Hz.
2. **Admissible cells.** S(c, s) is the set of levels l at station s whose cell satisfies the cell
   constraints. **c is eligible** only if S(c, LEFT) and S(c, RIGHT) are both non-empty. If no
   candidate is eligible: **G-analysis, halt.**
3. **DSP** (plan rule 2, operationalization). score(c) = the smaller, over the two stations, of
   [the largest W(c, s, l) over l in S(c, s)]. The chosen c maximises score.
4. **Pulse** (plan rule 3), at the chosen c. best(s) is the largest W over S(c, s). **A shared level**
   exists if some level in both S(c, LEFT) and S(c, RIGHT) (so 60, 72 or 85) has W within 10 pp of
   best(s) at both stations. Rates move in steps of 1/6, so within 10 pp means equal to the best. If
   none does, each station takes its own level with the largest W. A **plateau** is a maximal run of
   adjacent tested levels in S; a level is **interior** when both its neighbours in the tested list
   exist and are in S. The consistent clear rate stays the primary metric.
5. **Tie-breaks** (plan rule 4), in order: plateau interior; fewest fields changed from baseline (0,
   1 or 2); higher median SNR (rule 2a below); shorter pulse. For DSP candidates a remaining tie goes
   to the earlier one in the registered order.
6. **Refinement** (plan rule 5), at most one extra level per station, when the best level is at a
   tested edge that is not the M3 bracket edge, or two adjacent tested levels differ in W by more than
   25 pp. The extra level is the whole-ms midpoint of that gap or edge stretch, strictly inside the M3
   bracket (LEFT [40, 85], RIGHT [60, 95]). It is run from a new plan file with its own sha256, in
   a separate registration commit before its data. No other level is added.
7. **Class check** (plan rule 6, C4). At each station, at the chosen c, any level in S where one
   class's worst spoke sits more than 10 pp below the other class's is a class gap. If every level in
   S at a station has one, that is **G-class, halt** (a user decision, as B2 defines it).
8. **SNR, reported not gating** (plan rule 2a). Median `expect_snr_db` per level per station, over
   the strike captures that produced a spectrum, with n; per class where a class has at least 6 such
   captures at that level. Used only as the tie-break above. A level with a markedly lower clear rate
   and lower SNR than a shorter neighbour is read as corroborating dwell, and does not by itself change
   the selection.

### Gates

- **Exploration gate** (plan, Amendment 1): each station needs an eligible candidate with a cell at
  **W of 5/6 or better**. If not: one bounded mechanical lever (mic distance or strike point) and a
  reduced rerun; still failing, stop and report the ceiling for that station.
- **G-analysis**, **G-class**: halt and report as above. **B3.3 (the freeze) is halt-and-ask.** This
  section selects a candidate; it does not change a constant.

### Blind review

24 strikes, 12 per station, stratified by level (LEFT 3 per level, RIGHT 4 per level), one draw per
stratum by `random.Random(20260919)` over the sorted trial numbers of kept strikes. Drawn after all
data is in and before any machine verdict is looked at; the verdict is sealed until the responses are
filed. Five categories per trial: strike audible / ring in the analysed window / second impact or
rattle / clean excitation / metallic or rotor ring audible (RIGHT only). No pitch judgments. The review
selects nothing. **A systematic "clear without ring"** (operationalization) is 3 or more of the 24
where the baseline cleared and the reviewer heard no ring; it stops the stage.

### Reported, not gating

Per level and station: strikes kept, exclusions by code, overruns, truncated windows, unjudged
clears; per candidate: false clears by control set; per air group (station and width): captures with
an in-band line at or above the gate, and the lines (frequency, SNR), as B3.0 reported them, without a
recurrence rule (6 captures make a 50 % rule too coarse to gate on). **The noise-band report**, prompted by B3.0: how
many strike clears at baseline and at the chosen candidate have f1 between 355 and 395 Hz, the region
where B3.0's quiet controls put lines at the 12 dB gate (11 lines, 361-389 Hz, 12.1-15.1 dB), beside
the same count for the no-fire controls. It is a report, not a threshold, and does not select.

### Exclusions and stopping

Only the runner's five pre-declared codes (`ACK_REJECTED`, `NO_NEW_CAPTURE`, `FETCH_FAILED`,
`CAPTURE_NOT_OK`, `RECORD_CONTRADICTS_REQUEST`), a trial replaced in place, and a stop after 3 tries
of one trial. Overruns are excluded by `CAPTURE_NOT_OK` and therefore do not count against a level, so
they are reported per level (above). Nothing is deleted. A block interrupted by anything other than a
runner code is resumed with `--start-block`, and the interruption is logged here.

### Limits registered now

- **Air-shot controls at the tested widths were added** (Amendment 1 below), 6 per station and width.
  Six captures bound a false-clear rate only below about 39 % at 95 %, and 6 per cell is what the
  design affords; a cell with 0 false clears in 6 is not shown to be free of coupling, only not shown
  to have it. The air shots run at the wheel position the operator chose for a gap, and a plunger that
  actually touched something there would not be detected.
- **B3.0's controls used the profile pulse and a 20 ms plunger impact** and were taken with the room
  empty; B3.2's strikes will be taken under whatever the room is doing. Interleaved no-fire controls
  are the only check on that.
- **The exploration set is 4 spokes per station.** A rate on 6 strikes moves in steps of 16.7 pp, so
  the 10 pp and 25 pp thresholds are coarse; that is why rule 3's tolerance reduces to "equal to the
  best".
- **Consistency is self-referential**: a spoke that rings at a wrong but stable frequency is
  consistent. This campaign does not identify modes (plan, "What this campaign is not").

### Amendment 1 to this registration (2026-09-19, before any B3.2 data)

**Why.** The registration's own limits section named a gap: B3.2's only controls were no-fire, and
B3.0's air shots used the profile's 20 ms pulse, so nothing tested whether a 40-85 ms actuation couples
noise into the analysed window that a 20 ms one did not. The operator asked for it to be closed.

**What changed, and only this.**

- The plan gains an air block per station, 6 air shots at each tested width (42 in all), and the plan's
  sha256 is now `be053f63cef29608abd7a7026ba6d7fa2b1f567b03cdccd688882e5f8e9a0904`. The
  strike blocks and their order are byte-for-byte what `efd73512a17d...` had (checked by the generator's selftest,
  which regenerates the pre-amendment plan with `air_per_level=0` and compares).
- A cell (station, level) satisfies its constraints only with 0 false clears among its own 6 air shots
  under the candidate. The pooled control sets and every other rule are unchanged.
- The air groups' in-band lines are reported per station and width; nothing about them is gated.
- Plan text: Amendment 4 in `SOLENOID_CAMPAIGN_PLAN.md`, on the B3.2 physical section and rule 1.

**Tools changed with it.** `tools/campaign_sequence.py` (the air blocks, and its selftest);
`tools/b30_verdict.py` now considers only bundles stamped `campaign_stage` B3.0 -- without that, B3.2's
air shots sharing the bundle directory would have entered the B3.0 groups and turned a re-run of the B3.0
verdict INCONCLUSIVE (the selftest injects a B3.2 air shot and fails without the filter; the recorded
B3.0 verdict is unchanged); `tools/campaign_report.py` labels air-shot groups by width.

**What did not change.** No threshold, no DSP constant, no candidate, no exploration spoke, no level.
No B3.2 data existed: the plan committed in `fd75723` was never run.

## B3.2 air blocks - interim record (2026-09-19, before any B3.2 strike data)

Blocks 5 and 6 of `docs/campaign_plans/b32.json` (`B3.2-RIGHT-air`, `B3.2-LEFT-air`), plan sha256
`be053f63cef29608abd7a7026ba6d7fa2b1f567b03cdccd688882e5f8e9a0904`, build `2511fdf`, chain digest
`6328445a925e`, run 11:13-11:18 with `--start-block 4 --end-block 6`. **42 trials kept (trials 97-138),
0 excluded, no exclusions ledger.** Board uptime continuous, no reboot. The wheel had not been moved since
B3.0's last air block; the operator stated that both plungers were still in gaps and did not watch the wheel
during the run.

**Deviation, logged.** The registered order puts these blocks after the four RIGHT strike blocks; they were
run **first**, because the wheel was already at a gap and the strike blocks need a person to place each
spoke. Trial numbers still follow the plan (97-138), so the record is unaffected, but the seeded intent of
spreading the air shots through the session in time is not met: they precede all strikes.

**Result against the registered constraint: 0 false clears in 42.** Every air shot was rejected.

| station | width (ms) | n | rejection reasons |
|---|---|---|---|
| LEFT | 40 | 6 | 6 `LOW_SNR` |
| LEFT | 60 | 6 | 5 `LOW_SNR`, 1 `AMBIGUOUS_PEAK` |
| LEFT | 72 | 6 | 3 `LOW_SNR`, 3 `AMBIGUOUS_PEAK` |
| LEFT | 85 | 6 | 4 `LOW_SNR`, 2 `AMBIGUOUS_PEAK` |
| RIGHT | 60 | 6 | 3 `LOW_SNR`, 3 `AMBIGUOUS_PEAK` |
| RIGHT | 72 | 6 | 5 `LOW_SNR`, 1 `AMBIGUOUS_PEAK` |
| RIGHT | 85 | 6 | 3 `LOW_SNR`, 2 `AMBIGUOUS_PEAK`, 1 `VALUE_OUT_OF_RANGE` |

`capture_result` OK and `fired=true` on all 42; the recorded width equals the requested one on all 42. The one
`VALUE_OUT_OF_RANGE` (trial 112) is the window-selection failure at `acoustic_real.c:148` ("too little signal
survives gating"), not a near-clear: onset 115 ms into the capture, no window selected, and the host replay
reproduces it (`NO_WINDOW`). Read as a very short click that decays below the decay floor before the minimum
window; not traced further.

**In-band lines at or above the 12 dB gate** (reported, not gated, per the registration; the lines-mode harness,
onset floor disabled):

| group | captures with a line at the gate | lines (Hz, dB) |
|---|---|---|
| LEFT 40 / 60 / 72 ms | 0 / 0 / 0 | none (strongest in-band peak 11.7 / 11.3 / 9.8 dB) |
| LEFT 85 ms | 2 of 6 | 425.0 (12.4); 427.5 (13.8), different captures |
| **RIGHT 60 ms** | **3 of 6** | **508.4-508.8 (12.4-14.2), 529.1-529.7 (13.2-15.9), 542.0-542.2 (13.7-14.5)** |
| RIGHT 72 ms | 1 of 6 | 542.1 (14.7) |
| RIGHT 85 ms | 1 of 6 | 495.5 (12.3) |

### Observation: recurring tones after the impact at RIGHT, present with a 60 ms actuation and not with a 20 ms one

At the RIGHT station with a 60 ms pulse, three lines recur across the six air shots, at about **508.6, 529.5
and 542.0 Hz** (within +-1.5 Hz, any SNR, counting captures that have one): 4 of 6, 4 of 6 and 3 of 6, with 3, 3
and 2 of those at or above the gate, up to 15.9 dB. At RIGHT 72 and 85 ms they are weaker (at most 1 of 6 at the
gate). **B3.0's 20 ms RIGHT air shots have none of them (0 of 10, at any SNR).** LEFT shows the same
frequencies more weakly (at most 3 of 6, at most 7.1 dB), and B3.0's no-fire controls show them at 4-8 of 20
with at most 5.9 dB, so a low-level version of these tones is present in the quiet room and the 60 ms
actuation drives them harder. **What is established is the within-RIGHT comparison, 60 ms against 20 ms, at
one microphone position. It does not establish that the RIGHT station is the noisier one: the microphone is
closer to the RIGHT solenoid than to the LEFT one, so a weaker LEFT signal is expected from geometry alone**
(an earlier draft of this record said the lines were tied to the RIGHT station and not to the room; both
statements went beyond the data and are withdrawn).

What this is not: it is **not a false clear** (none of the 42 cleared), so the registered rule passes. What it
is: a plunger that hits nothing still excites something at RIGHT that rings in the 350-600 Hz band, above the
SNR gate, at frequencies where a spoke's own pitch could sit. Applying B3.0's own coherence wording (a line at
the gate within +-2 Hz in at least half a group) to the RIGHT 60 ms group would put 508.6 and 529.5 Hz at
exactly 3 of 6, 50 %; the B3.2 registration deliberately does not gate on it. The cause is **not identified**.
The plan's C8 predicted a RIGHT-only structural resonator (the rotor) that air shots would not excite; this
is evidence the actuation excites *something* at RIGHT anyway, but which part is not known and the rotor is
one candidate among the tower, extrusion and fitting.

**Also observed: the actuation is about 18-20 dB louder from 40 ms up.** The loudest sample of every B3.2 air
shot, at either station and every width, is between -2.5 and -0.0 dBFS (one loud event, about 180-205 ms into
the capture, nothing else within 20 dB of it), against -17 to -23 dBFS for B3.0's 20 ms air shots. No capture
is flat-topped (at most 2 samples within 2 % of the peak), so the microphone is near its limit and not
clipping. This fits B2's finding that a pulse reaches the spoke only from about 40 ms (LEFT) and 60 ms
(RIGHT): B3.0's 20 ms air shots were a weaker actuation than any B3.2 strike will be, so B3.0's
no-coupling result speaks for 20 ms only, which is why the amendment mattered. It also means the DSP
window sits right after a near-full-scale event.

**Why the tones may matter for the strike data (a hypothesis, not shown).** The registered consistency test is
per spoke, so a tone that repeats at the same frequency on every RIGHT spoke would pass it. But f1 is the
lowest strong peak in the band, so these tones can only become a spoke's f1 where its own peak is absent or
lies above about 508 Hz. Air shots cannot say whether strikes show it; the strike data will.

**Operator's listening check (2026-09-19).** The operator listened to the six groups (RIGHT 60/72/85 ms, RIGHT
20 ms, LEFT 60 ms, quiet no-fire) and reports it is clean actuator noise: only the plunger hitting its stop,
"the push event itself", and nothing rotor-like. This is a human judgement on the recordings, not a measurement,
and it discounts the rotor as the candidate; the tones are read as the ring of the plunger-stop impact. The
tower, extrusion and fitting stay possible carriers. The operator is considering rubber on the plunger stops to
damp it; that would be a physical change at the station (see the rig registry: a new rig_id, with B2's standoff
and pulse bracket re-measured). Not decided, and not needed to continue on `LEFT/rig-1` and `RIGHT/rig-1`.

**Decision (2026-09-19, before any strike): report the cross-spoke check, do not gate on it; carry on on
`LEFT/rig-1` and `RIGHT/rig-1`, no rubber.** Recorded as Amendment 2 to the B3.2 registration below.

Bundles (`_campaign/`, gitignored, this machine only): 42 B3.2 air bundles; PCM manifest sha256 over them,
sorted `name:pcm_sha256` lines:
`f0e4e0ff9effdfb8d60280ff030b9aa24fba364eef82a49f7ab80069ae2c8bda`.

### Amendment 2 to this registration (2026-09-19, after the air blocks, before any strike)

**Why.** The air blocks showed tones after the plunger-stop impact at RIGHT (see the interim record), and the
registered consistency test is per spoke, so a tone that repeats at the same frequency on every spoke would pass
it. The operator listened and reports it is actuator noise (the plunger hitting its stop), not the rotor.

**What is added, and only this.** A **cross-spoke coincidence report**, with a review trigger and **no gating**:

- For each station, width and candidate (the baseline and the chosen one): list the f1 of every strike clear. A
  cell is **flagged** when clears from at least 3 of the station's 4 exploration spokes have f1 within +-2 Hz of
  one another (the common frequency is their median).
- For each flagged cell, also report whether the common frequency lies within +-2 Hz of a line found in that
  station's air shots at the same width (the tool lists those lines from the air captures).
- **Trigger, not a filter.** If a flagged cell belongs to the chosen candidate at the chosen level, the selection
  report says so, and approval at B3.3 (already halt-and-ask) is not requested until the operator has looked at
  it. Whether those clears count as consistent is then the operator's call, recorded before the freeze. **No clear
  is removed automatically, and no selection rule, threshold, candidate, spoke or level changes.**

**Limits.** A flag is a prompt to look, not a verdict: spokes on a real wheel differ in tension, so a common f1
is suspicious, but several spokes can genuinely share a pitch. The check cannot tell the two apart by itself.

**Operationalisation (2026-09-19, before any strike; tool `tools/b32_crossspoke.py`, `4311fa3`).** Two phrases
above were given an exact meaning, reusing B3.0's own definitions, and nothing else in the amendment changed.
"Within +-2 Hz of one another" is B3.0's "same line" rule: each clear's f1 is tried as a centre, the members are
the clears within +-2 Hz of it (inclusive), the cell is flagged when the members come from at least 3 distinct
spokes, and the common frequency is the median of the members. "A line found in that station's air shots at the
same width" is a B3.0 coherent line (a strong in-band peak at or above the chain's minimum SNR, recurring within
+-2 Hz in at least half of those air shots), read from the lines run, which uses the baseline chain profile only.
The air shots are chosen by station and width, not by stage; B3.0's were fired at 20 ms and no B3.2 level is
below 40 ms, so the two sets do not meet at any registered width.

**Not done, and why.** Gating on the air-shot lines (a strike clear near one is not a consistent clear) was
rejected: six shots at one width and one microphone position are a thin basis, and it could discard a real
spoke clear. A hardware change (rubber on the plunger stops) was deferred: it would be a new rig_id with B2's
standoff and pulse bracket re-measured, and it is not needed to continue on `rig-1`. If a station misses the
exploration gate, a stop-damping change is a candidate for the plan's one bounded mechanical lever, by a small
amendment at that point.

### Readings fixed for the selection tool (2026-09-19, before any strike; `tools/b32_select.py`, `f83983d`)

The tool applies rules 1-8, the gates, the draw and the reported items as registered. Where the text admits more
than one mechanical reading it uses the one below. None changes a threshold, a level, a candidate or a spoke, and
each is fixed before a strike exists. An independent audit of the first draft found these readings unrecorded and
one of them (the draw) structurally unable to review one spoke per station; both are corrected here.

1. **A clear, and exact arithmetic.** A clear under a candidate is a sweep row with status `suspect` and an f1. The
   "+-2 Hz" band and "more than 2 Hz" (selection shift) are compared in exact millihertz, since the sweep prints
   three decimals: a difference of exactly 2.000 Hz is inside the band and is not a shift (as floats, 510.003 and
   512.003 differ by 2.000000000000057).
2. **Complete data only.** "All 224 trials" was written before Amendment 1 added the air shots; it is read as all
   266 planned trials, the control sets at 40 (B3.0), 18 (pass-C) and 1 (ambient), and every needed sweep row.
   Otherwise the tool reports INCOMPLETE and selects nothing. Trial number, requested level and spoke come from the
   plan by `campaign_trial`; attribution also requires the board's own `station` field to match the station derived
   from the spoke.
3. **Cannot run, and never clears.** The 7 candidates the validator refuses are not evaluable and never eligible. A
   candidate that runs and never clears has 0 false clears and 0 inconsistent clears, so its cells satisfy the
   constraints vacuously: it is eligible, with score 0. G-analysis therefore fires only when every evaluable
   candidate is inadmissible or has an empty S at a station. Nothing clearing at all is shown by a top score of 0,
   which the report prints as NO SELECTION (the score-0 tie-break winner is not a choice), and by the exploration gate.
4. **Rule 5 at candidate level.** Candidates tied on score are ordered by: plateau interior (how many stations, 0 to
   2, have a chosen level with both neighbours in S), fewest fields changed, higher median SNR, shorter pulse (sum
   over stations), then registered position. A candidate's chosen levels are the ones rule 4 gives it, using the same
   tie-breaks. For a shared level the SNR is the lower of the two stations' medians.
5. **SNR population.** The board records `expect_snr_db` only when its baseline analysis found an f1, so "the strike
   captures that produced a spectrum" is the captures that carry an SNR. The report says so.
6. **Exploration gate.** Per station: some eligible candidate has a cell in S at worst spoke 5/6 or better. It need
   not be the same candidate at both stations, nor the chosen one.
7. **Class check.** At the chosen candidate, over the levels in S; a gap is either class more than 10 pp below the other.
8. **Refinement** is reported and never applied. The edge trigger uses the station's chosen level, the gap trigger
   uses W over all tested levels; every trigger is listed with its whole-ms midpoint (two, when the sum is odd). Which
   single extra level per station to run is decided when the lead registers it.
9. **The blind-review draw.** One `random.Random(20260919)` consumed across the strata in the order LEFT 40, 60, 72,
   85, then RIGHT 60, 72, 85, calling `sample` over each stratum's sorted trial numbers (the tool prints the Python
   version). A fresh generator per stratum was rejected: all strata have 24 kept strikes, so each would draw the same
   positions, and a pool in trial order is in block order, so the same blocks would be drawn every time and one
   spoke per station could never be reviewed. It is made only when all planned trials are kept, and reads no verdict.
10. **Reported, not gating:** unjudged clears (baseline and chosen, per level); overruns among kept and excluded
    bundles; the window-truncation histogram; exclusions by code from the ledger; air-group lines, with captures that
    have no readable spectrum counted as such and not as "no line"; SNR per level, and per class where a class has 6
    or more captures; the noise-band counts (strike and no-fire clears with f1 in 355-395 Hz).

**Checked how.** The selftest asserts these on the registered plan with synthetic clears; 37 single-rule mutations
of the tool each fail it. On the real bundles the control sets count 40/18/1 and all 42 B3.2 air shots join their
plan trials. Not checked: any strike data, because none exists. The tool has not been run on a complete campaign.

## B3.2 strike blocks - interim record (2026-09-20)

### Block 1 of 10: `B3.2-RIGHT-sp17` (trials 1-24)

Run 2026-09-20, 16:20-16:23 local, `python tools/campaign_runner.py --plan docs/campaign_plans/b32.json --no-prompt
--start-block 0 --end-block 1`. Build `2511fdf`, plan sha256 `be053f63cef29608abd7a7026ba6d7fa2b1f567b03cdccd688882e5f8e9a0904`,
`RIGHT/rig-1`. **24 kept, 0 excluded, every trial on its first try; no ledger file was written.** PCM manifest sha256
over the 24 bundles (sorted `name:pcm_sha256` lines): `aba051b85b678ba059941230cff0a28b126e8767925777c67d25eb0af475f259`
(`_campaign/`, gitignored, this machine only).

**New rig condition, declared for every strike block from here on: the wheel is held by a pillow resting on the rim.**
The operator built it so the wheel stays aligned with nobody in the room; it is stamped into each bundle's `note`. It
was first used this morning. The 42 B3.2 air shots and B3.0's controls were taken without it as far as this record
shows (the wheel left at a gap; the operator's account of the pillow is this morning's, not confirmed for earlier
runs). The interleaved no-fire controls in the strike blocks are taken with it; the air shots are not. Whether the
pillow changes how the spokes or the structure ring is not known and is not measured here.

**Baseline results (the firmware's own status, as taken).** Strikes 0/18 clears: 15 `AMBIGUOUS_PEAK`, 3 `LOW_SNR`,
their f1 all between 350 and 366 Hz (the bottom of the band, where the mains harmonic sits). No-fire controls 1/6
clears: trial 8, `suspect` at 13.0 dB and 366 Hz; the other five `rejected`. Quiet-room loudest events were
-21.6 to -23.2 dBFS, the same as B3.0's no-fire controls; neighbourhood morning noise outside did not change that.

**What a first look at the audio shows. This is a description, not a selection, and nothing here changes a rule.**
- The plunger strikes are loud. 17 of 18 have 1 to 22 samples at the converter rail around the impact (about 0.16-0.21
  s into the capture), never more than 2 in a row, over spans up to 16.6 ms; most of it precedes the analysed window.
- After the impact there is ring. Between 1.0 and 2.5 kHz dozens of lines are present in 15-18 of the 18 strikes at
  15-35 dB over the no-fire floor. Inside the 350-600 Hz band there are lines at 398.8, 492.5, 506.2, 541.2, 567.5 and
  578.8 Hz in 14-18 of 18 strikes, 12-17 dB over that floor. 506 and 541 Hz sit on the RIGHT air-shot tones recorded
  above (508.6, 529.5, 542 Hz), so the plunger-stop ring is present in strikes too. Whether any line is the spoke is not
  known; the cross-spoke report over all four RIGHT spokes is what registration Amendment 2 provides for that.
- A sanity sweep of these 24 bundles alone (nothing selected, nothing acted on): no registered candidate clears more
  than 1 of the 18 strikes, and the 3 that clear one do so at 360 Hz; 19 of the 27 evaluable candidates clear at
  least one of the 6 no-fire controls. One spoke and one block say nothing about the other seven blocks.

**Not established:** anything about the other spokes, any candidate, any level, or whether the rings are the spoke or the
structure. The remaining blocks run as registered (fixed n; no outcome-dependent stopping). If a station misses the
exploration gate, Amendment 2 already names damping the plunger stops as a candidate for the one bounded mechanical
lever, by a small amendment at that point.

Pretest, not campaign data (scratch bundles, no plan, no trial numbers): this morning, before block 1, six no-fire
captures and four fired shots (20 ms and 72 ms at each station) confirmed both stations fire, are recorded as fired at
the requested width and station, and that the 72 ms impact is clearly audible; the 20 ms impact is not distinguishable
from room noise in the audio. One no-fire capture had a loud neighbourhood sound at its start and cleared.

### Block 2 of 10: `B3.2-RIGHT-sp5` (trials 25-48)

Run 2026-09-20, 16:38-16:42 local, `--start-block 1 --end-block 2`, same command shape, build `2511fdf`, `RIGHT/rig-1`,
same pillow note. **24 kept, 0 excluded, every trial on its first try; no ledger file.** PCM manifest sha256 over the 24
bundles (sorted `name:pcm_sha256` lines): `95756e8cc8e2da56c47b2accb88d9be5276fb2ef5fbb8dd046e54a0354d23ed6`
(`_campaign/`, gitignored, this machine only). Spoke 5 is RIGHT-leading; block 1's spoke 17 is also RIGHT-leading
(17 mod 4 = 1).

**Baseline results (firmware status as taken).** Strikes 0/18 clears, all 18 `LOW_SNR` (SNR -1.2 to 9.1 dB, all under the
12 dB gate; f1 352-422 Hz). No-fire controls 0/6 clears (3 `AMBIGUOUS_PEAK`, 3 `LOW_SNR`; SNR 2.1 to 9.3 dB). Per width, the
median strike SNR was 6.1 dB at 60 ms, 7.3 dB at 72 ms and 3.9 dB at 85 ms (6 strikes each): nothing here supports a
ranking of widths and none is made. Block 1's strikes were mostly `AMBIGUOUS_PEAK`; this block's are all `LOW_SNR`, and
what separates the two is not looked at yet. No audio look, no sweep, nothing selected and no rule touched.

### Block 3 of 10: `B3.2-RIGHT-sp3` (trials 49-72)

Run 2026-09-20, 16:45-16:50 local, `--start-block 2 --end-block 3`, build `2511fdf`, `RIGHT/rig-1`, same pillow note.
**24 kept, 0 excluded, every trial on its first try; no ledger file.** PCM manifest sha256 over the 24 bundles (sorted
`name:pcm_sha256` lines): `64113eae8dc3594fa17fc0b59eb63dd3948303ac0eed6f0c0b2bb69d2fd948fe` (`_campaign/`, gitignored,
this machine only). Spoke 3 is RIGHT-trailing (3 mod 4 = 3), the first RIGHT block of the other class.

**Baseline results (firmware status as taken).** Strikes 0/18 clears: 11 `AMBIGUOUS_PEAK`, 7 `LOW_SNR` (SNR -0.8 to 11.7
dB, none at the 12 dB gate; f1 350-362 Hz, the bottom of the band). No-fire controls 0/6 clears (1 `AMBIGUOUS_PEAK`, 5
`LOW_SNR`; SNR -0.4 to 11.5 dB, f1 351-361 Hz). Strike and no-fire results are not separated by SNR or f1 range in this
block. No audio look, no sweep, nothing selected and no rule touched.

### Block 4 of 10: `B3.2-RIGHT-sp15` (trials 73-96)

Run 2026-09-20, 16:59-17:04 local, `--start-block 3 --end-block 4`, build `2511fdf`, `RIGHT/rig-1`, same pillow note.
**24 kept, 0 excluded, every trial on its first try; no ledger file.** PCM manifest sha256 over the 24 bundles (sorted
`name:pcm_sha256` lines): `84aaeac455ed58bbca57b047f9679add036bdcf8e890d74cff70049b99a06566` (`_campaign/`, gitignored,
this machine only). Spoke 15 is RIGHT-trailing (15 mod 4 = 3), the same class as block 3's spoke 3. This closes the RIGHT
strike blocks.

**Baseline results (firmware status as taken).** Strikes **3/18 clears**, the first strike clears of the campaign: trials
77 (85 ms, 15.2 dB, 430.0 Hz), 90 (60 ms, 15.3 dB, 430.3 Hz) and 95 (60 ms, 13.3 dB, 430.2 Hz); the other 15 are 12
`LOW_SNR` and 3 `AMBIGUOUS_PEAK` (f1 350-373 Hz). No-fire controls 0/6 clears (2 `AMBIGUOUS_PEAK`, 4 `LOW_SNR`; SNR 5.5 to
12.8 dB, f1 350-359 Hz), so no false clear in this block. The three clears fall within 0.3 Hz of one another; none is at
72 ms; three clears at three trial numbers say nothing about widths.

**Across the four RIGHT blocks, baseline as taken:** strikes 3/72 (all on spoke 15), no-fire 1/24 (block 1, trial 8). No
strike on spokes 17, 5 or 3 has f1 within 425-435 Hz, and 430 Hz is not among the in-band lines listed for block 1 above.
Whether 430 Hz is the spoke, a structure line that only this spoke's position excites, or chance is not known: a reported
peak frequency is not a claim about the spoke's pitch (outcome vocabulary). No audio look at the other spokes, no sweep,
nothing selected and no rule touched. Exploration-gate status for RIGHT is decided only by the registered selection over
all blocks, not from this baseline.

### Listening copies of the RIGHT strike blocks (2026-09-20)

The 96 captures of blocks 1-4 were exported as WAV (faithful plus boosted listening copies, `--verify` read-back exact) to
`_wav/b32_right_blocks1-4_0920/` (gitignored, this machine only), with a manifest and a blank notes sheet. The operator
will listen later and give notes. That listening is informal and unblinded (file names show strike or no-fire, and the
manifest shows the firmware status). It does not select anything, changes no rule, and is not the registered blind review,
which is drawn later from all blocks with the machine verdicts sealed. Whatever the operator reports is recorded here as
their account when it is given.

### RIGHT blocks 1-4: first audio comparison across the four spokes, and a correction (2026-09-20)

Descriptive only; nothing selected, no rule touched, no candidate evaluated. Written to `_wav/TO_CONFIRM.md` for the operator.

**Correction.** Block 4's record above says no strike on spokes 17, 5 or 3 has f1 near 430 Hz. That is true of the
firmware's *reported* f1 and it read as if the line were only on spoke 15. In the audio (strongest bin in 420-440 Hz, 0.30-1.10
s, 1.25 Hz bins) it sits at exactly 430.0 Hz in 18/18 spoke-15 strikes (+12 to +16 dB over the 24 pooled RIGHT no-fire
controls) and in 12/18 spoke-5 strikes (median +16 dB); it is not present on spoke 17 (median +1 dB) and is broad, not at
430.0, on spoke 3 (+11 dB). The firmware cleared 3/18 on spoke 15 because in the other 15 a lower line at 350-373 Hz was the
lowest strong peak. Whether 430 Hz is the spoke or the structure is not known.

**Block 2 (spoke 5) differs from the other three.** Loudest 10 ms frame in strikes: median -15.6 dBFS against -1.3 to -2.2
in blocks 1, 3, 4; no rail samples in any of its 18 strikes (blocks 1, 3, 4: 17/18, 17/18, 18/18 strikes have some); analysed
window ended by the decay floor in 17 of 24 captures (0, 0 and 1 in the others); median analysed window 384 ms (182-500) against
500 ms. The initial click is similar (peak -1.5 dBFS median). Cause not known; put to the operator.

**Other, unremarkable.** No overruns, one onset in every capture, impact 0.16-0.22 s in every strike, no-fire controls' loudest
events -22.3 to -26.0 dBFS in every block. The worst read gap maximum is 40.6 ms in all four blocks (unexplained repeat).

### Operator answers on the RIGHT blocks 1-4 questions (2026-09-20, their account)

The pillow was in the same spot for all four RIGHT blocks. The mic and towers were untouched since the pretest. The counting
from S0 was clear for spokes 17, 5, 3 and 15. The operator saw nothing different about spoke 5 (block 2) or spoke 17 (block 1),
so the ~14 dB quieter ring in block 2 has no known cause and none is assumed. The operator also confirmed there was no pillow
for "the automatic 10 air shots"; the earlier "as far as this record shows" wording therefore stands confirmed for the air
shots. Not yet answered: where the pillow sits for the LEFT blocks, which is recorded when given.

### Block 7 of 10: `B3.2-LEFT-sp10` (trials 139-170), the first LEFT block

Run 2026-09-20, 17:38-17:42 local, `--start-block 6 --end-block 7`, build `2511fdf`, `LEFT/rig-1`. The `--note` stamped in each
bundle says the wheel was held by a pillow resting on the rim, in the same spot as for the RIGHT blocks (operator's account),
and that the air shots were taken without it. **32 kept (24 strikes at 40/60/72/85 ms, 6 each; 8 no-fire), 0 excluded, every
trial on its first try; no ledger file.** PCM manifest sha256 over the 32 bundles (sorted `name:pcm_sha256` lines):
`ab2b2ed613d491b9fb4fae8576752b869b7d136e463b4d12d6ee93b9483ba39c` (`_campaign/`, gitignored, this machine only). Spoke 10 is
LEFT-trailing (10 mod 4 = 2).

**Baseline results (firmware status as taken).** Strikes 1/24 clears: trial 157 (40 ms, `suspect`, 19.5 dB, f1 352.8 Hz); the
other 23 are 17 `LOW_SNR` and 6 `AMBIGUOUS_PEAK` (f1 350-375 Hz). **No-fire controls 1/8 clears: trial 166 (13.5 dB, f1 352.2
Hz), a false clear at LEFT.** The one strike clear and the one no-fire clear are 0.6 Hz apart near the bottom of the band, where
the mains harmonic sits; that is recorded, not interpreted. Median strike SNR by width: 6.1 dB at 40 ms, 3.9 at 60, 5.5 at 72,
5.0 at 85 (6 strikes each): no ranking of widths is made.

**Audio, descriptive only.** Strikes are quieter at the mic than the RIGHT blocks 1, 3, 4, as expected from distance: peak -2.1
dBFS median (-4.0 to -0.4), no strike touches the converter rail, loudest 10 ms frame median -17.2 dBFS (blocks 1, 3, 4 were -1.3
to -2.2; block 2 -15.6). The analysed window was ended by the decay floor in 19 of 32 captures (median 403 ms, 81-500). Lines in
350-600 Hz that stand at least 10 dB over this block's no-fire floor in at least 60% of the strikes: 352.5 Hz (20/24, +11.9
dB), 386.2 (24/24, +21.1), 482.5 (24/24, +22.8), 530.0 (24/24, +21.7), 543.8 (22/24, +16.5), 571.2 (15/24, +11.5), 585.0 (24/24,
+24.6). Not yet compared with the LEFT air-shot lines. No overruns, one onset per capture, impact 0.15-0.23 s, no-fire loudest
frames -23.1 to -23.8 dBFS, worst read gap max 40.6 ms again.

**Dog.** The operator reported, as block 7 finished, that their dog barked; when is not known (the last capture was fetched at
17:42:45, the report came about 17:44). A scan of all 32 captures (frames above 200 Hz, excluding each strike and its first 0.6
s) finds nothing above -44 dBFS inside any capture; five captures have energy at -39.8 to -43.6 dBFS in their first or last
frame (trials 150, 152, 155, 160, 163), which may be an edge artifact of the scan or something at the capture boundary. No
trial is excluded: none of the five registered exclusion codes covers room noise, and the no-fire controls exist to absorb it.

The listening copies described above now live in `_wav/B3.2_strike_blocks_0920/`, one folder per block with `norm/` (boosted
listening copies) and `faithful/` (real levels) inside each, plus a manifest, a blank notes sheet and a README; it holds
blocks 1-4 and 7 so far and gains each later block. The `_wav/b32_right_blocks1-4_0920/` folder named earlier was removed
after the move (its notes sheet was blank; the files are regenerated from the bundles). The operator says the dog that may
have barked around block 7 is downstairs.

### Block 8 of 10: `B3.2-LEFT-sp22` (trials 171-202)

Run 2026-09-20, 18:03-18:07 local, `--start-block 7 --end-block 8`, build `2511fdf`, `LEFT/rig-1`, same pillow note as block 7
(same spot as for the RIGHT blocks, operator's account). **32 kept (24 strikes, 8 no-fire), 0 excluded, every trial on its first
try; no ledger file.** PCM manifest sha256 over the 32 bundles (sorted `name:pcm_sha256` lines):
`9b477154c6433bb10ee10e6c8f3b465277d0b2372160b98b2ee97b3d7f40da05` (`_campaign/`, gitignored, this machine only). Spoke 22 is
LEFT-trailing (22 mod 4 = 2), the same class as spoke 10.

**Baseline results (firmware status as taken).** Strikes **0/24 clears**, all `LOW_SNR` (SNR -1.2 to 11.8 dB, none at the 12 dB
gate; f1 350-426 Hz). **No-fire controls 1/8 clears: trial 190 (13.6 dB, f1 350.0 Hz, the bottom edge of the band), a second
false clear at LEFT, one per LEFT block so far.** Median strike SNR by width: 2.2 dB at 40 ms, 4.4 at 60, 2.2 at 72, 2.8 at 85
(6 each): no ranking is made.

**Audio, descriptive only.** Strike peak -0.9 dBFS median but -10.7 to -0.1 (at least one strike is far softer than the rest;
not examined), no strike touches the rail, loudest 10 ms frame median -15.6 dBFS; analysed window ended by the decay floor in 15
of 32 captures (median 329 ms, 147-500). Lines in 350-600 Hz at least 10 dB over this block's no-fire floor in at least 60% of the
strikes: 397.5 Hz (15/24, +11.8), 491.2 (23/24, +24.9), 500.0 (20/24, +18.9), 531.2 (23/24, +17.5), 548.8 (22/24, +21.0), 567.5
(22/24, +17.3), 580.0 (23/24, +23.5). Block 7's (spoke 10) were 386.2, 482.5, 530.0, 543.8 and 585.0 Hz: only about 530 Hz is in
both; not interpreted. No overruns, one onset per capture, impact 0.16-0.24 s, no-fire loudest frames -22.6 to -24.0 dBFS. The
worst read gap maximum is 41.2 ms (40.6 in every earlier block). A room-event scan finds nothing mid-capture; edge-frame energy
at -39.7 to -40.7 dBFS in trials 176, 181 and 194, as in block 7. Listening copies added to `_wav/B3.2_strike_blocks_0920/`.

Block 8 follow-up (2026-09-20, descriptive): the two softest strikes are both 40 ms. Trial 183 has peak -10.7 dBFS and a loudest
10 ms frame of -23.6 dBFS, about the level of a no-fire control (-22.6 to -24.0); trial 188 has peak -5.3 and loudest frame
-21.7. The other 22 strikes have loudest frames of -12.7 dBFS or louder. Whether either was a weak or missed tap is not known;
both stay in as taken (no exclusion code applies) and are put to the operator to listen to.

### Block 9 of 10: `B3.2-LEFT-sp12` (trials 203-234), stopped and resumed

Build `2511fdf`, `LEFT/rig-1`, same pillow note as blocks 7 and 8. Spoke 12 is LEFT-leading (12 mod 4 = 0). **The first run
(`--start-block 8 --end-block 9`, 18:55) was stopped by the operator's accidental keypress after trials 203, 204 and 205 were
recorded** (seq 171-173: strikes at 72, 72 and 85 ms, all `LOW_SNR`). The board had by then executed plan trial 206 (a no-fire
control on spoke 12; seq 174, `fired=false`, capture OK, `AMBIGUOUS_PEAK`, 11.4 dB, 365.7 Hz, not a clear) but the runner had not
recorded it. That capture was saved from the board's ring before any further shot as an orphan, not a plan trial:
`_campaign/_orphans/ORPHAN_interrupted_B3.2-LEFT-sp12_no_fire_sp12_seq174.*` (PCM sha256
`ecfdd012b9e2b4ae56fba3fe6e91085c835630a744da35d85c0aae3d09039628`), outside every directory the analysis tools read. The block
was then resumed at trial 206 with `--start-trial 206` (added to the runner in `e8f1d5e`, which keeps the plan's numbering and
changes no rule or exclusion code), about 19:00-19:04, and trial 206 was run fresh. No trial was chosen or dropped by its
result: the orphan is a no-fire that was lost to the stop and its outcome is disclosed above. Operator's account of whether the
wheel was touched between the stop and the restart: not yet given. **All 32 plan trials are recorded once (203-234; trials 1-234
have no duplicates or gaps); 0 excluded; no ledger file.** PCM manifest sha256 over the 32 bundles (sorted `name:pcm_sha256`
lines): `f42ff0875179f81e4192ec2972e0e90aa4a90791c165910e860c78d2e1de9dd6` (`_campaign/`, gitignored, this machine only).

**Baseline results (firmware status as taken).** Strikes **0/24 clears** (23 `LOW_SNR`, 1 `AMBIGUOUS_PEAK`; SNR -5.1 to 7.6 dB;
f1 354-401 Hz). **No-fire controls 1/8 clears: trial 210 (12.0 dB, f1 359.5 Hz), a third false clear at LEFT, one per LEFT block.**
Median strike SNR by width: -0.9 dB at 40 ms, 4.6 at 60, 4.0 at 72, 0.8 at 85 (6 each): no ranking is made.

**Audio, descriptive only.** Strike peak -2.6 dBFS median (-11.0 to -1.1), none touches the rail, loudest 10 ms frame median
-17.8 dBFS; analysed window ended by the decay floor in 24 of 32 captures (median 284 ms, 170-500). The only lines in 350-600 Hz
at least 10 dB over this block's no-fire floor in at least 60% of strikes: 436.2 Hz (20/24, +15.9), 571.2 (15/24, +10.8) and
578.8 (16/24, +11.4); spokes 10 and 22 did not show 436.2. No overruns, one onset per capture, impact 0.15-0.23 s, worst read
gap max 41.2 ms. No-fire loudest frames -19.0 to -24.2 dBFS; trial 218 is the loud one (-19.0), cause not known. Room-event scan:
nothing mid-capture; edge-frame energy at -37.0 to -38.8 dBFS in trials 223, 231, 232. Listening copies added to
`_wav/B3.2_strike_blocks_0920/`.

### Block 10 of 10: `B3.2-LEFT-sp0` (trials 235-266): the last strike block

Run 2026-09-20, 19:14-19:19 local, `--start-block 9 --end-block 10`, build `2511fdf`, `LEFT/rig-1`, same pillow note as blocks 7-9.
Spoke 0 is the S0 spoke, LEFT-leading (0 mod 4 = 0). **32 kept (24 strikes, 8 no-fire), 0 excluded, every trial on its first
try; no ledger file.** PCM manifest sha256 over the 32 bundles (sorted `name:pcm_sha256` lines):
`5b9a17e039a86d5721c17d24fd74ccf400c40b56ce000f49ae7006c2519f9e21` (`_campaign/`, gitignored, this machine only). The operator's
account: the wheel was not touched between the stop and the restart of block 9.

**Baseline results (firmware status as taken).** Strikes **0/24 clears** (20 `LOW_SNR`, 4 `AMBIGUOUS_PEAK`; SNR -6.0 to 6.3
dB; f1 351-422 Hz). **No-fire controls 2/8 clears: trial 246 (15.4 dB, f1 352.0 Hz) and trial 250 (13.3 dB, f1 361.9 Hz).**
Median strike SNR by width: -2.0 dB at 40 ms, 3.1 at 60, 3.9 at 72, 4.3 at 85 (6 each): no ranking is made.

**Audio, descriptive only.** Strike peak -3.5 dBFS median (-6.5 to -1.0), none touches the rail, loudest 10 ms frame median -19.1
dBFS (the quietest LEFT block); analysed window ended by the decay floor in 16 of 32 captures (median 421 ms, 184-500). Lines in
350-600 Hz at least 10 dB over this block's no-fire floor in at least 60% of strikes: 426.2 Hz (23/24, +18.6), 546.2 (17/24,
+14.8) and 592.5 (24/24, +29.9). Block 9's (spoke 12, also LEFT-leading) was 436.2 Hz, not these; not interpreted. No overruns,
one onset per capture, impact 0.15-0.22 s, worst read gap max 41.2 ms, no-fire loudest frames -22.3 to -23.7 dBFS. Room-event
scan: nothing mid-capture; edge-frame energy at -39.1 to -39.4 dBFS in trials 243, 255, 263. Listening copies added to
`_wav/B3.2_strike_blocks_0920/`.

### B3.2 data taking is complete (2026-09-20)

**All 266 planned trials are recorded once** (bundles for trials 1-266: 168 strikes, 56 no-fire, 42 air shots; no duplicates, no
gaps; one plan sha256 `be053f63cef29608abd7a7026ba6d7fa2b1f567b03cdccd688882e5f8e9a0904`; one build `2511fdf`); **0 exclusions, no
ledger file.** One disclosed orphan: the unrecorded no-fire of the interrupted block 9 (above), kept outside every analysed
directory. Baseline firmware status as taken, strike blocks only (air shots not counted here): strikes **4/168 clears** (three on
RIGHT spoke 15 near 430 Hz, one on LEFT spoke 10 at 352.8 Hz); no-fire controls **6/56 clears** (RIGHT 1/24; LEFT 5/32: trials
166, 190, 210, 246, 250, all near the bottom of the band). Correction to two chat summaries of this session: the no-fire count
after block 8 was 3/40, not 3/46, and 168 is the campaign's total strike count, not the number run at that point. Nothing has been
selected; the registered offline sweep and selection are next.


### B3.2 offline sweep and registered selection (2026-09-20, after all 266 trials)

**Inputs.** `native_sweep` (`pio test -e native_sweep -v`, `TRUING_SWEEP_MODE` unset, onset floor as it is) over `_campaign/`
(376 bundles listed in its index: the 266 B3.2 trials, the 40 B3.0 controls and the earlier pass bundles; 14 min
07 s) and over `test/fixtures/acoustic/captures/` (the ambient fixture and the others; 8 s), both PASSED. Raw pio output
sha256 (local scratch files, they include timing lines): campaign `1b67dc0cf26f6002ccbeec5991a95a598fe0aa8d0e935c43521147a5c0cb9d29`, fixtures `13c11c49cf3e315a45b5ad94318649c1ea16ac6eb9714f3037295ce0eed4b446`. Selection: `python tools/b32_select.py
--plan docs/campaign_plans/b32.json --dir _campaign --sweep <campaign> --dir captures --sweep <fixtures>` (no `--lines` CSV yet, so the
air-shot line report is not in it), tool at `f83983d`. Its complete output, sha256 `c996acd50d3a982fe8582e2f0c990704867377fe450c1214b8fa70d3454eba38`:

```
B3.2 selection report
plan sha256 be053f63cef29608abd7a7026ba6d7fa2b1f567b03cdccd688882e5f8e9a0904

kept trials: 168 strikes, 42 air shots, 56 no-fire; control sets B3.0 40, B3.2 no-fire 56, ambient 1, pass-C 18

candidate (registered order)       state          detail
baseline=0                         inadmissible   false clears on controls: B3.2 no-fire 6
gate_start_ms=100                  inadmissible   false clears on controls: B3.0 1, B3.2 no-fire 3
gate_start_ms=200                  inadmissible   false clears on controls: B3.2 no-fire 3
gate_start_ms=300                  inadmissible   false clears on controls: B3.2 no-fire 3
gate_start_ms=500                  inadmissible   false clears on controls: B3.0 3, B3.2 no-fire 4
gate_start_ms=800                  not evaluable  the validator refuses this chain profile
window_ms=250                      inadmissible   false clears on controls: B3.0 1, B3.2 no-fire 10, pass-C 1
window_ms=750                      inadmissible   false clears on controls: pass-C 1
prominence_db=9                    inadmissible   false clears on controls: B3.2 no-fire 10
prominence_db=12                   inadmissible   false clears on controls: B3.0 2, B3.2 no-fire 13
gate_start_ms+window_ms=100+250    inadmissible   false clears on controls: B3.0 2, B3.2 no-fire 8, pass-C 1
gate_start_ms+window_ms=100+750    inadmissible   false clears on controls: B3.0 1, B3.2 no-fire 2
gate_start_ms+window_ms=200+250    inadmissible   false clears on controls: B3.0 3, B3.2 no-fire 9
gate_start_ms+window_ms=200+750    inadmissible   false clears on controls: B3.2 no-fire 1
gate_start_ms+window_ms=300+250    inadmissible   false clears on controls: B3.0 6, B3.2 no-fire 8
gate_start_ms+window_ms=300+750    not evaluable  the validator refuses this chain profile
gate_start_ms+window_ms=500+250    inadmissible   false clears on controls: B3.0 6, B3.2 no-fire 7
gate_start_ms+window_ms=500+750    not evaluable  the validator refuses this chain profile
gate_start_ms+window_ms=800+250    not evaluable  the validator refuses this chain profile
gate_start_ms+window_ms=800+750    not evaluable  the validator refuses this chain profile
gate_start_ms+prominence_db=100+9  inadmissible   false clears on controls: B3.0 1, B3.2 no-fire 7
gate_start_ms+prominence_db=100+12 inadmissible   false clears on controls: B3.0 3, B3.2 no-fire 11
gate_start_ms+prominence_db=200+9  inadmissible   false clears on controls: B3.0 2, B3.2 no-fire 11
gate_start_ms+prominence_db=200+12 inadmissible   false clears on controls: B3.0 3, B3.2 no-fire 15
gate_start_ms+prominence_db=300+9  inadmissible   false clears on controls: B3.0 1, B3.2 no-fire 4
gate_start_ms+prominence_db=300+12 inadmissible   false clears on controls: B3.0 5, B3.2 no-fire 9
gate_start_ms+prominence_db=500+9  inadmissible   false clears on controls: B3.0 8, B3.2 no-fire 7
gate_start_ms+prominence_db=500+12 inadmissible   false clears on controls: B3.0 10, B3.2 no-fire 10
gate_start_ms+prominence_db=800+9  not evaluable  the validator refuses this chain profile
gate_start_ms+prominence_db=800+12 not evaluable  the validator refuses this chain profile
window_ms+prominence_db=250+9      inadmissible   false clears on controls: B3.0 2, B3.2 no-fire 9, pass-C 1
window_ms+prominence_db=250+12     inadmissible   false clears on controls: B3.0 3, B3.2 no-fire 10, pass-C 1
window_ms+prominence_db=750+9      inadmissible   false clears on controls: B3.2 no-fire 2, pass-C 1
window_ms+prominence_db=750+12     inadmissible   false clears on controls: B3.2 no-fire 8, pass-C 1

HALT G-ANALYSIS: no candidate is eligible. A per-station analysis profile is an architecture decision.

Median SNR per level (dB; the board's own expect_snr_db, recorded only where its baseline analysis found an f1; n):
  LEFT  40 ms  1.5  n=24   lead -1.7 (n=12)   trail 3.4 (n=12)
  LEFT  60 ms  3.9  n=24   lead 3.7 (n=12)   trail 3.9 (n=12)
  LEFT  72 ms  4.4  n=24   lead 3.9 (n=12)   trail 4.6 (n=12)
  LEFT  85 ms  4.2  n=24   lead 3.6 (n=12)   trail 4.5 (n=12)
  RIGHT 60 ms  6.1  n=24   lead 6.1 (n=12)   trail 5.9 (n=12)
  RIGHT 72 ms  3.9  n=24   lead 3.5 (n=12)   trail 5.1 (n=12)
  RIGHT 85 ms  4.0  n=24   lead 3.7 (n=12)   trail 4.1 (n=12)
Reported, not gating (per station and level):
  LEFT  40 ms  kept 24  exclusions none  overruns kept 0 / excluded 0  window truncated by decay_floor 15, window_ms 9
  LEFT  60 ms  kept 24  exclusions none  overruns kept 0 / excluded 0  window truncated by decay_floor 16, window_ms 8
  LEFT  72 ms  kept 24  exclusions none  overruns kept 0 / excluded 0  window truncated by decay_floor 20, window_ms 4
  LEFT  85 ms  kept 24  exclusions none  overruns kept 0 / excluded 0  window truncated by decay_floor 20, window_ms 4
  RIGHT 60 ms  kept 24  exclusions none  overruns kept 0 / excluded 0  window truncated by decay_floor 5, window_ms 19
  RIGHT 72 ms  kept 24  exclusions none  overruns kept 0 / excluded 0  window truncated by decay_floor 6, window_ms 18
  RIGHT 85 ms  kept 24  exclusions none  overruns kept 0 / excluded 0  window truncated by decay_floor 6, window_ms 18
Air groups, in-band lines: not reported (no --lines CSV given)

This report applies the registered rules and changes no constant. A choice here is a candidate for the
B3.3 freeze, which is halt-and-ask; a flagged cell in the cross-spoke report (tools/b32_crossspoke.py)
holds that approval until the operator has looked at it.
```

**Result, as the registered rules give it: HALT G-ANALYSIS.** Of the 34 candidates, 7 cannot run at this capture length
(recorded earlier) and **all 27 evaluable candidates are inadmissible under rule 1** (0 false clears across every control set),
the baseline included (6 false clears in the B3.2 no-fire controls). The candidates closest to admissible: `window_ms=750` (1
false clear, in a pass-C control), `gate_start_ms+window_ms=200+750` (1, a B3.2 no-fire at LEFT), `gate_start_ms+window_ms=100+750`
(3) and `window_ms+prominence_db=750+9` (3). No candidate reaches rule 2, so the exploration gate is not evaluated by the tool. Per the
plan a per-station analysis profile is an architecture decision, and any change to a rule, the candidate set or the control
sets after this data is a registered-rule change: **halt and ask the operator.** Nothing is selected and no constant changes.

**Descriptive only, not a selection and not used by any rule.** Among the registered candidates the most strike clears pooled is
`gate_start_ms+prominence_db=300+12` with 43/168 (LEFT 24/96, RIGHT 19/72) and false clears in the controls; by spoke it clears
RIGHT 17 1/18, RIGHT 5 18/18, RIGHT 3 0/18, RIGHT 15 0/18, LEFT 10 13/24, LEFT 22 7/24, LEFT 12 2/24, LEFT 0 2/24. The worst spoke is
0 for every one of the 27 evaluable candidates (checked over all of them: the best worst-spoke rate is 0.000 against the gate's 0.833), so the exploration gate (worst spoke at least 5/6) would fail at both stations even if rule 1 were set aside.
The clears on RIGHT spoke 5 with a 300 ms gate are the same spoke whose strikes were about 14 dB quieter at the mic.

Follow-up to the descriptive note above (2026-09-20, same status: not a selection, not used by any rule). The 300 ms-gate clears
that look like a rescue on RIGHT spoke 5 and LEFT spoke 10 are at **f1 353.8 Hz (spoke 5, 17 clears within 353.5-354.1),
352.6 Hz (spoke 10, 10 clears within 351.8-352.7) and 355.0 Hz (spoke 22, 6 clears)**: the bottom of the band, where the
no-fire controls' own lowest strong peaks sit (351-363 Hz in the baseline results; the no-fire clears at LEFT are 350.0-361.9 Hz).
The no-fire captures in those blocks do not clear under a 300 ms gate, but the window is anchored on the onset, and a no-fire
capture's onset is whatever noise triggered it, so strike and no-fire windows are not the same thing and that comparison does
not show the line is the spoke's. What is established: the clears are tightly consistent within a block and differ by 1-2 Hz
between blocks. What is not: whether they are a spoke or structure tone or a persistent room line near 353 Hz. They are not
evidence that these spokes are readable, and no reading of them is to be carried into the demo or B3.3 as one.

### Diagnostic after the G-analysis halt: the excitation, not the analysis (2026-09-21)

Prompted by the operator, who listened to the block WAVs and said the no-strike ambience does not sound like electrical hum
(they play guitar), that the strikes do not ring enough, and that a guitar tuner reads a plucked string with no windowing at
all. Descriptive, outside the registration, selects nothing and changes no constant. All levels below are Hann-windowed
in-band (350-600 Hz) RMS over equal 500 ms windows, on the same bundles.

**1. The 350-366 Hz f1 is not a mains line. The earlier "hum" reading in this record's chat-level summaries was wrong.**
Averaged over 40 B3.2 no-fire controls, the 60 Hz harmonics stand over their own local broadband floor by: 60 Hz +9.3 dB,
120 +0.8, 180 +4.4, 240 +3.4, 300 -2.2, **360 +0.4**, 420 -1.6, 480 -2.1, 540 +1.4, 600 +0.4. There is effectively no line at
360 Hz. What the DSP reports as f1 at 350-366 Hz is the top of the broadband floor at the bottom edge of the search band,
which slopes downward with frequency; "the lowest strong peak" lands there because that is where the floor is highest, not
because a tone is there. B3.0's line finding is not contradicted (different mode, onset floor disabled), but the reading that
a mains tone drives these rejections is withdrawn. The operator's ear was right and the earlier hypothesis was not tested
before it was acted on.

**2. The chain's noise is dominated by content below 125 Hz, far under the search band.** Of the 20-16000 Hz power in a
no-fire capture, 41.9 % is in 20-60 Hz and 48.8 % in 60-125 Hz; 250-500 Hz holds 3.7 %. In the time domain the mean of each
50 ms block swings about 1.0 million counts of 8388608 peak-to-peak. At the INMP441's datasheet sensitivity a 30 dB SPL room
would move about 265 counts. This is drift or sub-audio content, not room sound. It costs headroom (it is part of why RIGHT
strikes touch the rail) but it sits below the band and is not what the peak picker sees.

**3. The excitation delivers far less into the measurement band than a hand pluck did on this same board and mic.**

| capture | in-band 350-600 Hz | board's own SNR |
|---|---|---|
| B3.2 no-fire control, nothing fired (n=56) | -63.7 dBFS | 8.1 dB median |
| B3.2 solenoid strike, ring after impact (n=168) | -56.8 dBFS | **4.0 dB median** |
| `nano_handpluck_provisional`, same chain (n=1) | -46.6 dBFS | 15.4 dB |
| `golden_ts03_e2`, the research fixture (n=1) | -28.8 dBFS | 75.6 dB |

The strike puts about 6.9 dB into the band over silence; the one hand pluck on this chain put about 17 dB, **10 dB more than
the solenoid**. And the strikes' median SNR (4.0 dB) is *below* the no-fire controls' (8.1 dB): firing makes the board's own
metric worse than not firing, because the impact's broadband energy lifts the floor around whatever peak is picked. n=1 for
the pluck, so it bounds nothing; it is one same-chain reference point, and it points at the excitation.

**4. The solenoid does not disturb the microphone electrically.** In the 100 ms before the plunger lands, in-band level is
-63.2 dBFS in strike captures and -62.9 dBFS in no-fire captures (0.4 dB apart, n=168 and 56), and the low-frequency wander is
the same (about 1.03 M counts against 0.96 M). The operator's supply/ground-coupling concern is not supported by these
captures: the B0 wiring looks clean on this evidence.

**5. The DSP constants were fitted on a different transducer and a different excitation.** The research campaign behind
`golden_ts03_e2` plucks a spoke and captures it with a **magnetic pickup** (`Acoustic Repo/README.md`), a proximity sensor
watching one spoke, with no room path and no rig ring. The search band, the 12 dB SNR gate and the 6 dB prominence come from
that signal. Phase A already recorded this chain as 26-32 dB noisier; the table above puts the in-band gap at about 28 dB
between the research fixture and a hand pluck here, and about 38 dB between it and a solenoid strike. Applying those constants
to a MEMS microphone in a room hearing a hammer blow is a provenance mismatch that this campaign carried without testing it.

**What this does and does not establish.** Established: the strike delivers little in-band energy; the hum reading was wrong;
the coil does not pollute the mic. Not established: that a pluck-style excitation would work here (one capture), that any
particular frequency is a spoke, or that a different algorithm would recover more from these captures. **Untested and cheap:**
the campaign's estimator picks a peak from one windowed FFT, whereas an instrument tuner tracks periodicity continuously over
a sustained ring (autocorrelation/YIN-style) and needs no window placement. Whether a periodicity estimator finds a consistent
per-spoke frequency in the 168 captures already taken is an offline question needing no hardware, and it separates "the signal
is absent" from "the estimator is the wrong one". It would be exploratory, not a registered candidate, and could support no
claim without confirmation on held-out spokes.

### Wideband look: the strike rings the rig, and the search band may be in the wrong place (2026-09-21)

Descriptive, outside the registration, selects nothing. Per block, the ring window (0.25-1.05 s) of every strike against that
same block's own no-fire controls, over 100-4000 Hz. A "consistent line" is at least 10 dB over that block's no-fire floor in at
least 80 % of its strikes.

**Each spoke's strikes produce 78 to 107 consistent lines.** The strongest are all above the search band: spoke 15 has lines at
1416, 1632, 1807, 1867, 2415 and 3325 Hz at +38 to +52 dB; spoke 0 at 634, 990, 1226, 1461, 1575 and 1615 Hz at +32 to +44 dB.
The 350-600 Hz band the DSP searches holds none of the strong content.

**Most of those lines are the rig, not the spoke.** Counting lines that appear within 6 Hz on at least 3 of a station's 4
exploration spokes: **65 shared lines at RIGHT and 46 at LEFT**. Different spokes, same lines. Only a few are unique to one
spoke, and inside 350-600 Hz they are: RIGHT sp17 none, sp5 354 and 388 Hz, sp3 342 and 556, sp15 468; LEFT sp10 352, 386 and
481, sp22 491, sp12 436, sp0 426 and 592.

**This retires the 430 Hz question.** 428 Hz is in the RIGHT shared list, so the line behind block 4's three clears is a
structural line common to RIGHT spokes, not spoke 15's own tone. The earlier note that it "appears only on spoke 15" was based
on the firmware's reported f1 and on two blocks; over all four RIGHT spokes and a wider band it is shared. No reading of those
three clears as a spoke measurement survives.

**A harmonic-sum estimate of the fundamental** (best f0 in 150-1200 Hz by the sum of its first four harmonics over the floor)
gives, per spoke: RIGHT 17 798.8 Hz, RIGHT 5 553.8, RIGHT 3 810.0, RIGHT 15 603.8, LEFT 10 582.5, LEFT 22 490.0, LEFT 12 652.5,
LEFT 0 635.0. **Five of the eight fall outside 350-600 Hz.** This is a weak estimator on a rig-dominated spectrum and is not a
tension or mode claim; what it does show is that nothing justifies assuming this wheel's spoke tones sit inside a band inherited
from a different wheel, a different transducer and a different excitation.

**What this explains.** The peak picker takes the lowest strong peak in 350-600 Hz. On these captures that is either the
band-edge floor or one of the 46-65 structural lines, and the spoke's own line, where it exists, is neither the lowest nor the
strongest. `AMBIGUOUS_PEAK` on a spectrum with dozens of competing structural lines is the correct behaviour of the estimator,
not a bug in it. The hammer blow that makes those lines is the excitation this campaign chose, and the research constants it
inherited came from a magnetic pickup that could not see the rig at all.

**Not established:** that any of the per-spoke unique lines is a fundamental, that a pluck would avoid the rig ring on this
mount, or any frequency-to-tension relation. Next test, operator-proposed: hand-pluck a spoke with a pencil and capture it on
the same chain, to compare the ring against these strikes. Diagnostic, not campaign data.

### Excitation probe: pulse sweep and hand plucks on spoke 0 (2026-09-21)

Exploratory, outside the registration, selects nothing and changes no constant. Bundles in `_explore/` (gitignored, no plan, no
trial numbers, no fixed n). LEFT/rig-1, spoke 0 (LEFT-leading, S0) under the LEFT plunger, build `2511fdf`, pillow hold. Tool
`tools/explore_pulse_sweep.py` (`4680ca4`..`b4da924`).

**Sweep, 6 shots at each of 9 widths, interleaved.** Impact = a click louder than -30 dBFS landing 0.10-0.32 s into the capture.
Ring over floor = level 250-550 ms after the impact, above 200 Hz, minus the LEFT no-fire median (-59.6 dBFS), impact shots only.
The board's own baseline DSP verdict is alongside.

| pulse | impact | ring over floor | board clears | board median SNR |
|---|---|---|---|---|
| 15 ms | 0/6 | - | 0/6 | 2.7 dB |
| 20 ms | 0/6 | - | 0/6 | 6.5 |
| 25 ms | 0/6 | - | 0/6 | 5.8 |
| 30 ms | 0/6 | - | 0/6 | 5.6 |
| 35 ms | 3/6 | +13.1 dB | 1/6 | 1.6 |
| 40 ms | 5/6 | +15.8 | 0/6 | 1.5 |
| 50 ms | 4/6 | +10.4 | 0/6 | 1.5 |
| 60 ms | 6/6 | +10.3 | 0/6 | -0.8 |
| 85 ms | 6/6 | +7.4 | 0/6 | 4.5 |

Below 35 ms the plunger does not arrive; from 40 ms to 85 ms the ring over floor falls monotonically (+15.8 to +7.4 dB), which
fits a plunger resting on the spoke and damping it. **But the board's own SNR does not follow the ring**: it is 1.5 dB at 40 ms
and 2.7 dB at 15 ms, where nothing touched the spoke, and only 1 of 54 shots cleared. The extra ring is broadband rig ring, not
a spoke line. The over-floor measure, adopted for cross-excitation comparison on 2026-09-21 before this data, said a 40 ms strike
(+15.8 dB) sits inside the plucks' range (+13.0 to +19.9); the direct outcome says it is nowhere near. **That measure was the
wrong one and is withdrawn as a viability test**; it remains a within-solenoid width comparison only. The two 40 ms test shots
fired earlier this session produced no impact, while 5 of 6 in the sweep did; the first shots after idle may be weaker (not
tested).

**Hand plucks, same spoke and chain, same unchanged DSP.** 15 cued plucks (`EXPLORE-pluck-lead3`, 12 of 15 windows opened after
the pluck). The board cleared **5 of 15**: r1 (16.3 dB, f1 406.1 Hz), r2 (16.1, 406.3), r5 (23.9, 406.2), r9 (17.5, 406.2) and r14
(12.7, 397.6). Of the five loud-click plucks, 4 cleared; of the operator's four ear-picked plucks, 1 cleared. **Four clears agree
to within 0.2 Hz at 406.2 Hz**, with SNRs of 16 to 24 dB against the 12 dB gate. An earlier, spoke-untracked cued run showed
lines at 405-407 Hz in five captures. The same DSP clears 1 of 54 solenoid shots today and 4 of 168 in B3.2. Solenoid strikes
on spoke 0 in B3.2 showed a strong line at 426.2 Hz (23 of 24), not 406. **Hypothesis, untested:** a plunger in contact loads the
spoke and shifts what it rings at, so the strike and the free spoke do not ring at the same frequency.

**What is and is not established.** The chain, the mic and the unchanged DSP can read spoke 0 from a hand pluck: consistent,
above the gate, with no window or constant changed. The solenoid, as mounted, cannot, at any width it can reach. Not
established: that 406 Hz is the spoke's own tone rather than a rig line the pencil excites (one spoke; the operator has not
plucked a second one yet), any frequency-to-tension relation, or that any mechanism the solenoid could be changed to would
match a pluck.

### Dual-microphone exploration: hardware change and workbench (2026-09-27)

Exploratory and outside every registration: no plan, no trial numbers, no fixed n, no exclusion ledger. Nothing here
selects anything or changes a constant, and nothing from it is campaign evidence until a later registered run.

**Hardware change (operator).** One INMP441 per acoustic station, each seated near its own solenoid's strike point on an
adjustable mount. Wiring: both on the shared bus (SCK D2/GPIO5, WS D3/GPIO6, SD D4/GPIO7, 3V3, GND); the original mic, L/R to
GND, moves to the LEFT station (left slot); the new mic, L/R to 3V3, goes to the RIGHT station (right slot); one pull-down on
SD at the board. Placement is not assumed to fix the Phase B excitation finding above.

**Rig registry.** The provenance table lists mic position as a rig-registry field, but positions will change shot to shot
during exploration, so they are recorded **per capture** (`wb_phys_*` fields, and per epoch in the workbench session) and the
tokens stay `LEFT/rig-1` / `RIGHT/rig-1` until a configuration is chosen for anything formal. That choice bumps both tokens
with a dated registry block, as the append-only rule requires.

**Firmware seam.** Image `nano_esp32_fastdemo_mic_dualmic` (`TRUING_DUAL_MIC`, legal only with `TRUING_CAMPAIGN_DEBUG`):
both slots read every frame; each capture hands the DSP the struck station's own mic only, and keeps the other as
`/debug/capture_far.pcm`. The chain profile and digest are unchanged. Not yet run on target.

**Tool.** `tools/workbench.py` (manual: `tools/WORKBENCH.md`); sessions under `test/fixtures/acoustic/captures/_explore/wb_*`
(gitignored, this machine only).

**Observation from existing data, not investigated (recorded so it is not rediscovered).** The board's onset landed at
sample 0 -- the start of the capture, not the excitation -- in 60 of 168 B3.2 strikes, 44 of 56 B3.2 no-fire controls, 12 of
15 of the 09-21 cued hand plucks and 41 of 54 of the 09-21 pulse-sweep shots. With the onset at 0 the board's analysis
window is 40-540 ms, which for a strike spans pre-impact floor and the impact itself (impact 0.10-0.32 s). Of the four hand
plucks that cleared at 406.2 Hz, two had the onset at 0 (r1, r2) and two a real one (r5 at sample 3120, r9 at 15360), so
clearing does not depend on it either way and this does not by itself explain the strike result. What it means for the onset
settings is an open question; the workbench draws the board window beside its own ring window on every shot so it stays
visible.

### Board change: the Nano died, both stations move to a spare DevKit (2026-09-27)

While wiring the second INMP441 above, the Nano ESP32 was damaged by a wiring mistake and is out of rotation. The operator
has a spare ESP32-S3-DevKitC-1 in stock; **the whole acoustic front end -- both mics and both solenoid stations -- moves
there**, not just the mics. This is a board change, not a design change: nothing about the DSP, the chain profile, the
excitation profile, or the workbench's method is different.

**What carries over unchanged.** The chain profile, its digest, the DSP, `tools/workbench.py`, `tools/wb_analysis.py`, and
every existing reference capture (`ref preset spoke0-pluck` etc.) -- those are frequency/shape comparisons already labelled
"not comparable" on absolute level, and stay valid regardless of which board takes new captures.

**What does not carry over.** The rig registry (`LEFT/rig-1` / `RIGHT/rig-1`) described the Nano's mounting hardware --
solenoid tower, standoff fitting, strike point. None of that exists on the new board yet (no solenoids are wired there).
**The tokens are not bumped now**, because there is nothing to register yet; whoever wires the solenoids records a fresh
`LEFT/rig-2` / `RIGHT/rig-2` block with real values at that time, per the registry's own append-only rule -- reusing
`rig-1`'s numbers for a different physical mount would misrepresent the rig. The B2-M3 pulse brackets (LEFT [40,85] ms,
RIGHT [60,95] ms) are likewise Nano-rig-specific (mount, standoff, spring) and unverified here;
`tools/workbench.py` keeps them only as a labelled starting point pending a fresh sweep.

**Firmware.** `board_s3_devkit.h` already reserved mic pins (BCLK 4, WS 5, DIN 6) and solenoid pins (LEFT 15, RIGHT 21) with
no conflicts against anything else on that board -- including the roller campaign's stepper pins (STEP 16, DIR 17, EN 18,
UART 8/9, DIAG 10), which this does not touch. Added: `BOARD_I2S_MIC_INPUT_LEFT_STATION`/`_RIGHT_STATION` (mirrors the Nano
header); `env:s3_devkit_fastdemo_mic_campaign` and `env:s3_devkit_fastdemo_mic_dualmic` in `platformio.ini` (same flags as
their Nano counterparts, which stay in the matrix as a record of what ran there). No changes to `audio_i2s.c`,
`acoustic_real.c`, `pluck_gpio.c`, `orch_demo.c` or `bringup_acoustic.c` -- all of it already read pins from the board
profile rather than assuming which board.

**Solenoids stay `PRESENT=0`.** Wiring two solenoids on a new board without redoing the Nano's B0 electrical validation
(channel isolation, no cross-talk, V_DS check) would be exactly the kind of unverified assumption that damaged the Nano.
`tools/bench/solenoid_smoke` gained an `s3_bench_solenoid` env (GPIO 15/21, `-DLEFT_GPIO=15 -DRIGHT_GPIO=21` overriding the
Nano defaults) and `solenoid_ctl.py` gained a `--port` flag, so the same manual bring-up procedure documented in this file's
B0 section can be repeated on the new board before `BOARD_PLUCK_ACTUATOR_*_PRESENT` is ever flipped. `MEASURE_ONCE`'s
no-fire path needs no actuator at all (`acoustic_real.c`: `if (ov_no_fire) { pulse_ms = 0.0f; }` never checks one), so
`tools/workbench.py`'s `ctrl` and `pluck` work today with zero solenoids wired; `fire`/`air` correctly reject
`EXCITATION_UNAVAILABLE` until B0 passes and `PRESENT` is flipped per station.

**Two physical DevKitC-1 boards, same type.** The roller campaign's board is fixed at `upload_port = COM4` in
`[env:s3_devkit]`. This one is not pinned in `platformio.ini` for exactly this reason -- `pio device list` and each board's
own `GET /id` (base MAC, AP SSID) are how they are told apart, never port or env name alone. Nothing here touches the
roller worktree (`feat/roller-nav`, `truing_roller`) or its schedule.

**Not yet done.** Nothing has been wired to the new board. This entry records the plan and the code/doc changes that make
it possible; the mic wiring, the tap test, and (separately, later) the solenoid B0 check are still ahead.

### DevKit bring-up: mics wired and alive, solenoids fired without B0, roller alongside (2026-09-29)

Exploratory and outside every registration: no plan, no trial numbers, no fixed n. Nothing here selects anything, changes a
constant, or is campaign evidence. It updates what the "Not yet done" paragraph above says is still ahead. Full pinout and
the roller/driver results are in `docs/SUBSYSTEM_BENCH.md`.

**Boards.** Two DevKitC-1 boards are in use, told apart by serial (`pio device list`), never by port: `5C4C213858`
(AP `truing-xxxxxx`) carried both mics, both solenoids and the roller on one breadboard; `5A84006975` (AP `truing-xxxxxx`)
carries the mics and a roller perfboard with a socketed TMC2209, and no solenoids. Image `s3_devkit_lab`
(`fastdemo+inmp441x2+campaign+drive`), pins as `board_s3_devkit.h`: mics 4/5/6, solenoids 15/21, roller 16/17/18, driver
UART 8/9, DIAG 10. The Nano is not used.

**Done: mic wiring and the tap test.** Both INMP441s on the shared bus (LEFT L/R to GND, RIGHT L/R to 3V3) read ALIVE
in the workbench heartbeat: LEFT about -40 dBFS, RIGHT about -32 dBFS in a quiet room, and a tap on either mic shows
`<< TAP`. Listening recordings were judged by the operator by ear (no analysis). A spare LEFT module measured 0 ohms across
its supply and took the shared rail down; it was removed and a second spare works. The cause is not known.

**Solenoids fired WITHOUT B0.** Both stations were fired from test images built with
`-DBOARD_PLUCK_ACTUATOR_LEFT_PRESENT=1 -DBOARD_PLUCK_ACTUATOR_RIGHT_PRESENT=1` (the flags default to 0 and the checked-in
header is unchanged), through the workbench burst with duty caps. Each shot was announced and the operator heard and saw
both fire, singly, alternating and together, and with the roller running. That is a bench observation that the outputs
switch. It is **not** the B0 validation the Board change entry requires before `PRESENT` is flipped: channel isolation
and cross-talk were not checked on this board, **V_DS is unmeasured** (it needs a meter), the `rig-2` registry blocks
do not exist, and the B2-M3 pulse brackets (LEFT [40,85] ms, RIGHT [60,95] ms) are Nano numbers not verified here. The
`PRESENT` flags stay 0 in the tree.

**Alongside.** The roller ran at the same time as a 40-shot burst and both mics; both stayed ALIVE and the board did not
reset. ALIVE means powered and clocked, not that the noise floor is unchanged: the mic noise floor against the motor off,
enabled-idle and moving has **not** been measured. The lab image records whether the drive is enabled with each capture
so that comparison is possible.

**Still ahead, in order.** Choose the board that carries the acoustic stations; B0 for both solenoids on the DevKit
(`s3_bench_solenoid`; isolation, cross-talk, V_DS), then flip `PRESENT`; `LEFT/rig-2` and `RIGHT/rig-2` registry blocks
when the solenoids and mics are mounted together; a fresh pulse-width sweep on the new mounts; then mic profiling.
The B3.2 excitation finding is unchanged: nothing here changes the conclusion that the solenoid, as mounted on the Nano,
did not excite a readable spoke.

### Frozen-rig characterization: pre-stated metrics (2026-09-30, before any data)

Exploratory and outside every registration: one spoke, no held-out spoke set, so nothing here is campaign
evidence or a selection under B3.2. Written and committed before the first shot, so the metrics cannot be
shaped by the data. Tool: `tools/char_loop.py`; data under `_explore/char_20260930/` (gitignored).

**Rig.** Mechanics frozen for the whole run: spoke 0 (S0, LEFT-leading) clamped at the LEFT station,
both mics seated as they are (rig-2: DevKit, a mic beside each strike point). The `LEFT/rig-2` values
(mic seating, standoff) are recorded from the operator's description when the rig is declared frozen.
Only the LEFT solenoid is enabled (`-DBOARD_PLUCK_ACTUATOR_LEFT_PRESENT=1`, a test build; B0 is still not
done on this board, and the operator accepted that for this run). Roller disabled.

**What varies.** Pulse width (per-shot override), recording choices (local or far mic; capture length
only with a reflash and a new batch), and analysis (chain-profile fields replayed through the firmware's
own DSP on the host; two-mic methods and code-level DSP changes as exploratory host analyses first).

**Reference frequency.** Five hand plucks of spoke 0 chosen by the operator from the 09-21 cued run
(`EXPLORE-pluck-lead3_sp0_r0/r1/r2/r7/r13`). The unchanged DSP clears 2 of 5 at 406.1 and 406.3 Hz:
reference f1 406.2 Hz. Only its frequency transfers (different board, mic and position), and only if
spoke 0's tension is unchanged since 09-21.

**Stage 0 go/no-go** (line finding, not the board's verdict; local and far mic): GO when, at some pulse
width, at least 50% of strikes have a strong in-band peak within +-5 Hz of 406.2 Hz and at most 25% of
the no-fire controls do. Otherwise stop and report: the excitation, not the analysis, is still the limit.

**Metrics, per candidate** (a candidate = a chain profile x a mic x a pulse width):

| metric | role |
|---|---|
| clears on the no-fire controls (at least one control required) | hard gate: 0 |
| strike f1 median within 2 Hz of a coherent line of the no-fire set | hard gate: must not be |
| strike clear rate on spoke 0 at that width | primary |
| clears within +-2 Hz of their own median f1, and the spread | primary |
| strike median f1 minus the pluck reference, under the same profile | reported, never tuned on |
| local/far level ratio at the f1 peak | reported; should favour the struck station |

**Discipline.** Every analysis variant is run as an answer to a written hypothesis with a prediction,
logged before the result exists (`char_loop.py sweep` refuses otherwise). No variant may lower
`measurement_min_snr_db` below 12 dB or narrow the f1 band (both refused by the tool). Selection only on
the exploration batch; the number of variants tried is reported with any winner.

**Confirmation.** The chosen configuration is flashed (a chain-profile change is halt-and-ask: the
operator approves it first) and a fresh batch runs with the board's own live verdicts: pass is at least
9 of 10 strikes clearing with f1 within +-2 Hz of their median, 0 of 10 no-fire controls clearing, and
the f1 not a no-fire line. One bounded return is allowed (one more exploration pass, one fresh batch),
then the run stops. Last: the operator gives spoke 0 a quarter turn and a short batch must show the f1
move (up when tightened); a reading that does not move is the rig, whatever else passed.

### Frozen-rig characterization, Amendment 1 (2026-10-01, after Stage 0 data, disclosed)

**What changed.** The pre-stated Stage 0 rule counted any strong in-band peak within +-5 Hz of 406.2 Hz. It omitted the
SNR floor that B3.0's own definition of a line carries (a peak counts as a line only at or above the chain's
`measurement_min_snr_db`, 12 dB). The omission made the rule degenerate: a no-fire control here has about 128 strong
peaks (the board rejects them as AMBIGUOUS_PEAK), so any 10 Hz window catches one by chance. As written, the rule gave
NO-GO because 5 of 8 controls "had" a peak near 406 Hz, at a median SNR of 0.1 dB. The amended rule counts only peaks at
or above 12 dB SNR. This was decided after seeing the data, which is why it is recorded here rather than folded in.

**Stage 0 under each rule** (`S0`, 26 shots: 40/60/85 ms x 6 strikes + 8 no-fire, seed 930, LEFT/rig-2):
as pre-stated NO-GO (local controls 62% "hit"); amended GO (local mic: strikes with a >= 12 dB line within +-5 Hz of
406.2 Hz: 40 ms 4/6, 60 ms 3/6, 85 ms 0/6; controls 0/8). Far mic: 0 at every width.

**Also observed, not yet interpreted.** The board itself cleared 13 of 18 strikes and 0 of 8 controls, every clear at
360.5-363.2 Hz (40 ms: 362.6-363.0). On the local mic the strikes carry three lines at >= 12 dB that no control
reaches: about 362 Hz (15/18), 426 Hz (12/18; B3.2's spoke-0 strike line) and 406 Hz (7/18; the pluck reference). All
three are weak on the far mic, so they are local to the LEFT station, but that does not say whether they are the
spoke or the station's own structure. The firmware picks the lowest strong peak in the band, so it reports 362 Hz.
Which of the three is spoke 0 is the next question; the tension nudge is the deciding test.

**Rig-2 record (LEFT).** Board 5A84006975, LEFT solenoid on GPIO 15, both mics as seated by the operator on
2026-10-01 (seating and standoff not yet described; to be filled in when the operator is back). Image: `s3_devkit_lab`
at 77500b5 with `-DBOARD_PLUCK_ACTUATOR_LEFT_PRESENT=1` (the build reports -dirty only for uncommitted host-tool edits
to `tools/workbench.py` from another session; the firmware sources are committed).

### Frozen-rig characterization: overnight results (2026-10-01/02, exploratory)

Full iteration log, hypotheses written before each result: `_explore/char_20260930/LOG.md` (gitignored, this
machine). Batches S0 (26 shots), E1 (76), E2 (40) with spoke 0 at LEFT as first seated; R1 (32), R2 (30) after
the operator refitted the roller (spring-loaded mount) and re-seated spoke 0. Board 5A84006975.

- **The board's reading is repeatable and clean, and probably not spoke 0.** With the firmware's rule (lowest
  strong line in 350-600 Hz) the LEFT strikes clear at 30-50 ms in 7-8 of 8, at 362.6-362.7 Hz, with 0 of 46
  no-fire controls clearing across S0/E1/E2. But the strike rings ~90 recurring lines (300-3000 Hz), and other
  generic rules (strongest line, most local line) give equally clean, equally repeatable readings at 376.7 or
  455.5 Hz. Repeatability and clean controls cannot tell spoke from rig here (A3, A4).
- **Spoke 0's own family, when excited, is 427.3 / 904.6 / 1497.5 Hz.** It is the only prominent family the
  plunger's dwell damps (present at 30-40 ms, gone at 85-100 ms; replicated on a fresh batch, E2), with the
  even partial (904.6) undamped as a mid-span strike predicts, ratios 2.117 and 3.505 -- the same three-line
  family and ratios as the operator's 09-21 hand plucks of spoke 0 on the old rig (426.4 / 897.1 / 1500.6).
  Post-hoc in part, labelled as such in the log.
- **A rig change confirmed the split.** After the re-seat (R1) every remaining line is an E2 line scaled by
  0.9962, and exactly the spoke-0 family (plus 422/434) vanished; shorter pulses did not bring it back (R2).
- **Not done (needs the operator):** re-seat spoke 0 at the strike point and the tension nudge, whose
  predictions are pre-registered in the log. A per-station rig-line mask is the proposed fix if the nudge
  confirms; nothing was changed in firmware's analysis for it.

None of this is campaign evidence; the pipeline-demonstration build (`docs/DEMO_BUILD.md`) makes no use of it.
