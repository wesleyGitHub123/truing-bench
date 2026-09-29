# Subsystem bench work (one DevKit, one breadboard)

The three subsystems -- microphones, solenoids, roller -- are bench-proven on the one
ESP32-S3-DevKitC-1 (serial `5C4C213858`, AP `truing-xxxxxx`, host 192.168.4.1) running the
`s3_devkit_lab` image. This file is the pinout and the state of that bench work. Operator manual for
the host side: `tools/WORKBENCH.md` (`heartbeat.cmd` is the one-command mic check). The roller's
step-by-step measurement plan stays in `docs/ROLLER_BENCH.md`; solenoid electrical checks in
`docs/SOLENOID_CAMPAIGN.md`.

The pins below are `lib/truing_board/include/board/board_s3_devkit.h` and are **frozen for the demo**
(2026-10-03). Nothing else may use them.

## Pinout

| function | GPIO | wired to |
|---|---|---|
| mic SCK (BCLK) | 4 | both INMP441 SCK |
| mic WS | 5 | both INMP441 WS |
| mic SD (DIN) | 6 | both INMP441 SD (shared line) |
| mic LEFT / RIGHT | -- | the L/R strap on each module picks its slot: one to GND, one to 3.3 V |
| solenoid LEFT | 15 | driver stage, see SOLENOID_CAMPAIGN.md |
| solenoid RIGHT | 21 | driver stage, see SOLENOID_CAMPAIGN.md |
| roller STEP | 16 | TMC2209 STEP |
| roller DIR | 17 | TMC2209 DIR |
| roller EN | 18 | TMC2209 EN, active low, 10 kΩ pull-up to 3.3 V |
| driver UART TX | 8 | **10 kΩ**, then the module's RX pad (see below) |
| driver UART RX | 9 | the module's RX pad directly (joined with the 10 kΩ output) |
| driver DIAG | 10 | TMC2209 DIAG (firmware pull-down: unconnected reads "no fault") |

Driver side: VIO 3.3 V, GND common with VM ground, VM 12-24 V with >= 100 µF across VM/GND at the
driver, motor current set by the VREF pot. Never connect or disconnect the motor with VM live.
Mic grounds return to the DevKit GND only; keep mic leads short and away from motor phase pairs and
solenoid leads.

## The RX conundrum (driver UART)

The TMC2209 talks over ONE wire (PDN_UART), sync byte 0x05, node address from MS1/MS2, CRC8 poly 0x07.
The ESP32's TX and RX therefore have to share that wire, and the board hears its own transmit echo.

- What we tried first: GPIO 8 through **1 kΩ** into the module, GPIO 9 direct. The bench log
  (ROLLER_BENCH.md) recorded it as unresolved: no answer on any node, continuity verified.
- What works on the BTT V1.3 module (2026-09-29): **GPIO 8 -> 10 kΩ -> the module's RX pad, with
  GPIO 9 joined to that same RX pad.** The driver then answers: IOIN, GCONF, CHOPCONF and DRV_STATUS
  read back, and the firmware verifies what it writes. The fix came from the 10 kΩ-on-TX advice in the
  community thread the user found; the value matters, 1 kΩ did not work here.
- Naming trap: the module's silkscreen "RX/TX" is the module's view, and on this board the pad that
  reaches PDN_UART is the RX pad. Wire the ESP's TX (through 10 kΩ) and the ESP's RX both onto it.
  Do not use the module's TX pad as the return path.
- Without the UART the drive still runs on STEP/DIR/EN alone; only the status readout and the
  microstep verify are lost.

## What the UART gives the bench (lab image)

- `GET /debug/drive.json` -> `drv_ok`, `drv_status`, `gstat`, `cs_actual`, `drv_flags` (OT, OTPW,
  S2GA/B, S2VSA/B, OLA/OLB or `ok`). The workbench `roll` line prints them.
- Microsteps: the bench drive now requests **16** and the driver verifies it over the UART, refusing
  moves (MICROSTEP_MISMATCH) if it cannot. 16 is the measured count (3200 steps = 1 revolution,
  ROLLER_BENCH R1). The MRES register read 8 before because it is ignored unless `mstep_reg_select`
  is set; the MS1/MS2 pins were already selecting 16. Confirmed 2026-09-29: image boots with the
  fault latch clear.
- The driver never sets motor current. That is the VREF pot (and IRUN, which powers up at 31). It
  reads back at 31 while moving, so it runs hot; turn VREF down by hand or add an IHOLD_IRUN write
  (not done).

## Bench results to date (2026-09-29, lab image, solenoids enabled)

| check | result |
|---|---|
| both mics | ALIVE, LEFT ~ -40 dBFS, RIGHT ~ -32 dBFS; a bad LEFT module (0 Ω across its supply) was replaced |
| solenoids | LEFT and RIGHT fire individually and together; user heard and saw each |
| roller | position returns to 0 after reversals; 40,000 steps at 3818 steps/s completes; shaft turns smoothly (user-confirmed) |
| everything at once | roller move + 40-shot solenoid burst + both mics ALIVE, no resets, driver read OK before and after |
| driver flags | 1 transient OLB in 73 samples at speed; GSTAT clean, DIAG never high |

Not measured: solenoid V_DS (B0), the cause of the shorted spare LEFT mic, and anything
acoustic (nothing here says a pluck is detected, only that the front end is alive).

## Next: mic profiling

Each solenoid now has a microphone station beside it. Profiling starts from the ALIVE heartbeat, one
mic per solenoid station, using the workbench (`tools/WORKBENCH.md`). Bench work above is the
baseline it builds on; profile results belong in their own entry, not here.
