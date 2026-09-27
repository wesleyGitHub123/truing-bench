# Dual-mic workbench: operator manual

A fast, manual loop for characterizing the two-microphone acoustic set-up:
**experiment → observe → analyze → adjust**. It is **exploratory**. There is no plan, no trial
numbers, no fixed n and no exclusion ledger, and nothing it produces is campaign evidence until a
later registered run confirms it. The record of what was done lives in `docs/SOLENOID_CAMPAIGN.md`.

Two people: **R** at the rig and **K** at the keyboard.

## 1. Board and wiring

**The acoustic front end lives on a spare ESP32-S3-DevKitC-1**, not the Nano — the original Nano
died to a wiring mistake (2026-09-27) while adding the second mic. Everything below is the DevKit's
own pins (`lib/truing_board/include/board/board_s3_devkit.h`), which already reserves both the mic
bus and both solenoid GPIOs with no conflicts.

**If a second DevKitC-1 is also attached** (the roller campaign's, fixed at COM4): tell them apart
with `pio device list` / each board's own `GET /id` before flashing or wiring anything — never by
env name or "the DevKit" alone. This board's own upload port is not pinned in `platformio.ini` for
exactly this reason; pass `--upload-port` explicitly once you know it.

### Microphones

| INMP441 pin | LEFT-station mic | RIGHT-station mic |
|---|---|---|
| VDD | 3.3V | 3.3V |
| GND | GND | GND |
| SCK | GPIO4, shared | GPIO4, shared |
| WS | GPIO5, shared | GPIO5, shared |
| SD | GPIO6, shared | GPIO6, shared |
| **L/R** | **GND → left slot** | **3.3V → right slot** |

- **SD pull-down:** fit one 100 kΩ from GPIO6 to GND. SD floats between the two mics' slots. This
  is the usual multi-mic INMP441 arrangement, but it has not been checked against the datasheet
  here, so read the datasheet first.
- **L/R:** never leave it floating.
- **Lead length:** BCLK is 3.07 MHz, so keep leads short and twist each signal with a ground.
- **Mount trap:** a mic on a solenoid tower hears the tower through its mount. Coupling is a knob to
  vary, not a detail.
- **Wire before flashing anything new.** Every existing image reads only the left slot, so it still
  hears the LEFT mic.

### Solenoids (bring up separately, after the mics)

| | LEFT | RIGHT |
|---|---|---|
| GPIO | 15 | 21 |
| `PRESENT` | 0 until bench-verified | 0 until bench-verified |

Same electrical design as the Nano's stations (`docs/SOLENOID_CAMPAIGN.md`'s rig registry): IRLB4132
logic-level MOSFET, 100–150 Ω series gate resistor, 10 kΩ gate-source pulldown (mandatory), flyback
diode across the solenoid (mandatory). **Grounds:** mic grounds and the solenoid 12 V return meet
only at the shared supply's star ground point — never route mic leads beside solenoid leads.
**Do not flip `BOARD_PLUCK_ACTUATOR_*_PRESENT` in the board header until that station passes its own
B0 electrical check** (`tools/bench/solenoid_smoke`'s `s3_bench_solenoid` env, `tools/bench/README.md`)
— the same discipline the Nano's stations went through before `fire`/`air` were trusted. Until then,
`fire`/`air` correctly reject `EXCITATION_UNAVAILABLE`; `ctrl` and `pluck` need no actuator and work
today.

## 2. Image and first run

    python tools/flash.py s3_devkit_fastdemo_mic_dualmic

The bring-up log prints a separate liveness line for the `LEFT-station mic` and the
`RIGHT-station mic`. A line that is all zeros means that mic is dead or its L/R strap is wrong.

`GET /id` should report `"mode":"fastdemo+inmp441x2+campaign"`. Join the board's AP, then:

    python tools/workbench.py --session <tag>             # resumes a session with the same tag
    python tools/workbench.py --session <tag> --offline   # no board: references, import, analysis, report

This runs with the system `python` (numpy and matplotlib). Sessions are written to
`test/fixtures/acoustic/captures/_explore/wb_<date>_<tag>/`, which is gitignored and exists only on
this machine.

## 3. How a shot works

1. `fire` sends one `MEASURE_ONCE`. The solenoid at the spoke's own station strikes; the station is
   **spoke parity**, even = LEFT.
2. The board captures 1.2 s from **both** mics over the same frames, so the two channels are
   sample-aligned.
3. **Only the struck station's own mic (`local`) goes through the board's DSP.** It is the same mono
   stream, chain and digest as before, so tension is never computed from two mics.
4. The other mic (`far`) is kept as a reference dump and never analysed on the board.
5. The workbench fetches both under one sequence check and writes the bundle:
   - `<shot>.pcm` + `<shot>.json` — local channel plus board verdict. This is the faithful record, and
     it replays in the same way as every other bundle.
   - `<shot>.far.pcm` — the far channel.
   - `*.local.norm.wav`, `*.far.norm.wav` — listening copies, peak-normalised, so **the levels are
     not real**.
   - `<shot>.png` — the figure.
   - a line in `session.jsonl`.
6. It prints the per-channel numbers and opens the figure.

## 4. The loop

**Setup, once per epoch.** Moving a mic moves the floor, so do this again after every physical
change:

- `ctrl 3` — no-fire controls, which set this epoch's room floor for both mics.
- Tap test — R taps the LEFT mic during a `ctrl`, and the LEFT station's channel must spike. Then do
  the same for RIGHT. A shot whose `mic_input` disagrees with the board profile is flagged `!!`.
- `ref preset spoke0-pluck` — the 09-21 hand plucks that cleared at 406.2 Hz.

**1. Experiment**

    set spoke 0            spoke parity picks the station
    set pulse 60           0 = the profile's width; the tool warns outside the OLD rig's B2-M3
                           brackets (LEFT 40–85, RIGHT 60–95) -- unverified on this rig, kept only
                           as a starting point until a fresh sweep re-measures them
    set reps 6
    set mic.dist 15        ANY key not listed by `knobs` is physical set-up and opens a new EPOCH:
    set mic.coupling foam  mic.mount, strike.point, standoff, hold, damping, ...
    fire | ctrl | air | pluck [n]

An **epoch** is one physical set-up plus one firmware identity (build and chain digest). A reflash
opens a new epoch automatically. Spoke, pulse and excitation vary freely inside an epoch and are
recorded on every shot, so nothing is compared across set-ups by accident.

**2. Observe** (printed after every shot)

- **Per mic:** peak, rail hits, impact, in-band (350–600 Hz) and wideband level over the epoch floor,
  and the top five lines with how far each stands over the floor.
- **Board:** status, reason, f1 and SNR on the local mic.
- **`loc` — localization.** Δ = local − far at the focus frequency, and the same Δ at this station's
  rig lines. **Index = Δ − median Δ(rig)**; with no rig lines yet it uses Δ − Δ(controls). A positive
  index suggests energy excited near the local mic. **No threshold exists.**
- **Figure:** rows are local and far. Columns are the waveform (host ring window and the board's
  window), the ring spectrum against the floor (350–600 shaded, first reference overlaid), and a
  spectrogram.
- `play local`, `play far`, `play ref [n]`, `view [id]`, `export [id]` (faithful 24-bit WAVs).
  Listening helps diagnosis; it is never the criterion.

**3. Analyze**

| command | what it gives |
|---|---|
| `compare [id\|refN] [id\|refN]` | onset-aligned overlay, normalised spectra, board metrics side by side, lines agreeing within ±2 Hz |
| `repeat` | per spoke, excitation and pulse: board f1 over the clears, and the top-line frequency spread |
| `lines` | the 09-21 method (≥10 dB over the floor in ≥80 % of a spoke's strikes; within 6 Hz on ≥2 spokes = **rig**). It needs ≥2 spokes struck at a station in the epoch, and it feeds `loc`. |
| `loc [f]` | localization for every shot in the epoch |
| `board` | verdicts by kind. Any clear on `ctrl` or `air` is a **false clear**. |
| `ls [all]` | the shot table |

**4. Adjust**

    decide keep|reject|ambiguous <why>
    note <text>
    report

`decide` stamps the epoch. Then change **one** thing and repeat. `report` writes `report.md`,
`shots.csv` and `epochs.csv` in the session directory.

## 5. What to look for

These are reading guides, not thresholds.

**Promising**
- A local line that repeats within ±2 Hz across one spoke's strikes (the plan's consistency band).
- It is **not** in the rig list.
- It moves when the spoke changes.
- Its localization index is clearly above the rig lines'.
- It stays put across pulse widths.
- On spoke 0 it agrees with the reference (~406 Hz) or has the reference's shape.
- The board clears on it, and the controls stay clean.

**Bad direction**
- The strongest lines are the same across spokes (rig).
- Δ at the candidate is no better than Δ at the rig lines.
- Only the impact click stands over the floor.
- A line moves when **only the mic mount** moves (the mount is ringing).
- A line moves with pulse width (the plunger is loading the spoke, an untested hypothesis).
- Rail hits on the local mic (it is too close).
- Any clear on a control.

**Reference points from Phase B**
- Hand pluck on the old single mic: SNR 16–24 dB at 406.2 Hz on spoke 0.
- B3.2 strikes: median SNR 4.0 dB.
- The references were taken with the old mic at its old position. Compare frequency and shape, not
  level.

**Worth keeping** means the promising signs hold on ≥2 spokes per station, with the controls clean.
Record it with `decide keep`. Confirming it is a separate, registered step.

**Also watch.** The board's onset often lands at sample 0, the start of the capture (see the
2026-09-27 entry in `docs/SOLENOID_CAMPAIGN.md`). When it does, the board's window (green in the
figure) spans the impact rather than the ring.

## 6. What may change

| tier | knobs | how |
|---|---|---|
| A — first | spoke, pulse, reps, excitation; mic position, distance, coupling and mount; strike point, standoff, holding and damping | `set` plus shots. Physical keys open an epoch. |
| B — only when an observation points at the DSP | f1 band, gate, window, prominence, SNR gate, onset | **Not on the board.** Replay the stored local `.pcm` offline under a named variant, reported in its own column (a later phase: the `native_sweep` harness gains an override). The board chain stays the fixed reference. |
| C — reflash | capture length and pre-trigger (both change `chain_digest`), I2S/DMA settings | Rebuild; the new build and digest open a new epoch automatically. |
| not here | tension model, sessions, solver, the demo image, PRESENT flags, pins, rig-registry tokens | halt and ask |

Raw captures never depend on DSP settings. A DSP change therefore never needs new shots, only a
replay of the ones already taken.

## 7. References

    ref preset spoke0-pluck
    ref <bundle name | .json | .pcm | any WAV exported from a bundle>
    ref list | ref clear

Replay and analysis read the **bundle** (`.pcm` + `.json`), never a WAV. A `.norm.wav` is traced
back to the bundle it was exported from; a WAV with no bundle is refused, because a listening copy's
levels are rescaled. Fresh hand plucks remain an option (`pluck`), but they are not needed on every
iteration.
