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
| standoff fitting | 3D-printed, set to reach the farther class (leading) | 3D-printed, set to reach the farther class (trailing) |
| strike point (current) | eyeballed, roughly mid free span | eyeballed, roughly mid free span |
| per-class standoff order | trailing closer, leading farther | leading closer, trailing farther |
| station angle | not yet measured (B2-M5, per class) | not yet measured (B2-M5, per class) |
| pulse bracket | not yet measured (B2-M3, per class, run first) | not yet measured (B2-M3, per class, run first) |
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

## Next

**B2 (per-station bench)** is next, gated only on the S0 mark (operator; mounting is already
done). Order per Amendment 1 (C3–C4): **M3 first** — per-class pulse brackets, since a station's
bracket is the intersection of its two classes' brackets and an empty intersection triggers
**G-class** (a user decision on whether one pulse per station can serve both spoke classes).
Then M1, M2 and M4–M8 per station (strike geometry, standoff, drift, station angle — all
per class where relevant), then the two-station checks (wrong-actuator hazard, idle rattle,
end-to-end attribution — M8 also verifies the declared class map above). PRESENT flips to 1
per station only after it passes.
