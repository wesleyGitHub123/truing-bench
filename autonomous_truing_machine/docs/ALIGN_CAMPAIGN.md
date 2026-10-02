# Mini campaign: roller / laser / pluck alignment

**Status (2026-10-02): built and host-tested, never run on the machine.** No result below is a measurement
yet. The laser receiver, its divider and its polarity are as recorded in `docs/LASER_BENCH.md`, but the
receiver has not been wired to the board running this code.

The problem this campaign attacks:

    HOW DO WE RELIABLY GO FROM "THE LASER DETECTED THIS SPOKE"
    TO "THAT SAME SPOKE IS NOW ALIGNED WITH THE SOLENOID STATION"?

It does not assume an answer. It runs competing roller behaviours on the real machine, classifies each
trial from the laser's own evidence, and decides per spoke class which behaviour (if any) is repeatable
enough to adopt.

## Where things run

- **On the ESP32, in real time:**
  - Count beam breaks.
  - Request the drive's existing controlled stop on the Nth break, from the beam interrupt.
  - After a settle, run the correction strategy: a slow reverse, a laser re-find with its own controlled
    stop, a centering trim.
  - Track which spoke is in the beam.
  - Classify the run.

  The code is `lib/truing_hal/src/beam_align.c`, which is pure and host-tested. `src/laser_bench.c` feeds
  it from the GPIO 7 interrupt and a 2 ms task. It runs on the lab image (`s3_devkit_lab`) and is served
  as `GET /debug/align.json`.
- **On the host (`tools/workbench.py`, `align ...`):**
  - Configure a run and start it.
  - Follow the run live.
  - Pluck and capture through the existing MEASURE_ONCE path.
  - Log, decide the next bounded test, and report.

  The host never decides when to stop the roller.

**Why the lab image and not `s3_devkit_demo`.** The demo image (baseline `de40940`) is the audience image.
It excludes the bench debug channel by design (`build_mode.h`), and its roller belongs to the manual jog.
The campaign needs:
- the bench drive;
- the debug channel, for MEASURE_ONCE;
- the laser input.

The lab image carries all three, built from the same tree as the demo baseline. The controller lives in
`lib/truing_hal`, beside `navigation_roller`, so adopting a winning policy is wiring, not rewriting (see
"Adoption" below).

## How the board knows which spoke is in the beam

**Zone accounting.** At the trigger, the target spoke is in the beam: zone 0. Every beam edge while the
drive moves adds one to the zone when the motion is along the approach direction, and subtracts one when
it is against it.
- An even zone means a spoke is in the beam, zone/2 spokes ahead of the target.
- An odd zone means the beam is in a gap.

"The same spoke is back in the beam" is exactly zone 0 with the beam blocked. If the beam level ever
disagrees with the zone's parity, the run is `LASER_AMBIGUOUS` rather than a guess.

**Edges while the drive is still.** These come from settle spring-back, a pluck or a hand. They have no
known direction, so they are counted and accepted only if the parity still agrees.

**The REACQUIRE same-spoke check.** Coming back against the approach, the target re-enters the beam at the
edge where it left. If the re-found edge is more than half the SHORTEST measured spacing away, it is not the same
spoke (the median over-trusted a neighbour 700 steps off, on a wheel whose gaps run 1963/718/1963/3255). The check
needs at least two breaks on the approach to have a gap to measure, so a break-1 trial reports `unchecked`. The offset (`reacq_offset`) is recorded either way, which measures backlash and slip.

**What "aligned" means here.** The target spoke blocks the beam. It means aligned with the solenoid station
**only if the beam crosses the strike point**. The campaign records that as an explicit, operator-declared
assumption (`align set beam_at_station yes`). It never infers it.

## The four spoke classes

`spoke m mod 4` maps as follows. This is the shipped artifact's class map (model_prep
`conventions.py`, origin spoke 0 = NDS leading, SPEC 6.5).

| m mod 4 | class | station (acoustic parity rule) |
|---|---|---|
| 0 | NDS-leading | LEFT |
| 1 | DS-leading | RIGHT |
| 2 | NDS-trailing | LEFT |
| 3 | DS-trailing | RIGHT |

The **physical** spoke 0 must be the wheel's declared NDS-leading spoke. Which way spoke numbers run
relative to the roller's step direction (`index_dir`) is declared by the operator, not assumed. Repo
conventions (positive steps turn the wheel CCW viewed from Side A, spokes numbered CCW) imply −1, but the
bench wheel was observed on 2026-10-02: forward steps (`dir` +1) bring spoke 1 to the beam after spoke 0, so
`index_dir` = **+1** there. Check by watching which spoke arrives next; do not trust the derivation.

## Stages

**Stage 0: laser count reliability.** Command: `align count <v1> <v2> ...`, a modest speed sweep.
- Each run stops on break 32 when spoke 0 starts in the beam, or break 33 when it starts just before the
  beam.
- It records:
  - breaks against expected;
  - missed breaks (a spacing near twice the median);
  - extra breaks (a spacing under half);
  - glitches and elapsed time;
  - the break-to-break spacing and blocked width;
  - one revolution in steps, edge to edge (when started before the beam);
  - the return-to-start error relative to spoke 0's own edge;
  - the final beam state.

**PASS is about counting only.** It means the laser is probably reliable enough to count passing spokes.
It says nothing about positioning a spoke at a station, so the stop position is reported beside the
verdict and never folded into it.

**Stage 1: the stop alone, per class.** Command: `align trial <class> 3 STOP`, or the ladder below. The
controller stops on the break of the next spoke of that class (spoke counting from the declared spoke) and
evaluates where the stop left it.

**Stage 2: corrections, only where stage 1 failed.**
- `RETURN_TO_EDGE` is step-based. It returns by the measured overshoot to the trigger edge, plus half the
  measured blocked width if `center`.
- `REACQUIRE` is laser-guided. It reverses at the slow ramp until the laser re-finds the same spoke, then
  trims to centre.
- `REVERSE_FIXED` (a fixed reverse step count) is available for testing a specific hypothesis.

**Stage 3: repetition.** A policy is repeated 5 times on the class before it can be called a candidate.

### The adaptive ladder (`align campaign <class|all>`)

Hypothesize, run, classify, then choose the next bounded test. The rules are printed as they fire:
1. **STOP ×3.** All succeed: repeat STOP ×5 (stage 3).
2. **Otherwise, test whether the stop is predictable enough for a step-based correction.** That holds if
   every stop fired and the overshoot spread is under half the measured blocked width (a step-based return
   lands inside the beam every time only if the spread is that tight). Then run `RETURN_TO_EDGE` (centered)
   ×3, and ×5 if all succeed.
3. **Otherwise, or if rule 2 failed:** run `REACQUIRE` (centered) ×3, then ×5 if all succeed.
4. **Nothing passed:** report what failed. Nothing is called adoptable.

A series stops early on ABORTED, FAULT, LASER_MISSED or LASER_AMBIGUOUS. After those the board can no
longer vouch for which spoke is where, so the operator re-places a spoke and runs `align spoke <k>`.

## Trial outcomes

**The controller's results, from the board:**

| result | meaning |
|---|---|
| ALIGNED | zone 0, beam blocked: the triggering spoke is in the beam |
| OVERSHOOT | ended past the target (zone > 0) |
| UNDERSHOOT | ended short of the target (zone < 0) |
| LASER_MISSED | the travel bound ran out before the target break |
| LASER_AMBIGUOUS | the beam level contradicts the zone, or the same-spoke check failed |
| REVERSE_FAILED | a step-based correction ended off the target |
| REACQUISITION_FAILED | the slow reverse search ran out without re-finding the spoke |
| ABORTED | `align abort`, a manual `roll stop`, or motion the controller did not command |
| FAULT | coils released, driver fault, or a refused drive command |
| COUNT_DONE | stage 0 only: the target break was reached |

**The trial verdict, from the host.**
- An ALIGNED run becomes `ALIGNMENT_SUCCESS` only if the pluck fired, when it is enabled.
- A pluck that did not fire is `PLUCK_FAILED`.
- A capture with `capture_result` not OK, or a `REJECTED` status, is `ACOUSTIC_CAPTURE_FAILED`.
- Whether the strike moved the spoke (the beam changed during the pluck) is recorded as
  `pluck_moved_spoke`.

## What every trial records (`align_trials.csv`, and `align_run_<n>.json` with every edge)

**Measured by the firmware (machine timestamps and the step counter):**
- trigger step and µs time;
- stop step and time, and stop duration;
- overshoot;
- zone and beam after the stop, and the deceleration's edges;
- where the target left the beam;
- the run's own median spacing and blocked width;
- correction command, reacquisitions, the re-found edge, and the same-spoke check;
- trim;
- final step, zone and beam;
- edges while still;
- result and detail.

**Configured (campaign parameters you chose):**
- speed, acceleration, slow ramp;
- reverse steps, offset, center, settle;
- target break, direction;
- travel and search bounds.

**Derived on the host:**
- class, spoke index and station (from the declared spoke and `index_dir`);
- the final error from the middle of the measured blocked span;
- the pluck and capture outcome (board status, reason and f1; the shot is saved in the session like any
  workbench shot);
- the verdict.

**Not measurable yet, so recorded as unavailable:**
- the angle between the beam and the strike point;
- the receiver's response time;
- any wheel motion the beam does not see.

Step counts are pulses sent, never proof that the wheel followed.

## Running it (operator)

1. **Flash** the lab image built from this branch, with both solenoids enabled:
   `PLATFORMIO_BUILD_FLAGS="-DBOARD_PLUCK_ACTUATOR_LEFT_PRESENT=1 -DBOARD_PLUCK_ACTUATOR_RIGHT_PRESENT=1"`, then
   `python tools/flash.py s3_devkit_lab --port COMxx --build-dir <worktree>`.
2. **Start a session:** `python tools/workbench.py --session align1`.
3. **Check health:**
   - `laser`: `CLEAR` with the laser on the receiver.
   - `laser watch`: wave a card through the beam.
   - `roll on`.
4. **Set the parameters.** These are your campaign choices:
   - `align set max <steps>`: more than one wheel turn.
   - `align set settle 300`.
   - `align set slow <steps/s> <steps/s2>`: needed for every correction strategy.
5. **Run stage 0:** place spoke 0 in the beam, then `align count 300 600 900` (a modest sweep). While it
   runs, watch which spoke follows spoke 0, then `align set index_dir +1` or `-1`.
6. **Declare the reference and run the ladder:**
   - `align spoke 0` (if spoke 0 is in the beam).
   - `align set beam_at_station yes`, only if the beam crosses the strike point.
   - `roll speed <v> <a>`, then `align campaign all`.
7. **Review:** `align log` prints the stage-0 table, the per-class policy table and the verdicts.

Ctrl+C during a run aborts it with the drive's controlled stop. `roll stop`, `roll off` and driver faults
keep priority, and the controller classifies their effect (ABORTED / FAULT) instead of fighting them.

## Adoption

**What counts as a candidate.** A policy (strategy plus speed, acceleration and its correction parameters)
becomes a **candidate for one class** when its last 5 trials on that class are all ALIGNMENT_SUCCESS.
Before proposing adoption, also check these, all in `align log` and the CSV:
- the overshoot range and the final-error range (a policy that succeeds only at the edge of the beam is
  fragile);
- the acoustic verdicts;
- `pluck_moved_spoke`;
- the REACQUIRE re-find offsets (backlash and slip);
- whether the result holds at a second speed.

**Global versus class policies.**
- **Global policy:** the same candidate policy on all four classes.
- **Class-dependent policies:** a candidate on every class, but not the same one.
- **Neither:** some class lacks a candidate. The log says which.

**What adoption means architecturally.** It means wiring `beam_align` into navigation:
- a spoke-sensor implementation of `spoke_sensor_if.h` on GPIO 7;
- `navigation_roller` asking the controller for "the next break of spoke k" instead of a step count;
- the per-class policy table as data (like the machine profile), filled only from campaign evidence.

That is a separate, reviewed change. The reserved flag `TRUING_SPOKE_SENSOR` and the D-decisions in the
roller plan govern it. Nothing in this campaign changes navigation, and no policy value is compiled into
any image.

## Deliberately not assumed or hard-coded

- **Physical geometry and timing:**
  - the beam's position relative to either strike point;
  - `index_dir`;
  - the receiver's polarity beyond the operator's bench test;
  - the spoke spacing, blocked width and revolution in steps (measured by stage 0, kept with their source
    run);
  - any speed, acceleration, settle or correction value (all operator-set, recorded per trial).
- **How a correct trial is decided:**
  - that STOP is enough;
  - that one policy fits every class;
  - that the pluck leaves the spoke in place;
  - that alignment implies a usable capture.
- **Defaults kept:** the 5-trial candidate rule and the half-width / half-pitch decision rules. They are
  stated rules, derived from the measured geometry, not from tuning.

## Bench results, 2026-10-02 / 03 (operator-run, spoke by spoke)

The operator replaced the automatic ladder with a hand-run, per-spoke method: pick a spoke, define the roller
sequence, repeat until it positions repeatably, then record it and move on. Spoke 0 and spoke 1 are done.
Everything below is **measured on one bench wheel**; none of it is a calibration or compiled into any image.
"Hit" was judged by the operator (eye and ear), not by the DSP.

**Declared, not measured:** spoke 0 is the wheel's NDS-leading spoke (so spoke 1 is DS-leading, spoke 2
NDS-trailing); `index_dir` +1 (spoke 1 follows spoke 0 on forward steps, observed); the beam crosses the strike
points (`beam_at_station yes`), taken from the operator's hits. Stage 0 was done by the operator's method: one pass
per speed (1000, 2000, 3000 steps/s) stopping at break 32, the operator confirming the wheel came back each time;
each counted 32 of 32 breaks, 0 glitches.

**Geometry seen at the beam (measured):** blocked width ~65-67 steps; the break-to-break spacing repeats every 4
spokes (0→1 ~1963, 1→2 ~718, 2→3 ~1963, 3→4 ~3255 steps); one revolution ~63450 steps. The spacing is not uniform
because the beam sits where spokes cross.

| | spoke 0 (NDS-leading, LEFT) | spoke 1 (DS-leading, RIGHT) |
|---|---|---|
| strategy | `REACQUIRE` | `REACQUIRE_CLEAR` (new, firmware) |
| approach | 1000 steps/s, 1600 steps/s2 | 800 steps/s, 1600 steps/s2 |
| slow reverse | 200 steps/s, 1000 steps/s2 | 200 steps/s, 1000 steps/s2 |
| sequence | forward to the break, controlled stop (+314), slow reverse until the beam reads blocked again, stop | forward to the break, controlled stop (+201), slow reverse until blocked, keep going until clear, stop, then 20 more steps back (`off -20`) |
| strikes | 5, profile pulse (20 ms) | 5, 60 ms (the 20 ms profile missed some) |
| trials aligned | 11 of 11 (6 standalone, 5 chained) | 11 of 11 (3 + 3 standalone, 5 chained) |
| final vs span middle | 1-4 steps, beam blocked | -78..-83 steps, beam clear (zone -1) |
| re-found edge vs where the spoke cleared | -7..-10 steps | -4..-8 steps |

The two spokes were also run **in sequence** (spoke 0 recipe, then spoke 1 from where spoke 0 left the wheel, no
operator placement between) five times; all ten trials aligned. Between chains the roller backed up 3800 steps; the
first trigger step drifted ~5 steps per chain (11482, 11476, 11470, 11465), which the 65-step window absorbs.

**Spokes 2 and 3: a plain STOP, no reverse, tuned by the deceleration (operator's calls).** The operator wanted a
gentle stop that simply ends past the beam, with no re-find. The stop distance is the lever and it follows
v^2/(2a) of the drive ramp exactly (checked on every run: 200, 312, 321, 251, 209, 115 ...). The strike point is
past the spoke's beam window (65 steps wide), so the wheel ends in the gap after the spoke (zone +1); the tool's
`OVERSHOOT` label is expected, not a failure. The approach must start at least v^2/(2a) steps before the spoke so
the trigger lands at cruise (a 232-step run-up at 500 steps/s2 shortened one stop from 251 to 234).

**Correction (found at the end of the session): the step scale silently halved during the spoke 3 tuning.** From
partway through the spoke 3 runs, one revolution read 32,018 steps (was 63,450), the 4-spoke spacing pattern
997/360/987/1659 (was 1963/718/1963/3255) and the blocked width 33 (was 66): everything in steps was half. After a
reboot of the board it read 63,845 / 1987,718,1961,3331 / 66 again. The motor driver's microstep setting had
reverted (an exact factor of two: 16 -> 8 microsteps per step); the firmware writes it only at boot. Cause of the
reset not found (a supply dip on a shared rail at a solenoid burst is the unproven guess; it coincided with a beam
chatter episode). **Consequence: the "+115 at 1100 steps/s2" for spoke 3 was tuned in the halved scale** and is not
valid at the normal scale; at the normal scale the matching stop is about twice that (the +209 at 600 steps/s2 that
the operator called a success *before* the change). The laser-based spoke 0 and spoke 1 behaviors are unaffected.

| | spoke 2 (NDS-trailing, LEFT) | spoke 3 (DS-trailing, RIGHT) |
|---|---|---|
| strategy | `STOP` | `STOP` |
| approach (normal step scale) | 500 steps/s, 500 steps/s2 | 500 steps/s, 600 steps/s2 |
| stop after the trigger | +251 steps | +209 steps (called a success once, before the scale change; not re-confirmed since) |
| strikes | 5, 60 ms burst | 5, 60 ms burst |
| result | 5 of 5 by the operator (stop tuned from 32 to 251 steps; 32-126 were too early) | the +209 run was a success; the later tuning to +115 was done in the halved scale (see above) |

Spoke 3 tuning path in the HALVED scale only (500 steps/s approach; the operator's "late" calls there are the
same stop distance landing twice as far): ramp 600 (+209) late, 700 (+180) late, 800 (+157) late, 900 (+140) "bit
less late", 1000 (+126) "getting there", 1100 (+115) right, five times. An invalid trial (the beam chattered at the
start: 3392 edges, glitch counter +7943) was excluded.

**Caveats, so these are not over-read:**
- The board's tension analysis rejected most strikes (LOW_SNR, AMBIGUOUS_PEAK); it accepted 0-1 of 5 in capture
  mode. Success here means "the strike hit", the operator's criterion, not "a usable tension estimate".
- The strike path timed out and then refused with 409 on two trials (spoke 0 trial 6, a spoke 1 run). The
  positioning was fine on both; the cause is not found. The workbench now waits out a 409 (up to 10 s: the board
  refuses before it fires, so asking again cannot double-strike) and still does not retry a timeout, which may
  already have fired. Not yet exercised on hardware.
- Burst mode (`align set pluckmode burst`) counts impacts from the mic stream with no per-strike analysis. Its
  first block size (200 ms) undercounted strikes 100 ms apart (it reported 3 of 5 on spoke 0); it now uses 50 ms
  blocks, not yet re-run.
- Spoke 1's `-20` is the operator's choice ("double the stop's ~20 steps"), provisional.
- The strike moved the spoke on several 60 ms runs (the beam changed during the strikes); unresolved.
- The firmware same-spoke check used half the *median* spacing; it now uses the shortest measured gap (`pitch_min`,
  host-tested only).
- Spokes 4 onward repeat the class pattern (mod 4); they are not separately characterised.
- The firmware did not check the driver's microstep setting after boot, so a driver reset silently changed the step scale (all stop distances in steps). It now reads GCONF/CHOPCONF back at every `enable(true)` (`roll on`) and rewrites
  the setting if it had reverted, and `/debug/drive.json` reports `microsteps_want`, `mres_ok`, `mres_repairs`; the workbench
  refuses an `align` trial while `mres_ok` is false and the `roll` line flags it. Host-tested and built, NOT yet exercised on hardware.

## Full-revolution run, 2026-10-03: stopped at spoke 6 by a power event

The run drives spokes 0..31 in order, the class recipe by spoke mod 4, no return move, and stops when the counter reaches 32
(spoke 0 again). Each trial is its own workbench invocation, with checks that halt the run: the targeted spoke, ALIGNED for the
laser-based classes, blocked width 55-80, the stop distance after the trigger (314/201/251/209 +-20) and the trigger spacing
(1985/700/1963/3290 by phase).

- Spokes 0-5 ran clean: stops +314/+201/+251/+209/+314/+201, finals +2/-80/+219/+176/+3/-83 from the span middle, widths 64-66,
  trigger spacing 1975/692/1963/3267/1977 (the 3->4 gap of 3267 shows the pattern repeats on the second pass).
- Spoke 6 (break 2, STOP at 500/500): the beam chattered, 3338 edges within ~5 ms at steps 78933-78937 (the wheel did not move
  that far), the controller's zone count ran to +/-128, the board's edge log overflowed, the width read 2 and the run halted. The
  stop itself was +251 as expected. Then the board was found rebooted with `no bench drive (init failed)`: the operator reported
  the supply "died for a second" and unplugged in a panic. After re-powering, the board came back healthy (drive ok, laser CLEAR,
  glitches 0) with the step counter at 0.
- Order of events (chatter, power loss, unplug) is not established. This is the second chatter episode (the first, 3392 edges,
  coincided with the microstep reverting 16 -> 8); both were at a stop request. Treat a supply or ground dip as the leading
  suspect, unproven. After any chatter episode or power loss: re-place spoke 0 by hand, `align spoke 31`, and trust only a run
  whose widths read 64-66.

### Full revolution, second attempt: 32 of 32, nothing missed (2026-10-03)

After the power event the board was re-powered, spoke 0 re-placed by hand, `align spoke 31`, and the same run repeated. All 32
spokes ran (counter 32 = spoke 0 again, no return move); the operator called every spoke a hit by eye. No check tripped and
the laser glitch counter stayed 0.

- Stop after trigger was constant per class: +314 (0 mod 4), +201 (1), +251 (2), +209 (3). Finals from the span middle:
  class 0 -1..+7, class 1 -84..-78, class 2 +218..+219, class 3 +175..+177. Blocked width 64-68 throughout (the step scale did
  not change).
- Trigger spacing repeated the 4-spoke pattern on all eight passes (about 1952-2014, 662-714, 1952-1975, 3263-3320); spoke 0 to
  spoke 31 trigger was 60197 steps.
- After spoke 5's strikes the beam changed (spoke 6 already in the beam at its start). `reconcile_belief` moved the belief to
  spoke 5, the trial stopped on break 1, and the counter stayed right: the same nudge case that failed before the fix.
- Caveats: one revolution, one wheel, one power state; the open chatter/power-dip cause is unresolved. (The microstep
  read-back guard was added after this run; this run itself had no such protection and relied on the width check.)
