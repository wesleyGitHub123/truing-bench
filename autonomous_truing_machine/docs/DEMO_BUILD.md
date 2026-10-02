# Pipeline-demonstration build (`s3_devkit_demo`)

The image for the 2026-10-03 demonstration. Its job is to show the whole chain working on the spot, in front of
an audience: a solenoid strikes a spoke, the station's microphone records it, the firmware analyses the
recording, and a frequency and a tension appear on a clean, light page with a per-spoke chart.

**It is a demonstration of the pipeline, not of the acoustic method.** The operator decided this on
2026-10-02: the absolute calibration rig is not available, so no f1-finding rule can be shown correct in
Stage 1 anyway, and the audience needs to see the system work. Where the DSP's own quality gates would
have rejected a capture (no onset, ambiguous or weak peaks, nothing in the f1 band), the build still
reports a frequency candidate, `SUSPECT / FORCED_CANDIDATE` (since 2026-10-03 the page no longer shows the
status; see "Tension survey" below). Captures the gates accept are reported
exactly as before (`SUSPECT / PROVISIONAL_MODE_ID`). Nothing is ever `VALID`. Still rejected: a capture
with no signal on the line (all zeros, or a data line stuck on one value -- a dead, unclocked or undriven
mic: there is no event, and a number there would hide a hardware fault), the rare capture where no
spectrum or finite SNR can be formed, and a frequency the tension model refuses. A forced pick that is the
band's largest spectral bin (rather than the interim rule's choice) carries mode identity `UNKNOWN`.

This deliberately departs from SPEC 7.4 (those captures map to rejected) for this build only. Because
`SUSPECT` is solver-admissible, a forced candidate must never reach a solve: it does not, because this
image's layout keeps tension out of the solve (`TENSION_ABSENT`), and that is **checked at boot** -- if the
loaded artifact ever selected a layout with tension, always-read stays off (fails closed) and `/id`
reports `"always_read":false`.

## Tension survey (2026-10-03, operator's decisions for the demonstration)

- **The session is the acoustic pass and nothing after it** (`TRUING_DEMO_TENSION_ONLY`). All 32 spokes are
  struck (`TRUING_ACOUSTIC_DEMO_SPOKES=32`); after the last one the run ends with the terminal result
  `TENSION_SURVEY_COMPLETE` -- no runout, no solve, no adjustment waits, no verify. That terminal is a
  **deviation from SPEC 13.2's terminal vocabulary, for this image only**: it is neither a truing success
  (nothing was trued) nor an abort. Every other image is unchanged (`end_after_tension_pass` is false).
- **The page shows readings without status wording.** No SUSPECT / FORCED_CANDIDATE chips, notes, tooltips or
  log suffixes; no pipeline-demonstration banners; no footnote under the spoke panel; no Session progress
  card; the station tables are Spoke / Hz / N / +-20%. The classifier is unchanged: every frame, the serial
  log and the captures still carry the status and reason, and other images' pages show them as before.
- **Complete card + Adjustment plan.** On `TENSION_SURVEY_COMPLETE` the status card reads "Complete - every
  spoke tension recorded" (or "N of 32" when fewer have a reading) and an **Adjustment plan** card lists, per
  spoke, the tension, its difference from its side's mean and the turn that would close it:
  `turns = (side mean - reading) / slope`, tighten when positive, capped at the solver's
  `max_adjustment_revolutions`. The page computes it; the firmware reports the inputs in `/id`
  (`c_side_a_n_per_rev`, `c_side_b_n_per_rev`, `max_adjustment_rev`). The slope is the wheel configuration's
  **reference figure (473 N/rev, fixture content), not measured on this wheel**, and the page says so. It is
  **not the matrix solver's plan** (that needs runout, which this image no longer reads). Side A = DS = odd
  spokes, matching the fixture wheel's indexing origin.

## Flash and open

    pio device list                                   # identify the board by serial, never by COM number
    python tools/flash.py s3_devkit_demo --port COMxx --build-dir ~/truing_roller/autonomous_truing_machine

Join the board's Wi-Fi (`truing-xxxxxx`; `heartbeat.cmd` / `tools/workbench.py` join it automatically),
open `http://192.168.4.1/`. `/id` reports `"mode":"fastdemo+inmp441x2+alwaysread+laser+survey"`,
`"always_read":true` (the fallback is actually on), `"jog":true`, `"survey":true`, `"laser":true` (the
receiver came up and positions the stations). `s3_devkit_demo_manual` reports `...+jog+survey`, `"laser":false`.

What the image is, flag by flag (`src/build_mode.h`, `platformio.ini`):

| flag | effect |
|---|---|
| `TRUING_FAST_DEMO` + `TRUING_REAL_FRONT_END` | the acoustic demonstration: real INMP441s and solenoid strikes; runout and the solve stay synthetic; tension is displayed evidence, never solver input (TENSION_ABSENT layout) |
| `TRUING_DEMO_ALWAYS_READ` | every capture yields a candidate (above); excludes the bench debug channel |
| `TRUING_DUAL_MIC` | each station listens with its own mic (the mics sit beside their solenoids) |
| `TRUING_DEMO_ROLLER_JOG` | the manual roller mode (below) |
| `TRUING_DEMO_TENSION_ONLY` | the session ends after the acoustic pass (`TENSION_SURVEY_COMPLETE`, above) |
| `TRUING_DEMO_LASER_NAV` | the laser-guided roller positions each spoke (below); not in `s3_devkit_demo_manual` |
| `BOARD_PLUCK_ACTUATOR_*_PRESENT=1` | both solenoids fire; the B0 electrical check on the DevKit is still open and the operator accepted that for the demonstration |

A session measures `TRUING_ACOUSTIC_DEMO_SPOKES` (32) spokes: the operator places each one at its station
and confirms it (spoke 0 at LEFT first, as the reference), the solenoid strikes, the reading appears.

## Navigation: two modes, one scaffold

### Manual roller mode (built, this image)

The operator turns the wheel with the roller from the page (Roller card: small and 1/8-turn jogs each
way, Release), then confirms the spoke at the station **by eye**, exactly as with hand positioning.
`GET /demo/jog?steps=<signed>` moves (energising the coils if needed), `GET /demo/jog?enable=0` stops and
releases, and a bare `GET /demo/jog` reports. It is not an operator intent: it answers no wait and moves no
state machine. Because it turns the wheel during a session, the firmware fences it both ways:

- a jog starts only when no session is running or the machine is asking the operator to position a spoke
  (reference, spoke, rim index) -- never between a confirmed positioning and its measurement, during a
  strike, capture, analysis or retry, or while a strike is being prepared;
- every strike first waits for a jog already under way to finish, releases the coils (no holding-current
  noise under the capture; the spring-loaded roller holds the wheel), waits 300 ms for the wheel to
  settle, and only then fires; a roller still moving after 10 s refuses the strike.

It is also refused while the roller still moves and beyond +-16000 steps per request. 3000 steps/s cruise,
3000 steps/s², 16 microsteps. The mic noise floor with the motor energised or moving is unmeasured; the
coil release before each strike is there so it does not have to be.

**Step counts are a jog distance, never a position.** The roller now sits in a spring-loaded bracket
(the operator's design: the stepper's mount is spring-loaded so the roller follows the wheel's shape and
stays in contact, which removed the dead spots where the wheel stopped being driven). A back-and-forth
test on it -- `roll +N` then `roll -N` -- did not consistently return the same spoke to the solenoid
(2026-10-01). So nothing in this build presumes a spoke's position from steps, and roller-calibration
work aimed at steps-per-revolution is set aside (`docs/ROLLER_BENCH.md` R3/R4, `src/roller_calibration.h`
stays empty).

### Automatic mode: laser-guided roller (built 2026-10-03, `TRUING_DEMO_LASER_NAV`, in `s3_devkit_demo`)

The alignment campaign proved the chain on the lab image with the PC driving it: a full revolution, 32/32
spokes found, stopped on and struck with nothing missed (`docs/ALIGN_CAMPAIGN.md`). This image runs that sequencer in firmware:

- **`navigation_laser`** (`lib/truing_hal`, host-tested against a simulated wheel with the bench wheel's
  spacing: `test/test_nav_laser`) is the composite's physical child. For each spoke it runs the per-class
  recipe (spoke mod 4: ramp, strategy, offset; values from `tools/align_revolution.py`), then **names the
  spoke that stopped from the wheel's measured 4-spoke spacing before anything strikes**. A stop on the
  wrong spoke, a run that did not complete, beam chatter, a width or stop distance off its value, or a
  reacquiring recipe that did not end aligned: back up to the gap before the spoke and run again, at most
  3 tries; then the spoke is handed to the operator as a positioning wait (jog it into the beam with the
  Roller card, confirm), and the next spoke is automatic again. A strike that nudged the next spoke into
  the beam is reconciled the way the workbench did.
- **Reference:** the operator places spoke 0 **just before the beam** and confirms; the first trigger is
  trusted as spoke 0 (nothing to name it against yet), later ones are named by spacing.
- **Strikes keep the coils energised** in this mode (the campaign struck that way; a released roller lets
  the wheel drift off the aligned spoke). The manual image still releases them.
- **Not ported:** the PC script's backward re-locating scan (its anchor looks one blocked width off) and its
  reboot / Wi-Fi recovery. Whatever would need them goes to the operator instead.
- A driver reset that the enable repaired (incident 1) drops the spacing anchor; the spoke count carries on.
- If the receiver or the drive does not come up at boot, the stations stay **manual** (never synthetic) and
  `/id` reports `"laser":false`.
- **Fallback image:** `s3_devkit_demo_manual` is the same survey with the manual roller only.

What is unproven: none of this has run on the board yet (it was written while the perfboard was being
resoldered); the recipes were tuned on one wheel in one power state; the mic noise floor with energised
coils is unmeasured.

### The earlier scaffold

The operator's plan: a laser transmitter and receiver across the strike point, one on each side of the
wheel near the plunger. A spoke passing breaks the beam. The roller turns the wheel at a known speed and,
on the beam break of the wanted spoke, decelerates so the wheel stops with that spoke at the solenoid.
Position knowledge comes from beam edges, not steps; the beam being blocked at the stop is the
corroboration a navigation implementation can report as `sensor_confirmed`.

What exists: the seam `lib/truing_hal/include/truing_hal/spoke_sensor_if.h` (beam edges ENTER/LEAVE with
timestamp and the drive's step counter at that instant; `blocked()`), a fake with a host test
(`test/test_hal_drive`), and the reserved flag `TRUING_SPOKE_SENSOR`, which refuses to build until a real
implementation exists. The board profile already reserves a pin for an index sensor
(`BOARD_INDEX_SENSOR_GPIO`, 7 on the DevKit). Not designed yet, because the module is not delivered: the
sensor driver, the counting of spokes from the spoke-0 reference, the stopping-distance calibration
(edge to strike point at a given speed), and the navigation implementation that would replace the
manual stations in the composite.

The first bench step toward it exists on the lab image (`s3_devkit_lab`, not this one): the receiver on GPIO
7 as an observable subsystem and a one-shot laser-stop that measures trigger -> controlled stop -> where the
counter and the beam ended up (`docs/LASER_BENCH.md`). It is characterization, not this navigation.

If the automatic mode misbehaves at the demo, flash `s3_devkit_demo_manual`: manual roller, operator
confirms by eye.

## What the audience sees

A light page (no dark mode). The Operator tab carries the session controls, the latest measurement, a
**Spoke tension** panel (left-station and right-station tables: spoke, frequency, tension in N, within
+-20% of that station's mean; a radar chart of the measured spokes; per-station mean, standard deviation
and +-20% limits), the Roller card, and at the end the Complete card and the Adjustment plan. Stations are
split by spoke parity (even = LEFT, odd = RIGHT), which is the actuator station, not a verified flange side.

## Limits, stated

- A reading from this build is not evidence of anything about spoke tension. The frequency the firmware
  reports on the frozen spoke 0 is, on this rig, most likely a wheel/frame line rather than spoke 0's own
  fundamental (`docs/SOLENOID_CAMPAIGN.md`, frozen-rig characterization; `_explore/char_20260930/LOG.md`).
- The tension number comes from the fixture tension model (ideal string with a synthetic effective
  length), not a calibrated one.
- B0 (solenoid V_DS, channel isolation) is open on the DevKit.
