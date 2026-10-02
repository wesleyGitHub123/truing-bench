# Laser break-beam bench: laser-stop characterization

**Status (2026-10-02): built and host-tested, not yet run on the board.** No trial has run. One fact is
tested on the bench: the receiver is **active low**, so its OUT goes low while the laser hits it (the
operator's test; the seller's listing says the opposite). So a clear beam reads 0 and a blocked beam
reads 1, and a beam break is a low-to-high edge. Everything else about the modules comes from the
listing and is unmeasured.

The goal is narrow. The roller moves slowly, and the receiver sees the beam break. The firmware then
asks for the drive's existing controlled stop, and the tooling records where the step counter was at the
break, where it ended, and whether the beam was still blocked there. Repeating that at different speeds
and decelerations finds the region where a stop lands inside the beam. This is **not** the
wheel-indexing algorithm. It does not tell you where a spoke sits relative to the plunger, and nothing
here becomes navigation (`TRUING_SPOKE_SENSOR` stays reserved; `docs/DEMO_BUILD.md`, "Automatic mode").

## The modules (seller's listing, e-Gizmo)

| | transmitter (KY-008) | receiver ("Laser Sensor and Receiver Module") |
|---|---|---|
| supply | 5 V DC | 5 V |
| pins | `S`, middle, `-` | `VCC`, `OUT`, `GND` |
| output | (n/a) | 1-bit digital, **active low, tested on this unit (2026-10-02): OUT goes LOW while the laser hits the receiver.** The listing says "logic low on no laser detection", the opposite, and is wrong for this unit. |
| wavelength | 650 nm | about 650 nm |
| listing's Arduino wiring | `S` to digital pin 6, middle to 5 V, `-` to GND | VCC to 5 V, OUT to digital pin 7, GND to GND |

The listing does **not** state:
- the laser's current;
- whether the receiver's OUT is push-pull or a resistor pull-up;
- the receiver's response time or whether it has hysteresis;
- what its on-board LED shows.

Each of these matters below, and each is a bench measurement, not an assumption.

## Wiring (proposed; you wire it)

| what | to |
|---|---|
| transmitter middle | 5 V |
| transmitter `-` | GND |
| transmitter `S` | 5 V: the laser stays on. This is the listing's wiring with its pin 6 held high. |
| receiver VCC | 5 V |
| receiver GND | GND, common with the DevKit |
| receiver OUT | the divider below: **50 kΩ** from OUT to **GPIO 7**, and GPIO 7 **100 kΩ** to GND |

```
receiver OUT ──[50 kΩ]──┬── GPIO 7
                        │
                     [100 kΩ]
                        │
                       GND
```

**Why 50k / 100k (operator, 2026-10-02).** The receiver's output is weak: into a 10k + 20k divider (30 kΩ
load) it sagged from 5 V to 3.5 V. That implies about 13 kΩ of source impedance (30 × (5/3.5 − 1)), a
first-order estimate, not a measurement. At 3.5 V a 2:3 divider gives only 2.33 V, below the ~2.5 V
(0.75 × 3.3 V) the pin needs to read high. The same 2:3 ratio at 150 kΩ loads the output five times less:
OUT should rise to about 4.6 V and the pin to **about 3.07 V**, inside the 2.5–3.6 V window. If OUT were a
full 5 V the pin would sit at 3.33 V, still safe. **Measure both nodes** (OUT and GPIO 7) with the beam
blocked after fitting it; if GPIO 7 reads under about 2.6 V the output is weaker than modelled.

**What the large values cost.** The firmware's ~45 kΩ internal pull-up would lift the LOW level to about
1.4 V against a 100 kΩ bottom leg, so the pin has **no internal pull** (`BOARD_SPOKE_BEAM_RESTING_PULL 0`).
Two consequences: the pull test (`laser probe`, `input driven/floating`) cannot work and reports
`unprobed`, and a broken OUT wire reads CLEAR (the 100 kΩ pulls the pin to 0), not NO INPUT. Confirm the
wiring with a card in the beam under `laser watch`. A 33 kΩ source also picks up noise more easily than a
stiff one: keep the wires short and away from the motor and solenoid leads, and read the `glitches` and
`edges` counters while the roller runs.

**Why a divider at all.** OUT is nominally a 5 V logic level, and the ESP32-S3's GPIOs are not 5 V
tolerant (absolute maximum is VDD + 0.3 V = 3.6 V). Never wire OUT straight to the pin.

**Why GPIO 7.** It is the pin the frozen DevKit map already reserves for a wheel sensor
(`BOARD_INDEX_SENSOR_GPIO`, `board_s3_devkit.h`). Nothing moves, and nothing else uses it.

**Why not drive the laser from a GPIO.** The ESP32 pin is 3.3 V and the laser's current is not
specified. Switching the laser is not needed for this bench step.

The laser is a 650 nm pointer-class diode. Never look into the beam or its reflection off the rim, and
mount it below eye level.

### Meter checks before trusting it

1. **Receiver alone at 5 V, OUT unloaded.** The unit is active low (the operator's bench test): with the
   laser hitting the receiver OUT should read about 0 V, and with the beam blocked about 5 V. The meter
   also settles whether OUT is push-pull or relies on a pull-up inside the module.
2. **With the 50k + 100k divider, at both OUT and GPIO 7, board powered** (no internal pull on the pin).
   Beam blocked (OUT high): GPIO 7 must read at least 2.6 V and at most 3.4 V; modelled 3.07 V, and OUT
   about 4.6 V. Beam clear (laser hitting): GPIO 7 must read at most 0.8 V. If the blocked level reads
   under about 2.6 V, the output is weaker than modelled: use a level shifter instead, for example a
   74LVC1G17 powered from 3.3 V (5 V-tolerant input, with hysteresis). Over 3.5 V: stop and re-measure OUT.
3. **`laser` in the workbench.** It should show `input unprobed` (no pull test with this divider),
   `CLEAR` with the laser on the receiver, and `BLOCKED` with a card in the beam. `laser watch` prints
   each break and clear as you wave the card: do this, because it is also the only check that the OUT
   wire is connected.

## What the firmware does (`s3_devkit_lab` only)

- **Where it lives.** `src/laser_bench.c` holds the GPIO 7 interrupt, a 2 ms poll and the probe.
  `lib/truing_hal/src/beam_stop.c` holds the trigger decision and trial record; it is pure and
  host-tested in `test/test_hal_drive`. Both are compiled into every image, but only the lab image
  (`TRUING_BENCH_DRIVE`) does anything with them.
- **The trigger.** Every edge on GPIO 7 interrupts. The interrupt reads the level, the drive's step
  counter and the time. If the laser-stop is **armed**, the edge is a **break** (the beam became
  blocked), and the drive **is moving**, it calls the drive's own `stop()`. That is the same
  controlled deceleration `roll stop` asks for, and nothing else.
- **One-shot.** It disarms the moment it fires, so each trial is one `arm`.
- **Priority.** The laser only ever requests that stop. It never starts, extends or re-times a move.
  `roll stop` behaves as before (a second stop request is ignored once the drive is decelerating).
  `roll off` and a driver fault still end motion at once, and such a trial is recorded `INTERRUPTED`.
- **Arming needs no admission**, because it can only stop motion.
- **The endpoint.** `GET /debug/laser.json` is plain state. It also takes `?arm=1|0`, `?probe=1`, and
  `?since=<seq>` for the edge log.

**Trial outcomes**, judged from the beam level when the last step pulse went out:

| result | meaning |
|---|---|
| `STOPPED_IN_BEAM` | the beam stayed blocked from the break to the last pulse |
| `PASSED_THROUGH` | the beam was clear again by the last pulse; the clear position is recorded |
| `BLOCKED_AGAIN` | the beam cleared after the break and was blocked again at the end: chatter at the spoke's edge, or a later spoke |
| `NO_TRIGGER` | armed through a whole move with no break, or stopped by someone else first |
| `INTERRUPTED` | the coils were released or the driver faulted before the controlled stop finished |

**Diagnostics, and how far each one goes:**
- **`input driven` / `floating` / `unprobed`.** With a stiff external network the pin can be read with the
  internal pull-up and then the pull-down: a pin that follows the pull floats. That test is compiled
  out for the 50k + 100k divider (`BOARD_SPOKE_BEAM_RESTING_PULL 0`), where it would call a working
  receiver floating, so the input reads `unprobed` and a broken OUT wire reads CLEAR. Only a card in the
  beam proves the wiring.
- **`DARK`.** No laser has been seen for 10 s or more. The beam may be blocked, the laser off or
  misaligned, or the receiver unpowered. One input pin cannot tell which.
- **`glitches`.** An interrupt that found the level unchanged, meaning a pulse shorter than the
  interrupt latency.
- **`resyncs`.** An edge the poll found that no interrupt reported. Both counters are chatter evidence.

**Limits:**
- Step counts are pulses sent, never proof the wheel followed. The spring-loaded roller is known to slip
  (`DEMO_BUILD.md`).
- The stop time is good to about 2 ms, the poll period.
- The receiver's own response time is inside the measured overshoot and not separated from it.
- The trigger path has no debounce: chatter is counted, not filtered.

## The bench workflow (`tools/workbench.py`, `tools/WORKBENCH.md`)

    laser                   # health: input driven/floating, beam CLEAR/BLOCKED, edges, glitches
    laser watch             # wave a card through the beam: every break and clear, live
    roll on
    roll speed 400 800      # slow first
    laser scan 3200         # DISARMED pass: how many steps each spoke blocks the beam, and the spacing
    laser trial 3200 5      # 5 trials: arm, move, stop on the break, report, log a row
    roll speed 400 1600     # change ONE thing
    laser trial 3200 5
    laser log               # every trial this session, side by side (laser_trials.csv in the session folder)

`laser arm` followed by an ordinary `roll <steps>` runs one trial by hand and reports it the same way.
The heartbeat (`--heartbeat`, `mic`) gains a laser line beside the roller line.

**What to compare.** A stop lands inside the beam only when the steps sent after the break are fewer
than the steps the beam stays blocked. `laser scan` measures the blocked width; each trial measures the
overshoot. Each trial also prints the ramp's arithmetic for a stop from cruise, about v²/2a pulses
(800 steps/s at 1600 steps/s² gives about 200; 500 at 1000 gives about 125). Agreement with that
number says the counter did what the ramp planned. The beam level says what the wheel did.

## Deliberately unfrozen until there is data

- **Polarity.** Settled for this unit: active low (laser hits receiver = LOW), per the operator's bench
  test. `BOARD_SPOKE_BEAM_BLOCKED_LEVEL` is 1. The seller's listing is wrong about it.
- **Divider versus level shifter.** Meter check 2 decides.
- **Debounce and edge qualification.** None in the trigger path. Decide from `glitches`,
  `edges_after_trigger` and `BLOCKED_AGAIN` counts.
- **Which edge triggers.** Only a break, for now.
- **The speed and deceleration operating region.** No value is recorded anywhere until trials justify
  one.
- **The receiver's response time.**
- **Electrical noise.** Does the stepper or a solenoid kick couple into the receiver line? Run a
  `burst` and a long `roll` with the laser disarmed and watch `glitches`.
- **Where the beam sits relative to the plunger.** A trial says "stopped in the beam", not "spoke under
  the plunger".
- **Navigation.** A real `spoke_sensor_if.h` implementation, edge counting from spoke 0, and the
  stopping-distance calibration all come after this data exists.

## Results

None yet. Record each session as a dated entry:
- the conditions (speed, acceleration, roller preload, tyre, beam height);
- the session folder holding `laser_trials.csv`;
- what changed between runs.
