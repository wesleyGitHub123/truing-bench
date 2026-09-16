# Building, testing and flashing

The firmware project is the PlatformIO project in `autonomous_truing_machine/`.
The specification is `docs/SPEC.md` in the
same directory and is the implementation authority.

## Toolchain (as verified on the development PC)

| Component | Version |
|---|---|
| PlatformIO Core | 6.1.19 |
| Platform | `espressif32@6.10.0` (pinned in `platformio.ini`) |
| Framework | ESP-IDF 5.4.0 (`framework-espidf@3.50400.0`), toolchain `xtensa-esp-elf` 14.2.0 |
| Host tests | PlatformIO `native` platform; MinGW GCC 6.3 (on `PATH`) works, MSYS2 UCRT64 GCC 15 also present |

The development board is an ESP32-S3-DevKitC-1 (N16R8) on the UART bridge port
`COM4` (CH343). The Arduino Nano ESP32 runs the same firmware from its native
USB-Serial/JTAG port (`COM5`); see "Flashing the Nano ESP32" below.

## Path with spaces: build through a junction

PlatformIO's ESP-IDF integration refuses build paths that contain spaces, and
this project lives under `College Files - 4th Year\…`. Host tests are not
affected. For ESP-IDF builds, create a space-free directory junction once:

```powershell
New-Item -ItemType Junction -Path "%USERPROFILE%\truing_ws" `
    -Target "%USERPROFILE%\OneDrive\Desktop\College Files\College Files - 4th Year\1st Term\Stage 1\Workspace\Truing Repo\autonomous_truing_machine"
```

and build with `-d %USERPROFILE%\truing_ws`. The junction is a view of the same
files; edits made in either location are the same edits. Remove it with
`Remove-Item %USERPROFILE%\truing_ws` (this removes only the junction).

If a build fails during CMake configuration with "Include directory
`…/.pio/build/<env>/config` is not a directory", delete
`.pio/build/<env>` and build again; a build directory created by an earlier
failed run under the other path is stale.

## Commands

```bash
# Host-side unit tests (framework-free core, HAL contracts, simulations)
pio test -e native

# Firmware, development board (must always build)
pio run -d %USERPROFILE%\truing_ws -e s3_devkit

# Firmware, final form-factor board (must always build)
pio run -d %USERPROFILE%\truing_ws -e nano_esp32

# Flash and monitor the development board
pio run -d %USERPROFILE%\truing_ws -e s3_devkit -t upload
pio device monitor -p COM4 -b 115200

# One-off: provision the SYNTHETIC fixture configuration into NVS to exercise
# persistence (SPEC 11.5). Never a demonstration configuration.
pio run -d %USERPROFILE%\truing_ws -e s3_devkit_provision -t upload
```

## How much to verify, and when

Build the cheapest thing that covers what changed. Measured costs on the development PC:
the native suite is **45 s**, `ui_check` is **3 s**, one warm image is **5–100 s**, and a
sweep of all ten release images is **8–11 minutes**.

The full sweep is a **release gate, not a commit gate**. It earns its cost when the board
profile changes, when `platformio.ini` changes, or before tagging — because those are the
only things that vary across the matrix. The eleven ESP images differ along exactly two
axes, the board-profile header and the `src/build_mode.h` flags, so a change touching
neither is proven by one build per board.

A one-file change does not earn a ten-image sweep. When it is unclear which images are
affected, build the five flag variants on one board plus one build on the other; escalate
to the whole matrix only for the reasons above.

The full tier ladder, including the three-second flag-matrix syntax check that catches an
illegal `build_mode.h` combination without building anything, is in `DEV_NOTES.md`. It
is kept there rather than duplicated here so there is one operational source.

## Flashing the Nano ESP32

The Nano needs a different route from the DevKit and the difference is not
cosmetic, so follow this rather than rediscovering it.

`pio run -t upload` on `nano_esp32` uses the Arduino DFU path, which writes an
**application only**, into an OTA slot — its own recipe caps the payload at
`0x300000`, one app partition. It cannot place a bootloader or a partition
table, so a merged image handed to DFU alternate 0 does not land at flash offset
`0x0` and the result does not boot, however cleanly the transfer reports success.

The working route is a raw esptool write over ROM download mode:

1. **No jumper is needed.** esptool enters download mode by itself over the native
   USB-Serial/JTAG with `--before default_reset`; this has been verified repeatedly on
   2026-09-05. Keep the **B1 → GND** + **RESET** jumper procedure for the genuine
   emergency: an image so broken the chip cannot get far enough to present its USB device.
2. Write all three regions:

```bash
esptool.py --chip esp32s3 --port COM5 --before default_reset --after hard_reset \
  write_flash -z --flash_mode dio --flash_freq 80m --flash_size 16MB \
  0x0 bootloader.bin 0x8000 partitions.bin 0x10000 firmware.bin
```

   (the three files are under `.pio/build/nano_esp32/`; `esptool` needs
   `intelhex` installed in the PlatformIO penv.)
3. **Check that it booted, and replug if it did not.** Download mode entered through
   USB-Serial/JTAG can latch, and when it does neither `--after hard_reset` nor the RESET
   button clears it — only a real power cycle. This is **intermittent**: most flashes boot
   straight into the new image, and some do not, so verify rather than assume either way.

   Three `Hash of data verified.` lines mean the write succeeded, **not** that the board is
   running. Read `COM5` afterwards: output means it booted; silence, with no AP on the air,
   means download mode. Confirm with `--before no_reset chip_id` — if esptool connects
   without resetting anything, the chip is parked in the ROM bootloader. Unplug and replug.

Then the console is on `COM5` at 115200 and the board runs the same image as the
DevKit.

**Opening the port matters on this board.** The USB-Serial/JTAG peripheral
emulates the modem lines, and **DTR is wired to GPIO0**. Clearing DTR pulls GPIO0
low and the chip reboots into ROM download mode instead of running the app — a
monitor that de-asserts DTR on open will appear to have bricked the board. Keep
DTR asserted and reset with RTS (EN) only. `pio device monitor` is fine; a script
using pyserial should set `dtr = True` before opening.

Recovery, if the board ends up in an unknown state: Arduino IDE's *Burn
Bootloader* restores the stock Arduino bootloader over the same B1 → GND route,
after which DFU works again.

## Two boards, one address: how to know which one you are looking at

Both boards run the same firmware and both serve the UI on **`http://192.168.4.1/`**. Only
the SSID differs, and a laptop or phone that loses one access point will silently rejoin
the other, at the same address, with no visible sign that it moved. If the two boards are
carrying different builds — which happens the moment you flash one and not the other — the
result looks exactly like a stale page or a failed flash. It is neither.

So every image carries its own identity:

```
I (989) net:    board esp32-s3-devkitc-1-n16r8 | firmware 0.1.0-phase1f | build 55c92cc | ui 5e9be3b3
```

- printed at boot,
- served as JSON at **`http://192.168.4.1/id`**,
- shown in the page header and under Diagnostics.

`build` is the git short hash, suffixed `-dirty` when the tree does not match it; `ui` is
`sha256(src/web_ui.h)` truncated to 8 hex, printed by the build too:

```
build identity: rev 55c92cc | ui 5e9be3b3 | env s3_devkit
```

**If the `ui` in the page does not match the `ui` the build printed, you are not looking at
the firmware you just flashed** — most likely you are on the other board. Check the SSID.

The UI is served with `Cache-Control: no-store, must-revalidate`, so a browser cannot be
the cause of a stale page. That is deliberate: the page is 32 KB off local flash over an
otherwise idle link, and caching it buys nothing against the cost of ever doubting what
the board is running.

## The images

`s3_devkit` / `nano_esp32` are **interactive**: the machine sits in READY and does nothing
until an operator presses *Start truing* in the web UI, and every operator wait is answered
from the browser. This is the demonstration image.

`s3_devkit_selfplay` / `nano_esp32_selfplay` add `-DTRUING_SELF_PLAY=1`: the auto-operator
starts the session and answers its own waits, unattended. This is the on-target evidence
run recorded in `docs/BRINGUP_LOG.md`, and it is what to flash when reproducing those
numbers. It stands down while a browser is attached, so a person can still take over.

`s3_devkit_fastdemo` / `nano_esp32_fastdemo` add `-DTRUING_FAST_DEMO=1`: an **accelerated
demonstration**. A person still presses *Start Fast Demo* and still applies every
adjustment, but the repetitive acquisition can run by itself, so the solver, adjustment and
verification stages are reachable in a couple of minutes instead of sixty-four operator
confirmations.

This image carries **both** acquisition paths and lets the operator pick between them in the
UI, per session — the manual path is not removed, it is the other choice:

- **Automatic (synthetic)** — the machine measures all 32 spokes and the whole rim unattended.
- **Manual (as on the machine)** — the machine stops and asks you to position the wheel and
  read the dial gauges, exactly as the physical path does. 64 answers per cycle.

Choosing a path re-initialises the machine so it re-establishes the spoke-0 reference through
the newly selected navigation implementation, and the next session is admitted against what
actually ran. It is refused while a session is active, and applied by the orchestrator's own
task between steps, so no session can change character underneath itself.

It does that by **swapping implementations, not by answering waits**, and the difference
matters:

| | navigation | runout | provenance |
|---|---|---|---|
| interactive / self-play | `navigation_manual` | `runout_manual` | `TRUING_SOURCE_REAL` |
| fast demo, automatic path | `navigation_synthetic` | `runout_synthetic` | `TRUING_SOURCE_SYNTHETIC` |
| fast demo, manual path | `navigation_manual` | `runout_manual` | `TRUING_SOURCE_REAL` |
| acoustic demonstration | `navigation_synthetic` | `runout_synthetic` | `TRUING_SOURCE_SYNTHETIC` |

The manual implementations are REAL because a person turns a real wheel and reads real dial
gauges. Having the firmware answer their waits with numbers it invented would record
`runout_manual` / REAL against measurements no gauge produced — the session would misreport
its own inputs. Substituting the implementation instead means the orchestrator asks the same
interfaces it always asks and gets an honest answer, `contains_non_real_implementations` is
true from session admission onward, and no orchestrator state is skipped: `POSITION`,
`READ_RUNOUT` and `MEASURE_SPOKE_TENSION` are entered exactly as before.

The choice is made before `truing_orch_init()`, so it cannot change mid-session, and Fast
Demo adds no wire command — there is nothing for the interactive firmware to refuse, because
nothing was added to ask it. See `src/build_mode.h`.

`s3_devkit_mic` / `nano_esp32_mic` add `-DTRUING_REAL_FRONT_END=1`: the interactive image
with the **physical INMP441 front end**. The microphone is opened once at boot and drained
continuously by its own core-1 task into a PSRAM ring; each spoke measurement fires the
solenoid at that spoke's own acoustic station (spoke 0 → LEFT, alternating), then captures one
bounded window (~1.2 s) of real audio and runs the existing DSP on it. **There is no hand-pluck
path.** A station whose solenoid the board profile does not declare
(`BOARD_PLUCK_ACTUATOR_LEFT/RIGHT_PRESENT`) makes the acoustic subsystem not ready, so
**`START_TRUING` is refused with `EXCITATION_UNAVAILABLE`**. Both boards ship with both flags 0,
so today every `*_mic` and `*_fastdemo_mic` image refuses sessions until a station's solenoid
passes its bench check and the flag is set.

A failed fire is a rejected attempt with the same reason, never an invitation to pluck. The
page shows the measurement as three phases, driven by firmware events and never by a browser
timer: **Striking spoke N — left/right actuator** with a draining capture bar → **Window
closed** → **Analysing**. A retry re-opens the window and the card says *Attempt 2*, which is
the orchestrator's bounded retry (SPEC §7.4) made visible — it changes no state, so nothing
else reports it. The
frames are best-effort: lose them and the ordinary measuring card is what shows, and the
measurement is identical. See `docs/IMPLEMENTATION_NOTES.md`, "The acoustic measurement
lifecycle". Every estimate is still `suspect`
/ `PROVISIONAL_MODE_ID`: a real front end means the samples are real, not that mode
identification or tension accuracy is validated. It is exclusive with self-play — the
auto-operator cannot rotate the wheel, so a solenoid would strike an unmoved spoke under the
wrong index — and `GET /id` reports it as `"mode":"interactive+inmp441"`.

### The acoustic demonstration (`*_fastdemo_mic`)

`s3_devkit_fastdemo_mic` / `nano_esp32_fastdemo_mic` set **both** `-DTRUING_FAST_DEMO=1` and
`-DTRUING_REAL_FRONT_END=1`: the real microphone with the synthetic acquisition path, and a
**bound on how many spokes are struck** (`TRUING_ACOUSTIC_DEMO_SPOKES`, 3). The station
solenoids strike a few spokes to show that the INMP441 → DSP path works; the remaining tension
rows are left uncollected; the runout sweep is untouched and the real solver runs on it. Like
every real-front-end image it refuses sessions until both stations' solenoids are declared.
Its synthetic navigation also never moves the wheel, so its three "spokes" are one physical
spoke. Reconciling that is plan gate G-demo, before it is a demonstration of more than one
spoke.

This is the one combination that needs an argument, because real tension from the wheel in
front of you and synthetic runout from a simulated rim describe two different objects, and a
row vector built from both would be a wheel that does not exist. It is contained by one fact:
**the tension channel is excluded from the solve by policy.** The shipped artifact has
`n_mt_identified == false` (asserted by the bring-up self-test in `src/bringup_artifact.c`),
so `art_select_layout()` returns `TENSION_ABSENT` and admission masks every tension row out
before the solver sees it. Rule R1 then compares the active row set against a layout mask that
holds no tension rows at all, so **how many spokes were struck cannot change whether the
state is admissible.** The real strikes are displayed evidence, never solver input.

That condition is *checked, not assumed*. `tension_targets_this_pass()` asks the calculation
which layout it would select — `select_layout` is a pure query — and applies the bound only
while the answer is `TENSION_ABSENT`. If the artifact ever identifies its common mode the
layout becomes `FULL`, tension rows become load-bearing, the bound stops applying and every
spoke is measured again. The failure direction is "it measured everything", which is never a
dishonest one. `test_acoustic_demo_bound_is_refused_when_tension_is_in_the_layout` pins it.

The bound is a dependency (`truing_orch_deps_t.tension_sample_limit`), **0 in every other
image**, so the machine and the interactive builds are unchanged and still collect every
spoke even under `TENSION_ABSENT` — those measurements are the evidence that the acquisition
chain works, and the mode-ID research still needs them.

The record says what it holds and why: provenance carries `tension_sample_limit`,
`tension_sampled` and `tension_omitted_by_layout`, and the page prints

> **Acoustic demonstration: 3 spokes sampled.** Remaining tension measurements omitted because
> tension is not part of the active solver layout (TENSION_ABSENT), so no adjustment depends on
> them. Runout is synthetic in this Fast Demo session.

so three tension records on a 32-spoke wheel can never be read as a wheel that was measured
and mostly failed.

### The campaign bench variant (`nano_esp32_fastdemo_mic_campaign`)

Same wiring as `nano_esp32_fastdemo_mic` above, plus `-DTRUING_CAMPAIGN_DEBUG=1`. This is the
only environment where `deps.debug_channel_enabled` is ever set true, which is what makes
`TRUING_INTENT_DEBUG` (SPEC §12.5) admissible at all — everywhere else, including the demo
image itself, it rejects with `REJECT_DEBUG_DISABLED` regardless of anything a client sends.
Admission is still further restricted to `TRUING_STATE_READY` with no session running, so
`MEASURE_ONCE` (the one debug code currently defined, `docs/SOLENOID_CAMPAIGN_PLAN.md` item
10) can fire a single station capture for bench characterization, but can never be reached
once a real session has started. `GET /id` reports this image as
`"mode":"fastdemo+inmp441+campaign"`. Nano-only: campaign bench work runs against the board
the INMP441 is actually wired to (`docs/SOLENOID_CAMPAIGN.md`); there is no DevKit variant.

```bash
pio run -d %USERPROFILE%\truing_ws -e s3_devkit_selfplay -t upload   # unattended evidence
pio run -d %USERPROFILE%\truing_ws -e s3_devkit_fastdemo -t upload   # accelerated demonstration
pio run -d %USERPROFILE%\truing_ws -e s3_devkit -t upload            # the physical-path image
pio run -d %USERPROFILE%\truing_ws -e nano_esp32_mic -t upload        # the physical INMP441 front end
pio run -d %USERPROFILE%\truing_ws -e nano_esp32_fastdemo_mic -t upload   # the acoustic demonstration
pio run -d %USERPROFILE%\truing_ws -e nano_esp32_fastdemo_mic_campaign -t upload   # campaign bench variant
```

The boot banner says which one is running, and so does `GET /id`:

```json
{"board":"esp32-s3-devkitc-1-n16r8","build":"45a6d2c","ui":"f9e59677","mode":"fastdemo", ...}
```

A fast-demo board says so three ways — the boot log, `/id`, and a banner across the top of
the page reading **FAST DEMO / Synthetic measurement mode / It is not a physical
wheel-truing result** — and the Start button is labelled *Start Fast Demo*. Diagnostics
shows `build mode` for all three.

## Checking the UI without a board

`tools/ui_check.js` extracts the page from `src/web_ui.h` and runs its script against a
stub DOM, a virtual clock and real firmware frames. Nothing else in the build executes that
script, so this is what stops a typo in the operator card from being found in front of an
audience.

```bash
node tools/ui_check.js                  # against src/web_ui.h
node tools/ui_check.js served.html      # against a page fetched from a board
```

## Reaching the web UI (SPEC 12.1)

The board hosts its own network; there is nothing to install on the host. At
boot it prints the credentials, which are derived from that board's MAC and are
therefore different on every unit and not stored in this repository:

```
net:  SPEC 12.1 transport up. Join the network and browse to:
net:    SSID       truing-xxxxxx
net:    passphrase truing-xxxxxxxx
net:    URL        http://192.168.4.1/
```

Join that network and open the URL. The page opens a websocket to `/ws`, asks
for the current state, and mirrors the machine from there.

While a browser is attached the workflow self-play stops answering its own
operator waits and the person at the browser answers them instead. With nobody
attached the self-play behaves as it always has, so the on-target demonstration
does not depend on a host being present.

Debug output stays on the serial monitor, which on the DevKitC-1 is a separate
physical port from the UI transport (SPEC 12.1 — a property of this development
board, not an architectural guarantee).

## Changing sdkconfig.defaults

Editing a value in `sdkconfig.defaults` does **not** change an environment that
has been built before. ESP-IDF's kconfig treats an existing `sdkconfig.<env>` as
authoritative for every symbol already in it and applies the defaults file only
to symbols it does not yet carry. PlatformIO does notice that the defaults file
is newer and reconfigures, which makes the change look as though it landed, but
the generated value still wins. Measured on this toolchain: with
`CONFIG_ESP_TASK_WDT_TIMEOUT_S=17` in the defaults and a full reconfigure, the
generated file still read `20`.

Because the generated files are gitignored, each machine's copies are whatever
it last happened to produce, and two boards can be built from what looks like one
configuration and not be. That is exactly what happened during the dual-board
comparison: the Nano built with a 5 s task watchdog while the DevKit had 20 s.

So after editing `sdkconfig.defaults`, delete the generated files:

```powershell
Remove-Item sdkconfig.s3_devkit, sdkconfig.nano_esp32
```

`tools/check_sdkconfig.py` runs before every ESP-IDF build and fails it if a
generated file contradicts the defaults, naming the symbol and both values. It
is a guard, not a substitute: it reports the drift, and deleting the generated
file is still the fix. A symbol the generated file does not mention at all is not
reported, since kconfig legitimately drops symbols whose dependencies are unmet.

There is no per-environment defaults mechanism to reach for instead. PlatformIO
passes only `-DSDKCONFIG=<path>` to ESP-IDF and never sets `SDKCONFIG_DEFAULTS`,
so both environments read the one shared `sdkconfig.defaults`. A board manifest
may set `build.esp-idf.sdkconfig_path`, but that relocates the *generated* file,
not the defaults. Per-board configuration differences would therefore need a
custom pre-script, which the project does not currently need (see
`docs/IMPLEMENTATION_NOTES.md` on the console/USB question).

## Host model preparation (Phase 1b, `../model_prep`)

```bash
cd model_prep
python -m venv .venv
.venv\Scripts\python -m pip install -r requirements.txt     # numpy, scipy, pytest, bike-wheel-calc @ 6fc380c
.venv\Scripts\python -m pytest -q                            # the SPEC 14.3 host gate
.venv\Scripts\python -m truing_model_prep.cli generate-fixture --out golden/fixture_sym32_artifact.json
.venv\Scripts\python -m truing_model_prep.cli parity-fixtures --artifact golden/fixture_sym32_artifact.json --out golden/parity_sym32.json
# Phase 1c: regenerate the compiled-in golden artifact blob and the parity cases for the firmware tests
.venv\Scripts\python -m truing_model_prep.cli export-c-fixtures --artifact golden/fixture_sym32_artifact.json --parity golden/parity_sym32.json --artifact-c ../autonomous_truing_machine/lib/truing_fixtures/src/fixture_sym32_artifact.c --parity-h ../autonomous_truing_machine/lib/truing_fixtures/include/truing_fixtures/fixture_sym32_parity.h --id 1
```

The two generated files are committed; regenerate them only when the golden
artifact or parity cases change, and commit the regeneration together with
the golden JSON that produced it.

`bike-wheel-calc` is fetched from GitHub at the pinned commit; the install
needs network access once. `git clone` of that repository into a very long path
fails on Windows with "Filename too long"; clone with `-c core.longpaths=true`
or into a short path if you want its examples.

## Host acoustic fixtures (Phase 1f, `../acoustic_prep`)

The C port of acoustic layers 2–4 is verified against the research
repository's Python pipeline on recorded campaign excerpts (SPEC §14.2). The
generator imports that repository read-only and writes the excerpts, the
reference values and the C fixtures:

```bash
# needs numpy, scipy and pyyaml; the system Python 3.12 on this PC has them
python acoustic_prep/make_golden.py --acoustic-repo "../Acoustic Repo" --out-dir autonomous_truing_machine
python -m pytest -q acoustic_prep/tests    # the committed fixtures still reproduce the committed values
```

Regenerate only when the excerpt selection or a DSP parameter changes, and
commit the regenerated `.pcm` files, `acoustic_golden.json`,
`acoustic_golden.h` and `fixture_acoustic_pcm.c` together.

The excerpts are compiled into the firmware as C arrays rather than embedded
as binaries. None of the binary-embedding routes works here: PlatformIO reads
CMake for sources and flags but builds with SCons, so the assembly file that
`EMBED_FILES` and `target_add_binary_data` generate through a CMake custom
command is never produced, and `board_build.embed_files` is ignored when the
project supplies its own `src/CMakeLists.txt`. The arrays cost about 550 KB
of flash, which the 4 MB factory partition absorbs comfortably.

## Notes

- The first ESP-IDF build downloads the framework and toolchains (about 1.5 GB)
  and creates the IDF Python virtual environment under
  `~/.platformio/penv/.espidf-5.4.0`. On this PC the `ensurepip` step of that
  environment failed once and succeeded on retry; if a build stops with
  "Missing the `pip` binary", simply run the build again.
- `sdkconfig.<env>` files are generated from `sdkconfig.defaults` and are not
  committed. Change `sdkconfig.defaults`, not the generated file — **and then
  delete the generated file**, because changing the defaults is not enough on
  its own. See below.
- `.pio/` (build output) is inside the OneDrive-synced tree; it is ignored by
  Git but OneDrive will still sync it.
