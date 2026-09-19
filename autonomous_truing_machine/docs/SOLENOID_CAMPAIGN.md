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
  a note for whichever tool writes the manifest (`tools/campaign_runner.py`, in progress
  elsewhere) rather than something this doc enforces on its own.
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

- `tools/review_packet.py` (untracked, another agent's) still lists no-fire and strike captures
  identically — it does not show `fired` or `pulse_ms`. Needs fixing before any blind review packet
  is built from B3.0 data.
- The override's ceiling is the driver's existing 1000 ms stuck-actuator bound, not a thermal or
  dwell limit; the B2 brackets are enforced only by the runner. Whether the debug channel should
  carry a tighter cap is an operator decision.

**To run B3.0 (physical):** connect the mic; I flash `nano_esp32_fastdemo_mic_campaign` and confirm
it boots; position the wheel at a gap once per station for the air shots; keep the room quiet
through the ~5 minutes of automated capture.
