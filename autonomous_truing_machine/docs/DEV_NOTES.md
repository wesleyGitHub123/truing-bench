# Developer notes

Operational notes for working on this firmware: where things live, how to build, which test tier to
run, how to flash. `docs/SPEC.md` is the **normative contract**; this file is operational only and never
overrides it. Where the two disagree, the spec wins and this file is the stale one.

## Layout

    autonomous_truing_machine/   the firmware (PlatformIO project)
    model_prep/                  host-side influence-matrix generation -> artifact
    acoustic_prep/               golden acoustic fixtures from the research campaign

## Subsystem boundaries

One `lib/` directory per subsystem; `src/` is the ESP-IDF application and the composition root. SPEC §5
fixes both the decomposition and the dependency rules.

| where | owns | spec |
|---|---|---|
| `truing_core` | pure data and contracts: wheel state, measurements, status, admission, config, artifact, plan | §6, §8.11 |
| `truing_calc` | the solver: influence-matrix application, two-part solve, cost function. No I/O. | §8 |
| `truing_dsp` | onset, envelope, FFT, spectrum, peaks, tension model | §9.2 |
| `truing_hal` | interfaces plus real/recorded/synthetic implementations: acoustic, runout, navigation, pluck, clock, telemetry | §5.1, §10, §10A |
| `truing_orchestrator` | the state machine, cycle loop, per-spoke loop, auto-operator | §7 |
| `truing_proto` | JSON, wire framing, session protocol | §12 |
| `truing_board` | one board-profile header per board, the only place a pin number is allowed to appear | §4.3 |
| `truing_fixtures` | golden fixtures shared by host tests and on-target bring-up | §14.2 |
| `src/` | application: `main`, `bringup*` (on-target self-check), `orch_demo` (dependency wiring), `net_transport` + `web_ui`, NVS and artifact stores | §4.4 |

Four rules are binding:

1. **The domain layer never touches the HAL.** `truing_calc` is pure math and stays host-testable with
   no hardware present.
2. **Everything hardware-facing goes through `truing_hal`**, comms and navigation included.
3. **Verification is a domain module, not a measurement subsystem.** It re-invokes the same acoustic
   and runout interfaces the measurement phase used and never grows its own acquisition path.
4. **Wheel state is data.** It holds measurements; it does not acquire them.

Acoustic is a sensor+actuator composite behind a one-call contract (§9.1): pluck, capture and estimate
are its internals, not orchestrator steps. Navigation is the single authority on wheel position
(§10A); in Stage 1 the operator is the actuator through `WAIT_FOR_OPERATOR`, or the laser-guided
roller on the demonstration image. The influence matrix is a data artifact, not a component (§5.2).

## Which spec section answers which question

| question | section |
|---|---|
| what a status value means, and what may be solved on | §6.3, §8.11 |
| why tension rows are excluded from this solve | §8.4 Part 1, §8.11 |
| retry, settle, partial state, `REMEASURE_GAPS` | §7.3.1, §7.4 |
| what a session may claim to have converged | §7.5.1, §8.6.3 |
| acoustic external contract and internal seams | §9.1, §9.2 |
| capture robustness requirements | §9.4 |
| units, sign conventions, indexing origin | §6.4, §6.5 |
| telemetry rules; what telemetry must never do | §12.2, §13.3 |
| debug channel scope | §12.5 |
| what is deliberately unresolved | §16 |
| the silent-wrong-answer list, before implementing | §17.3 |

## Build path and tools

ESP-IDF refuses any build path containing a space. On Windows, if the checkout lives under a folder with
spaces, every `pio` command runs through a junction:

    $env:USERPROFILE\truing_ws  ->  ...\autonomous_truing_machine

`pio` is not on PATH on the development machine. Write commands against `$env:USERPROFILE`:

    & "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe" run -d "$env:USERPROFILE\truing_ws" -e s3_devkit

Always pass `-d`. Recreate a missing junction with:

    New-Item -ItemType Junction -Path "$env:USERPROFILE\truing_ws" `
             -Target (Join-Path (git rev-parse --show-toplevel) autonomous_truing_machine)

Host tests are unaffected by the space problem and run from either path. `docs/BUILD.md` explains each
image and why it exists.

## Verification tiers

Run the cheapest tier that covers what changed, then stop. Costs are measured on the development PC.

| Tier | What | Cost | Run when |
|---|---|---|---|
| T0 | flag-matrix syntax check (below) | 3 s | `build_mode.h` or `platformio.ini` changed |
| T1 | `node tools/ui_check.js` | 3 s | `src/web_ui.h` changed |
| T2 | `pio test -e native` | about 1 min | always |
| T3 | build the affected image(s) only | 5-100 s | any `src/` or `lib/` change |
| T4 | the 5 flag variants on one board + 1 build on the other | about 2 min | pre-commit for a cross-cutting change |
| T5 | every release image | 8-11 min | release gate, and whenever T4's coverage is not provable |
| T6 | on-target probe | 3-5 min | driver, transport or timing changes |

`-f <suite>` runs one native suite (for example `-f test_acoustic_replay`, about 1 s). Program output
only appears with `-v`.

T6 tooling: `tools/probe/` (network checks), `tools/serial_capture.py` and `tools/nano_serial.py` (UART,
one per board, which need opposite reset handling), `tools/flash.py` (build, flash and confirm the boot).

T0 in full: five legal flag combinations must produce no errors, three illegal ones must fail
(`build_mode.h` enforces them with `#error`):

    cd autonomous_truing_machine/src
    printf '#include "build_mode.h"\nint main(void){return 0;}\n' > /tmp/bm.c
    gcc -fsyntax-only -I. $FLAGS /tmp/bm.c

    legal   (none) | SELF_PLAY | FAST_DEMO | REAL_FRONT_END | FAST_DEMO+REAL_FRONT_END
            | FAST_DEMO+REAL_FRONT_END+CAMPAIGN_DEBUG[+DUAL_MIC]
    illegal SELF_PLAY+FAST_DEMO | SELF_PLAY+REAL_FRONT_END | ACOUSTIC_DEMO_SPOKES alone
            | DUAL_MIC without CAMPAIGN_DEBUG

## Working loops

**Feature.** Read the one spec section that governs it. Write the host test first, against
`truing_fixtures` and the synthetic HAL. Prove every host-testable contract before touching a board;
driver, transport, timing and physical-signal claims still need on-target evidence. Implement in the
owning `lib/`, run T2, then build the affected image.

**Debugging.** Classify first, then buy the cheapest ground truth that separates the candidates;
reproduce; fix; add the regression. Reproduce on the host wherever the bug allows. If it is target-only,
capture the serial console and telemetry and work from the capture. Serial is ground truth when the UI
and the firmware disagree.

**Acoustic bugs have their own loop**, because a pluck cannot be repeated. Keep the samples:
`GET /debug/capture.json` + `/debug/capture.pcm` (SPEC §12.5) hand over the words the last measurement
analysed; `tools/capture_fetch.py` turns them into a bundle under `test/fixtures/acoustic/captures/`;
`pio test -e native -f test_acoustic_replay` pushes them back through DSP layers 2-4 on the host with the
board's own numbers as the expectation.

**Observability** is fair game anywhere. Telemetry is observational: it must never participate in a
control decision (§12.2, §13.3).

## Images

Every ESP environment differs along two axes: the board profile header (pins) and the
`src/build_mode.h` flags.

| env | what it is |
|---|---|
| `s3_devkit`, `nano_esp32` | interactive: a person answers every wait |
| `*_selfplay` | auto-operator answers its own waits; the unattended evidence run |
| `*_mic` | interactive + the physical INMP441 front end |
| `*_fastdemo` | synthetic acquisition, selectable per session in the UI |
| `*_fastdemo_mic` | real mic, synthetic runout, bounded solenoid strikes |
| `*_fastdemo_mic_campaign` | the above plus the debug channel for `MEASURE_ONCE` bench characterization |
| `*_fastdemo_mic_dualmic` | the campaign variant with both I2S slots read, for `tools/workbench.py` |
| `s3_devkit_demo`, `s3_devkit_demo_manual` | the pipeline demonstration: laser-guided roller (or manual roller) and a tension survey across all spokes; see `docs/DEMO_BUILD.md` |
| `s3_devkit_lab` | the laser/roller lab image driven from the PC |
| `s3_devkit_provision` | one-off NVS fixture provisioning |
| `native` | host unit tests |

Every image stamps its own identity at build time (`tools/build_identity.py`): the short git revision,
`-dirty` if the tree was not clean, and a hash of `src/web_ui.h`. `GET /id`, the boot log and the page
itself print them, so "is the board serving what I just built" is a comparison, not a guess.

## Boards

The acoustic front end (both mics, both solenoid stations) and the roller drive each run on an
ESP32-S3-DevKitC-1. The Arduino Nano ESP32 that carried the first acoustic rig was damaged by a wiring
mistake on 2026-09-27 and is out of rotation; its environments stay in the build matrix as a record of
what ran there.

COM numbers are assigned by the OS and can move. The MAC and the access-point SSID are each board's
identity (SSID and passphrase are derived from the MAC, `derive_credentials` in `src/net_transport.c`).
`GET /id` is the reliable way to tell which board and which build you reached.

### Flashing

`tools/flash.py <env>` builds, flashes the board-correct way, then resets and reads the console for the
bring-up line `BRINGUP SUMMARY: passed=N failed=0` and the build-identity line.

DevKit: ordinary, the UART bridge handles reset:

    pio run -e <env> -t upload --upload-port COMx

Nano: **never `-t upload`.** PlatformIO routes it through Arduino DFU, which writes an application only
into an OTA slot and cannot place a bootloader or partition table; it reports success and the board does
not boot. Build, then write all three regions with esptool:

    0x0 bootloader.bin   0x8000 partitions.bin   0x10000 firmware.bin
    --chip esp32s3 --port COMx --before default_reset --after hard_reset
    write_flash -z --flash_mode dio --flash_freq 80m --flash_size 16MB

Three `Hash of data verified.` means the write worked, not that the board is running. Silence plus no
access point after a Nano flash means download mode: confirm with `--before no_reset chip_id`, and replug
the board if esptool connects without resetting anything. Never open the Nano's port with a naive serial
monitor: DTR is wired to GPIO0, and a monitor that asserts DTR on open reboots the board into DFU.

## Project constraints

- The shipped artifact has `n_mt_identified == false`, so the active layout is `TENSION_ABSENT` and
  **every tension row is excluded from the solve by policy**. The geometric correction comes from the
  runout rows (SPEC §8.4 Part 1).
- The honest ceiling in Stage 1 is `CONVERGED_GEOMETRIC_ONLY` (SPEC §8.6.3). Acoustic estimates are
  `suspect` / `PROVISIONAL_MODE_ID` and cannot support `CONVERGED`.
- `src/web_ui.h` is a C string literal full of `\n`. Edit it with an editor, never through a shell
  heredoc, which strips the backslashes and silently corrupts the page.
- Commits are atomic and conventional; the body says cause, evidence and limits, including what a change
  does *not* establish.
