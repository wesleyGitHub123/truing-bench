# Roller wheel drive — bench campaign

The evidence log for the roller prototype: NEMA 17 + TMC2209 + direct-drive polyurethane roller
on the DevKit, on a **separate wheel stand**. Every mechanical or control number the firmware
uses comes from an entry here, with its date and conditions. Nothing is typed into
`src/roller_calibration.h` that this file does not show being measured (SPEC §10A.4, §10A.10).

Separate from the acoustic campaign on purpose: the DevKit (COM4), not the Nano (COM5); its own
wheel; its own branch (`feat/roller-nav`, worktree `%USERPROFILE%\truing_roller`). If the stand
shares a room with the acoustic rig, the stepper does not run during acoustic captures.

**Tools.** `tools/bench/roller_smoke/` (the sketch; it compiles the real
`src/wheel_drive_tmc2209.c`) and `tools/bench/roller_ctl.py` (keys in, board replies and your
`n` notes out, into `%TEMP%\roller_ctl.log`). Build and flash:

    pio run -d "$env:USERPROFILE\truing_roller\autonomous_truing_machine\tools\bench\roller_smoke" -e s3_bench_roller -t upload --upload-port COM4
    & "$env:USERPROFILE\.platformio\penv\Scripts\python.exe" "$env:USERPROFILE\truing_roller\autonomous_truing_machine\tools\bench\roller_ctl.py"

`?` lists the keys. Nothing moves on boot; `e` energises the coils; **`d` is the panic key**
(coils off at once); any key stops a run or a repeat test with a controlled deceleration.

## Hardware registry — fill in before R1

| item | value | source |
|---|---|---|
| motor make / model | | nameplate |
| motor rated current (A/phase), step angle | | nameplate / datasheet |
| TMC2209 breakout make / version | | board silkscreen |
| its VREF → current formula | | the breakout's own documentation |
| VREF set to (V) → current (A) | | multimeter, R0 |
| MS1 / MS2 jumpers (→ UART node, default microsteps) | | the breakout |
| motor supply VM (V) and source | | |
| roller outer diameter (mm), durometer if known | | calipers / supplier |
| contact surface on the wheel (tire or brake track) | | |
| tire pressure (if on the tire) | | gauge |
| rim / wheel (BSD, spoke count), rim radius at contact (mm) | | tape |
| roller preload arrangement | | description |

## R0 — wiring and power (no motor power until the checklist is done)

Pins are `board_s3_devkit.h`'s `BOARD_WHEEL_DRIVE_*`; nothing else may use them.

| TMC2209 | DevKit | note |
|---|---|---|
| STEP | GPIO16 | |
| DIR | GPIO17 | |
| EN | GPIO18 | active low; **10 kΩ pull-up to 3.3 V** so the driver stays off through boot and reset |
| PDN_UART | GPIO8 via **10 kΩ**, joined with GPIO9 at the module's RX pad | single-wire UART; the board hears its own echo. 1 kΩ did not answer on this module; see docs/SUBSYSTEM_BENCH.md |
| DIAG | GPIO10 | the firmware pulls it down, so unconnected reads "no fault" |
| VIO | 3.3 V | |
| GND | GND | **common** with VM's ground |
| VM | 12–24 V supply | **≥ 100 µF** electrolytic across VM/GND at the driver |

- [ ] **Never connect or disconnect the motor while VM is live.** It destroys the driver.
- [ ] Current limit set with the VREF pot, by the breakout's formula, to **~60 % of the rated
      current** for the first spin. Record VREF above.
- [ ] VM kill switch within reach. Roller and wheel: fingers clear of the contact nip.
- [ ] Nothing wired to GPIO 16/17/18/8/9/10 other than the above.

## R1 — driver alone, motor unloaded (no roller on the shaft or roller off the wheel)

1. Flash, start `roller_ctl.py`. Boot line: `TMC2209 on UART1 node 0: GCONF … CHOPCONF …` means the
   UART works; `did not answer` means check the 1 kΩ / PDN wiring, or `a` to scan nodes 0–3.
2. `u` — **IOIN version must read 0x21**. Record GCONF, CHOPCONF (MRES), DRV_STATUS, GSTAT.
3. `s` with the driver disabled, then `e`, then `s` — record DIAG in both states (the firmware only
   trusts DIAG while enabled; this is where that gets checked). Watch the shaft flag as `e`
   energises the coils: it may snap to the nearest full step. Confirming the spoke-0 reference in
   the firmware image does exactly this `e`, so record the size of any jump.
4. Put a tape flag on the motor shaft. `z`, `j` → type `200 × microsteps` (e.g. 1600 at 8), Enter,
   `f`. The flag must make **exactly one turn**. That confirms the microstep setting the part is
   really in. If not, `m` to set it explicitly over the UART and repeat.
5. `f` / `b`: record which way the shaft turns for `+` (viewed from the shaft end).
6. Heat: `F`, run 5 minutes, stop, touch-test the motor and driver. Record.

**Record:** microsteps (verified how), DIR sense, DIAG behaviour, register values, temperature.

## R2 — roller on the shaft, free air

1. Start with the cautious knobs (800 steps/s, 1600 steps/s²). `F`, stop, repeat while raising
   cruise with `+` until the motor misses or stalls; back off. Then acceleration with `]`.
2. Listen for resonance bands. StealthChop (the default) is quiet; note any speed that buzzes.
3. Stop behaviour: `F`, then a key, then watch for overshoot or coast after the last pulse.

**Record:** highest cruise and acceleration with no missed step, stall onset, and the values you
propose for the wheel test (not the edge — a margin below it).

## R3 — on the wheel: the calibration

Mark a tape flag on the rim and a fixed pointer on the stand. The count is only as good as the
alignment, so **always finish an alignment moving in the same direction as the run**. On an
overshoot, back off past the pointer and approach again.

1. Flag at the pointer. `z`.
2. `F` (run +). As the flag comes round toward the pointer, press a key to stop short of it.
3. Fine-jog in the **same direction**: `4`/`3`/`2`/`1` then `f` until the flag sits on the pointer.
   `p` gives the steps for one wheel revolution. For better resolution go round 3 times before
   aligning, and divide by 3.
4. Three runs `+`, then three runs `−` (`B` and `b`).
5. Note which way the wheel turned for `+` steps, **viewed from Side A** (SPEC §6.4.1).

**Result → calibration.** `steps_per_wheel_rev` = the mean. Its **sign** is `+` when positive
steps turn the wheel counter-clockwise viewed from Side A, `−` otherwise. Record the spread,
the + versus − difference (backlash / creep), microsteps, and the contact conditions.

## R4 — repeatability, drift, faults

1. **Revolution drift.** `j` = round(steps per rev), `k` = 10, `r`. After each revolution, `n`
   and note the flag's offset from the pointer in mm. Then the same with `R`.
2. **Spoke-pitch moves.** `j` = round(steps_per_rev / spokes), `k` = spokes, `r`; offset after
   every 8th. The bench rounds the pitch to whole steps and so drifts by design; the firmware
   does not. Compare against the rounding: offset beyond it is slip.
3. **Session-shaped run.** The firmware's shortest-path moves swing both ways. Do `r` then `R`
   alternately a few times and check the flag comes home.
4. **Faults.** With a gloved hand, drag the wheel against a slow `F` until it slips or stalls.
   Note whether DIAG goes high (StallGuard is not configured, so it probably will not: an
   open-loop stall is **silent** lost steps, which is why navigation re-references). `F` then `d`
   (coils off mid-move). Cut VM mid-move and watch what the board reports.

**Gate G1** (decision D7): worst positioning error over one session's worth of moves
≤ **¼ spoke pitch** (2.8° on 32 spokes, which is rim radius × 0.049 rad, about 15 mm at a
311 mm radius). Fail: the demo shows the bench sketch, and the integrated image waits.

## The calibration record (`src/roller_calibration.h`)

| field | from |
|---|---|
| `steps_per_wheel_rev` (signed) | R3 mean and sign |
| `max_step_hz`, `accel_steps_s2` | R2 proposal, confirmed by R4 with no lost steps |
| `microsteps` | R1 (verified), and the setting R3 ran at |
| `evidence` | the R3/R4 entry below, e.g. `"ROLLER_BENCH R3 2026-09-28"` |

Any field missing and the firmware refuses to position (`CALIBRATION_MISSING`). The image sets
and verifies the microstep count over the UART at boot; without the UART it cannot, and says so.

---

## Entries

### 2026-09-24 — pulse engine on target, nothing wired (T6, unloaded)

`roller_smoke` flashed to the DevKit (COM4, MAC xx:xx:xx:xx:xx:xx). No driver was connected; the
UART probe duly reported `IOIN no reply`. STEP toggled an unconnected GPIO16.

| check | result |
|---|---|
| boot | driver disabled, pins 16/17/18/10, UART1 8/9, as the board profile declares |
| +1000 then −1000 steps (800 steps/s, 1600 steps/s²) | exact both ways, back to 0; 1.730 s each (ramp estimate 1.75 s) |
| 10-step (triangle) move | exact, 0.129 s |
| run, controlled stop ~2 s in | 1711 steps in 2.616 s — 0.5 s accel, cruise, ~200 steps of deceleration |
| move while disabled | refused |

Establishes: the committed driver's pulse count and ramp timing on the real chip. Does **not**
establish anything about the TMC2209, the motor, the roller or the wheel.

### 2026-09-28 — R1 on a new TMC2209 board: UART dead, STEP/DIR/EN alive, 8 microsteps

Fresh (unused) TMC2209 breakout, motor from a prior project. UART never answered on any of 4 node
addresses despite verified continuity on every wire (VIO, GND, both data lines with the 1 kΩ
resistor), MS1/MS2 tied to GND and power-cycled correctly, and both a jumpered and an unjumpered
RX/TX topology tried. **UART is unresolved and not usable on this board as wired.**

STEP/DIR/EN works: motor turns cleanly once a firmware fault-latch bug was fixed (`b38167a` --
`enable(true)` wasn't clearing a latched DIAG fault unless the driver had been explicitly
disabled first; an operator pressing enable again to recover never worked). Jog sizes 1-10 are
imperceptible by design (a fraction of one microstep's worth of full-step angle); 100+ is visibly
moving.

**Microsteps confirmed empirically (no UART, so this is the only way): 3200 steps landed on
exactly 1 full shaft revolution → 3200 / 200 full-steps-per-rev = 16 microsteps.** (Corrected
from an initial reading of 8/1600 steps.) This is the number to carry forward into
`roller_calibration.h`'s `microsteps` field, cited to this entry -- not UART-verified, since
UART doesn't work on this board.

DIAG behaviour was noisy/inconsistent across sessions (0 and 1 seen at various points, both with
and without VM connected) and not yet understood; not blocking since our fault-latch only trusts
DIAG while the driver is enabled, and a real fault did latch and correctly halt a move once.
Revisit if faults start happening during normal operation with nothing actually wrong. One more
DIAG-high-on-enable instance seen mid-session (2026-09-28 continued, handoff to a second
operator); cleared on a second `e` press with nothing else touched -- consistent with a transient
settling condition right after a reset, not a persistent fault. Position was 0 at that point
(board had reset since the earlier 3200-step measurement), supporting that read.

**DIR sign (R1) -- CONFIRMED: + (the `F`/`f` commands) turns the wheel CCW viewed from Side A**
(this wheel: a front wheel, rotor side = Side A per SPEC §6.4.1; operator confirmed watching from
the rotor side). This matches the spec's positive convention directly. **`steps_per_wheel_rev`
(R3) is recorded positive, no sign flip.** Two repeated +F runs (4s each) landed on identical
+3282 steps every time with no fault -- also a first repeatability data point, informal, not a
substitute for R4.

**Heat check (R1) -- PASS.** 5 continuous minutes at 3818 steps/s (higher than the original 800
steps/s default -- picked up somewhere during earlier informal speed exploration; if anything
this is a more demanding test than R1's baseline asks for). Motor and driver both "decently
warm" afterward by touch, nothing alarming. Confirmed stopped afterward with no fault
(1,611,537 total steps this session, 46 moves, 0 refused).

**R1 is now fully closed out:** motor/driver alive and repeatable, 16 microsteps confirmed, DIR
sign confirmed positive, heat check passed. UART remains unresolved and deferred (not required
for positioning; see the "let's continue without the UART fix" discussion). Not yet done:
EN-polarity/DIAG-state table (minor, can fold into R2 notes).

### R2 -- speed/accel envelope (same session, continued)

Motor audibly whines above ~5000 steps/s -- a real signal (approaching torque ceiling or a
resonance band), not chased further since we don't need top speed, just something that reliably
finishes a move inside the D4 deadline. Settled on backing off with real margin rather than
hugging the whine threshold.

**Candidate: 3818 steps/s cruise, 1600 steps/s2 accel.** Confirmed with an 8000-step jog (~2.5
shaft revs, enough to hold cruise for a real stretch, not just touch it): first attempt faulted
at 985 steps (still in the acceleration ramp, well before reaching cruise) -- cleared and
retried identically, and the retry completed the full 8000 steps exactly, no fault, cruise held
clean throughout. Treating the first fault as another instance of the same intermittent
transient DIAG behavior already noted above, not a real limitation of this speed/accel pair --
but this is inference from one retry, not proof; R4 is where repeatability/fault-rate actually
gets characterized properly.

These are the values proposed for `roller_calibration.h`'s `max_step_hz` / `accel_steps_s2`,
pending R4 confirming no lost steps over a full repeated-move campaign.

### 2026-09-29 -- R3/R4 paused, motor detached for a mount redesign

This wheel's radial runout leaves the roller out of contact with the rim over part of every
revolution (dead spots) -- a real loss of drive, not measurement noise, and no amount of
averaging fixes it. R3's step-per-revolution count would be corrupted by exactly this. The fix is
mechanical: the roller mount needs compliance (spring-loaded/floating) to follow the rim's radial
variation, being designed with a teammate. R3, R4 and Gate G1 are paused until that exists (or
a rounder substitute wheel is found to calibrate against instead).

The motor is now detached from the driver/breadboard for that redesign, so the planned hardware
check of `navigation_roller` against the real TMC2209 driver (confirming `CALIBRATION_MISSING` is
correctly refused end-to-end, through the actual orchestrator image rather than the bench sketch)
did not happen this session -- it needs the motor wired to safely enable the drive. Still open;
do it once the motor is back, before anything else touches this seam.

R0/R1 remain valid (they characterize the driver/motor/microstep setting, not the wheel). R2's
candidate speed/accel values remain valid for the same reason. Nothing about the architecture is
in question -- see the plan file's 2026-09-29 entry for the distinction drawn between the
software being validated and the mechanism being unproven.

## 2026-10-01 -- spring-loaded roller mount; step counts are not a position (operator's tests)

**Mechanics, as the operator described it (not reverse-engineered here):** the NEMA 17's bracket is now
spring-loaded (a simple dynamic spring-loaded 3D-printed plane), so the whole stepper mount adjusts to the
deviations of the wheel's shape. It removed essentially all the dead spots where the roller lost contact
with the wheel and stopped turning it.

**Finding:** with dead spots gone, the operator ran a simple back-and-forth repeatability test -- `roll N`
then `roll -N`, to see whether the same spoke comes back to its solenoid station. It did not, consistently:
the same procedure over a few tests did not bring the spoke back. **So the step count sent to the driver is
not a reliable ground truth for navigation**, and roller calibration phases aimed at steps (R3/R4 above,
`steps_per_wheel_rev` in `src/roller_calibration.h`) are set aside rather than pursued.

**Direction:** a laser break-beam spoke sensor at the strike point (transmitter on one side, receiver on
the other) detects a spoke passing; the roller decelerates from a known speed so the wheel stops with that
spoke at the solenoid. Scaffold only until the module is delivered: `truing_hal/spoke_sensor_if.h`,
reserved flag `TRUING_SPOKE_SENSOR`, and `docs/DEMO_BUILD.md`. Meanwhile the demonstration build jogs the
roller manually and the operator confirms each spoke by eye.
