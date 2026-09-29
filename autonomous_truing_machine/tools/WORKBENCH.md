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

- **SD pull-down (two mics):** one 100 kΩ from GPIO6 to GND keeps SD at a clean 0 while it floats
  between the two mics' slots. This is the usual multi-mic INMP441 arrangement, but it has not been
  checked against the datasheet here. **Not needed with one mic**: a lone LEFT mic read ALIVE
  without it (2026-09-28). Add it once both mics share the bus if a dead slot or the padding bits look noisy.
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

### Roller (lab image only)

The roller keeps the pins its bench wiring already uses (`docs/ROLLER_BENCH.md` R0), so nothing
on the roller side moves when the mics and solenoids join it on the same board:

| roller | GPIO |
|---|---|
| STEP / DIR / EN | 16 / 17 / 18 (EN has its 10 kΩ pull-up to 3.3 V) |
| PDN_UART (optional, dead on the current driver) | 8 via 1 kΩ, and 9 direct |
| DIAG | 10 |

No pin is shared with the mics (4/5/6) or the solenoids (15/21).

**One breadboard, one supply.** The DevKit runs from USB. The TMC2209's VM and the solenoids can share one
12 V supply if its current rating covers a solenoid pulse *and* the motor; otherwise use two. Either way,
join grounds at **one** point at the supply's negative terminal and bring the DevKit's GND to it once. The
mic grounds return to the DevKit's GND only. Twist each motor phase pair, keep the motor and solenoid wires
away from the mic wires, and keep the mic leads short. The stepper's chopping noise is real: the workbench
opens a new epoch when you energise the coils, so a `ctrl 3` with the roller on and one with it off give
two floors.

## 2. Image and first run

For the mics alone:

    python tools/flash.py s3_devkit_fastdemo_mic_dualmic

For the mics, solenoids **and** roller on one board (no reflash to switch between them):

    python tools/flash.py s3_devkit_lab --port COMx       # COMx from `pio device list`

The lab image is the same firmware plus the roller as a bench drive, and `GET /id` reports
`"mode":"fastdemo+inmp441x2+campaign+drive"`. In the workbench:

    roll                # state: energised or off, moving or idle, position, speed, faults
    roll on             # energise the coils (opens a new epoch; the board keeps holding until `roll off`)
    roll 1600           # move +1600 steps at the current ramp; Ctrl+C decelerates it to a stop
    roll -1600          # and back (position returns to 0)
    roll speed 3818 1600  # cruise steps/s and acceleration steps/s2 for the next moves
    roll zero / roll stop / roll off

The roller moves in raw steps: there is no spoke, no reference and no calibration in this image, and
`position` counts pulses sent, not proof the wheel followed. A capture is refused while it moves, and every
saved shot records whether the coils were energised. The roller demo's navigation is a different image
(`s3_devkit_fastdemo_roller`).

The bring-up log prints a separate liveness line for the `LEFT-station mic` and the
`RIGHT-station mic`. Since the liveness fix it applies the same rule as the heartbeat below: a mic is
alive when its samples scatter, and a data line stuck on one value (0 when low, -1 when high, with or
without mains glitches on it) is reported as not driven, naming the value. A dead mic is a warning, not a
bring-up failure. `truing_hal/audio_liveness.h` holds the rule, and the heartbeat's `mic_health()` in
`tools/wb_analysis.py` is its host twin.

`GET /id` should report `"mode":"fastdemo+inmp441x2+campaign"` (`...+campaign+drive` on the lab image).
Join the board's AP, then:

    python tools/workbench.py --heartbeat                 # live mic check, nothing saved; Ctrl+C stops
    python tools/workbench.py --session <tag>             # resumes a session with the same tag
    python tools/workbench.py --session <tag> --offline   # no board: references, import, analysis, report

### Mic heartbeat (`--heartbeat`, or `mic` inside a session)

This reads half a second of both mics from the board's live stream about once a second (no acoustic
analysis, so it is fast enough for wiring work; on an older image without the stream it falls back to one
full capture every ~7 s) and prints one line per mic. Run it while you wire, and tap each mic to confirm it. Double-click `heartbeat.cmd` (in the repo's `autonomous_truing_machine` folder) or run
`python tools/workbench.py --heartbeat`: it joins the board's Wi-Fi and waits for the board to boot by
itself, and does the same again if the board drops out mid-run. The `--session` workbench does this at
start-up too.

| state | meaning |
|---|---|
| `ALIVE` | The samples scatter the way a powered mic's self-noise does. A quiet room reads about −90 to −60 dBFS rms. |
| `DEAD` | The data line sits on one value. That mic is unpowered, unclocked (SCK/WS not reaching it), or not on GPIO6. |
| `CLIPPING` | The mic is live but hitting full scale: too loud or too close. |

- **Tap test:** tap a mic. Its bar should jump and show `<< TAP`, and the other mic's line should not.
- **Don't trust a spectrum on a DEAD channel.** A floating wire picks up mains hum and flips level now and
  then. Those flips reach full scale and produce a comb of "lines" at multiples of ~120 Hz, and the
  board's DSP will even report a peak on them. Every saved shot prints the same ALIVE/DEAD verdict above
  its numbers.
- **Both mics DEAD at once** points at something they share: the SD, SCK or WS wire, or 3.3V/GND. It is
  unlikely to be the two mics failing together.

### Listening check (`listen`, inside a session)

    listen                 # 10 s from the current spoke's mic, then plays it
    listen 20 right        # 20 s (1..30) from the RIGHT mic

A capture is only 1.2 s. `listen` streams the mic live from the board's ring for as long as you ask
(`GET /debug/listen.pcm?s=<seconds>&station=<0|1>`) and saves one long `.norm.wav` and the raw `.pcm` as
`listen_NN_<LEFT|RIGHT>` in the session folder. The wav is peak-normalised, so it sounds loud whatever the
real level: for ears only. The verdict line says ALIVE/DEAD and the rms level. Use it to compare mics by ear
on the same track at the same volume and distance. Nothing is analysed and nothing is added to the shots.
The board serves nothing else while it streams (a ~20 s pause for the heartbeat and `/id`), and a second
listener is refused. If the stream ends early it says so instead of splicing over a gap. The Wi-Fi is the
limit: the PC may need `netsh wlan connect name=truing-xxxxxx` first if it dropped off the AP.

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
