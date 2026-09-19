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

**Not done, and why.** Gating on the air-shot lines (a strike clear near one is not a consistent clear) was
rejected: six shots at one width and one microphone position are a thin basis, and it could discard a real
spoke clear. A hardware change (rubber on the plunger stops) was deferred: it would be a new rig_id with B2's
standoff and pulse bracket re-measured, and it is not needed to continue on `rig-1`. If a station misses the
exploration gate, a stop-damping change is a candidate for the plan's one bounded mechanical lever, by a small
amendment at that point.
