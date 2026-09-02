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
`COM4` (CH343). The Arduino Nano ESP32 target builds but has not been flashed.

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

## Host model preparation (Phase 1b, `../model_prep`)

```bash
cd model_prep
python -m venv .venv
.venv\Scripts\python -m pip install -r requirements.txt     # numpy, scipy, pytest, bike-wheel-calc @ 6fc380c
.venv\Scripts\python -m pytest -q                            # the SPEC 14.3 host gate
.venv\Scripts\python -m truing_model_prep.cli generate-fixture --out golden/fixture_sym32_artifact.json
.venv\Scripts\python -m truing_model_prep.cli parity-fixtures --artifact golden/fixture_sym32_artifact.json --out golden/parity_sym32.json
```

`bike-wheel-calc` is fetched from GitHub at the pinned commit; the install
needs network access once. `git clone` of that repository into a very long path
fails on Windows with "Filename too long"; clone with `-c core.longpaths=true`
or into a short path if you want its examples.

## Notes

- The first ESP-IDF build downloads the framework and toolchains (about 1.5 GB)
  and creates the IDF Python virtual environment under
  `~/.platformio/penv/.espidf-5.4.0`. On this PC the `ensurepip` step of that
  environment failed once and succeeded on retry; if a build stops with
  "Missing the `pip` binary", simply run the build again.
- `sdkconfig.<env>` files are generated from `sdkconfig.defaults` and are not
  committed. Change `sdkconfig.defaults`, not the generated file.
- `.pio/` (build output) is inside the OneDrive-synced tree; it is ignored by
  Git but OneDrive will still sync it.
