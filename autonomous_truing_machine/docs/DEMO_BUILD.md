# Pipeline-demonstration build (`s3_devkit_demo`)

The image for the 2026-10-03 demonstration. Its job is to show the whole chain working on the spot, in front of
an audience: a solenoid strikes a spoke, the station's microphone records it, the firmware analyses the
recording, and a frequency and a tension appear on a clean, light page with a per-spoke chart.

**It is a demonstration of the pipeline, not of the acoustic method.** The operator decided this on
2026-10-02: the absolute calibration rig is not available, so no f1-finding rule can be shown correct in
Stage 1 anyway, and the audience needs to see the system work. Where the DSP's own quality gates would
have rejected a capture (no onset, ambiguous or weak peaks, nothing in the f1 band), the build still
reports a frequency candidate, `SUSPECT / FORCED_CANDIDATE`, and the page says, in plain words, that
readings show the chain working and are not validated tensions. Captures the gates accept are reported
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

## Flash and open

    pio device list                                   # identify the board by serial, never by COM number
    python tools/flash.py s3_devkit_demo --port COMxx --build-dir ~/truing_roller/autonomous_truing_machine

Join the board's Wi-Fi (`truing-xxxxxx`; `heartbeat.cmd` / `tools/workbench.py` join it automatically),
open `http://192.168.4.1/`. `/id` reports `"mode":"fastdemo+inmp441x2+alwaysread+jog"`,
`"always_read":true` (the fallback is actually on), `"jog":true`.

What the image is, flag by flag (`src/build_mode.h`, `platformio.ini`):

| flag | effect |
|---|---|
| `TRUING_FAST_DEMO` + `TRUING_REAL_FRONT_END` | the acoustic demonstration: real INMP441s and solenoid strikes; runout and the solve stay synthetic; tension is displayed evidence, never solver input (TENSION_ABSENT layout) |
| `TRUING_DEMO_ALWAYS_READ` | every capture yields a candidate (above); excludes the bench debug channel |
| `TRUING_DUAL_MIC` | each station listens with its own mic (the mics sit beside their solenoids) |
| `TRUING_DEMO_ROLLER_JOG` | the manual roller mode (below) |
| `BOARD_PLUCK_ACTUATOR_*_PRESENT=1` | both solenoids fire; the B0 electrical check on the DevKit is still open and the operator accepted that for the demonstration |

A session measures `TRUING_ACOUSTIC_DEMO_SPOKES` (3) spokes: the operator places each one at its station
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

### Automatic mode (planned; scaffold only)

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

If the automatic mode is not ready by the demo, this image IS the fallback: manual roller, operator
confirms by eye.

## What the audience sees

A light page (no dark mode). The Operator tab carries the session controls, the latest measurement, a
**Spoke tension** panel (left-station and right-station tables: spoke, frequency, tension in N and kgf,
within +-20% of that station's mean; a radar chart of the measured spokes; per-station mean, standard
deviation and +-20% limits), the Roller card, and a calm banner stating that this is a pipeline
demonstration. Stations are split by spoke parity (even = LEFT, odd = RIGHT), which is the actuator
station, not a verified flange side.

## Limits, stated

- A reading from this build is not evidence of anything about spoke tension. The frequency the firmware
  reports on the frozen spoke 0 is, on this rig, most likely a wheel/frame line rather than spoke 0's own
  fundamental (`docs/SOLENOID_CAMPAIGN.md`, frozen-rig characterization; `_explore/char_20260930/LOG.md`).
- The tension number comes from the fixture tension model (ideal string with a synthetic effective
  length), not a calibrated one.
- B0 (solenoid V_DS, channel isolation) is open on the DevKit.
