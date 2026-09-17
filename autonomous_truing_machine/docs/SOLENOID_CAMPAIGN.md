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
**Declared, not yet verified** — B2-M8 confirms S0..S3 against this table before it is relied on.
See `SOLENOID_CAMPAIGN_PLAN.md`'s Amendment 1 (C1–C2) for the full reasoning, and
`IMPLEMENTATION_NOTES.md`'s Known Limitations for what this does and does not settle for a real
physical truing session.

## Rig registry

| | LEFT | RIGHT |
|---|---|---|
| board GPIO | 9 (D6) | 10 (D7) |
| board macro | `BOARD_PLUCK_ACTUATOR_LEFT_GPIO` | `BOARD_PLUCK_ACTUATOR_RIGHT_GPIO` |
| PRESENT (current) | 0 | 0 |
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
| S0/S1 physical marking | S0 rule declared (C1); mark not yet placed | S1 = whatever the rule leaves at RIGHT; confirmed by exclusion once S0 is marked |

**Donor wheel:** front disc wheel, rotor on the RIGHT side. Expected asymmetric per SPEC §972,
unverified.

**Shared, declared:** one 12 V / 8 A wall supply, one bulk electrolytic capacitor (470–1000 µF,
≥25 V) across the 12 V rail at the point it splits to both solenoids, star ground (source
returns and the ESP32 return meet only at supply −), Nano powered from a power bank during
bench blocks.

**PRESENT is flipped to 1 only after that station passes B2** (per the plan's execution-order
rule). Both remain 0 until then, so every `*_mic` and `*_fastdemo_mic` image correctly refuses
sessions in the meantime (A10).

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
consistent with the load tasks doing something, not a sign of trouble. **LEFT's loaded
evidence already exists** (`ab02201`, B1a's original T6 run) — its quiet counterpart is not yet
captured, since B1a only needed to prove the fix holds under adversarial load, not establish a
quiet baseline. Cheap to add later if wanted for symmetry.

### B2-E4 — quiet boot, both channels (2026-09-17)

**Tool used:** `tools/bench/solenoid_smoke` (the bench sketch itself, not the T6 harness —
`pluck_timing_check` fires automatically by design and is the wrong tool for this check).

**Method:** three independent reset events (one from the flash's own `--after hard_reset`, two
deliberate `esptool --before default_reset --after hard_reset chip_id` no-op resets), operator
watching both plungers live through each one.

**Result:** zero movement on either channel across all three resets. Matches the sketch's own
design ("Fires nothing on boot; every activation is a keystroke") and confirms it holds in
practice, not just by source inspection.

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

## Next

**M2, M4, M5** per station (standoff, drift, station angle — all per class where relevant, per
Amendment 1 C3), then the two-station checks: M6 (wrong-actuator hazard), M7 (idle rattle), M8
(end-to-end attribution, which also verifies the declared class map in the Convention section
above). PRESENT flips to 1 per station only after it passes.
