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

## Flashing the Nano ESP32

The Nano needs a different route from the DevKit and the difference is not
cosmetic, so follow this rather than rediscovering it.

`pio run -t upload` on `nano_esp32` uses the Arduino DFU path, which writes an
**application only**, into an OTA slot — its own recipe caps the payload at
`0x300000`, one app partition. It cannot place a bootloader or a partition
table, so a merged image handed to DFU alternate 0 does not land at flash offset
`0x0` and the result does not boot, however cleanly the transfer reports success.

The working route is a raw esptool write over ROM download mode:

1. Put the board in ROM download mode by hand: jumper **B1 → GND**, press
   **RESET**, release, then remove the jumper. The LED goes solid purple.
2. Write all three regions:

```bash
esptool.py --chip esp32s3 --port COM5 --before default_reset --after hard_reset \
  write_flash -z --flash_mode dio --flash_freq 80m --flash_size 16MB \
  0x0 bootloader.bin 0x8000 partitions.bin 0x10000 firmware.bin
```

   (the three files are under `.pio/build/nano_esp32/`; `esptool` needs
   `intelhex` installed in the PlatformIO penv.)
3. **Physically unplug and replug the USB cable.** This step is mandatory and is
   the one that is easy to skip. Download mode entered through USB-Serial/JTAG is
   latched, and neither `--after hard_reset` nor the RESET button clears it; only
   a real power cycle does. Without it the board stays in download mode and looks
   like a failed flash.

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

## Interactive and self-play images

`s3_devkit` / `nano_esp32` are **interactive**: the machine sits in READY and does nothing
until an operator presses *Start truing* in the web UI, and every operator wait is answered
from the browser. This is the demonstration image.

`s3_devkit_selfplay` / `nano_esp32_selfplay` add `-DTRUING_SELF_PLAY=1`: the auto-operator
starts the session and answers its own waits, unattended. This is the on-target evidence
run recorded in `docs/BRINGUP_LOG.md`, and it is what to flash when reproducing those
numbers. It stands down while a browser is attached, so a person can still take over.

```bash
pio run -d %USERPROFILE%\truing_ws -e s3_devkit_selfplay -t upload   # unattended evidence
pio run -d %USERPROFILE%\truing_ws -e s3_devkit -t upload            # the demo image
```

The boot banner says which one is running.

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
