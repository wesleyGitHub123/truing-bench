"""Build, flash, and confirm boot - the board-specific parts of this done once, not re-derived.

The two boards need genuinely different flashing routes (docs/DEV_NOTES.md has the full story): the
DevKit's CH343 bridge lets PlatformIO's own upload handle reset, but the Nano's native
USB-Serial/JTAG means `-t upload` is actively wrong there - it routes through Arduino DFU,
which can write an application but not a bootloader or partition table, reports success, and
leaves the board not booting. That distinction is the one thing worth a script: everything
else here is just doing the documented steps in order and checking the one thing a
`Hash of data verified.` does NOT tell you - whether the board is actually running.

    python tools/flash.py <env> [--port COM5] [--seconds 45] [--no-build] [--verify-only]

<env> must start with `s3_devkit` or `nano_esp32` - that prefix is how the board (and
therefore the flashing route) is chosen; anything else is refused rather than guessed at.

Boot verification always resets the board first - each board's own documented mechanism, not
a fresh flash - so `--verify-only` checks a boot happening right now, not whatever the board
logged the last time it came up. It then reads the serial console (same DTR/RTS-safe handling
as serial_capture.py / nano_serial.py - never a naive open) and looks for the bring-up
subsystem's own pass/fail line, not a heuristic:

    BRINGUP SUMMARY: passed=N failed=0

`failed=0` is required; the line existing with a nonzero failure count is still reported as a
failure. If nothing appears in the window, the Nano route runs the ROM-bootloader probe
docs/DEV_NOTES.md documents (an `esptool --before no_reset chip_id` that only succeeds when the chip
never left the bootloader) to say WHY, rather than leaving "no output" unexplained.

Tool paths resolve from $USERPROFILE by default (this workspace's convention - PATH is not
reliable here) but every one of them can be overridden, so this isn't tied to one account:

    TRUING_PIO, TRUING_PYTHON, TRUING_ESPTOOL, TRUING_BUILD_DIR
"""
import argparse
import os
import shutil
import subprocess
import sys
import time

DEVKIT_MARKER = "s3_devkit"
NANO_MARKER = "nano_esp32"
DEFAULT_PORTS = {"devkit": "COM4", "nano": "COM5"}
DEFAULT_SECONDS = {"devkit": 25.0, "nano": 45.0}   # nano's bring-up (I2S + acoustic self-test) runs slower


def resolve_tool(env_var, which_name, fallback_relative):
    """env var > PATH (if `which_name` is checkable there) > the documented $USERPROFILE
    default. `which_name=None` skips the PATH lookup - for esptool.py, which is a script,
    not something PATH would ever resolve on its own."""
    if os.environ.get(env_var):
        return os.environ[env_var]
    if which_name:
        found = shutil.which(which_name)
        if found:
            return found
    home = os.environ.get("USERPROFILE") or os.path.expanduser("~")
    candidate = os.path.join(home, *fallback_relative)
    if os.path.exists(candidate):
        return candidate
    raise SystemExit("cannot find %s - set %s explicitly (looked on PATH and at %s)"
                     % (which_name or env_var, env_var, candidate))


def board_for_env(env):
    if env.startswith(DEVKIT_MARKER):
        return "devkit"
    if env.startswith(NANO_MARKER):
        return "nano"
    raise SystemExit("'%s' doesn't start with '%s' or '%s' - which board is this? "
                     "(see the Images table in docs/DEV_NOTES.md)" % (env, DEVKIT_MARKER, NANO_MARKER))


def run(cmd, **kw):
    print("+ %s" % " ".join(str(c) for c in cmd))
    return subprocess.run(cmd, **kw)


def build(pio, build_dir, env):
    r = run([pio, "run", "-d", build_dir, "-e", env])
    if r.returncode != 0:
        raise SystemExit("build failed (exit %d) - not flashing a stale or missing image" % r.returncode)


def flash_devkit(pio, build_dir, env, port):
    r = run([pio, "run", "-d", build_dir, "-e", env, "-t", "upload", "--upload-port", port])
    if r.returncode != 0:
        raise SystemExit("upload failed (exit %d)" % r.returncode)


def flash_nano(python, esptool, build_dir, env, port):
    out = os.path.join(build_dir, ".pio", "build", env)
    images = {
        "bootloader.bin": "0x0",
        "partitions.bin": "0x8000",
        "firmware.bin": "0x10000",
    }
    missing = [f for f in images if not os.path.exists(os.path.join(out, f))]
    if missing:
        raise SystemExit("missing build output in %s: %s (build first, or drop --no-build)" % (out, missing))
    cmd = [python, esptool, "--chip", "esp32s3", "--port", port,
           "--before", "default_reset", "--after", "hard_reset", "write_flash", "-z",
           "--flash_mode", "dio", "--flash_freq", "80m", "--flash_size", "16MB"]
    for name, addr in images.items():
        cmd += [addr, os.path.join(out, name)]
    r = run(cmd)
    if r.returncode != 0:
        raise SystemExit("esptool write_flash failed (exit %d) - see docs/DEV_NOTES.md's Nano flashing "
                         "notes before retrying blind" % r.returncode)


def nano_is_stuck_in_bootloader(python, esptool, port):
    """docs/DEV_NOTES.md's diagnostic: --before no_reset succeeding means the chip never left the ROM
    bootloader (it isn't running firmware at all), not that it's healthy."""
    r = subprocess.run([python, esptool, "--chip", "esp32s3", "--port", port,
                        "--before", "no_reset", "--after", "no_reset", "chip_id"],
                       capture_output=True, text=True, timeout=15)
    return r.returncode == 0


def reset_devkit(port):
    """The classic ESP auto-reset the CH343 bridge exposes: pulse EN (RTS) low with IO0 (DTR)
    released. Same sequence tools/serial_capture.py uses."""
    import serial
    with serial.Serial(port, 115200, timeout=0.2) as ser:
        ser.setDTR(False)
        ser.setRTS(True)
        time.sleep(0.1)
        ser.setRTS(False)


def reset_nano(python, esptool, port):
    """No GPIO line to pulse on the Nano's native USB-Serial/JTAG - use esptool's own
    reset-into-bootloader-then-hard-reset cycle (its default --before/--after) as a clean way
    to get back to a known boot, without writing anything to flash."""
    subprocess.run([python, esptool, "--chip", "esp32s3", "--port", port, "chip_id"],
                  capture_output=True, text=True, timeout=15)


def read_serial(port, seconds):
    import serial
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = 115200
    ser.timeout = 0.2
    ser.dtr = False   # set before open(): the Nano reboots into DFU if DTR is asserted on open,
    ser.rts = False   # and asserting either needlessly on the DevKit risks retriggering reset
    ser.open()
    chunks = []
    deadline = time.time() + seconds
    while time.time() < deadline:
        data = ser.read(4096)
        if data:
            chunks.append(data)
    ser.close()
    return b"".join(chunks).decode("utf-8", "replace")


def check_bringup(log):
    for line in log.splitlines():
        if "BRINGUP SUMMARY" in line:
            return line
    return None


def find_identity(log):
    """The line net_transport.c logs at startup: 'board X | firmware Y | build Z | ui W | mode
    V'. Distinct from ESP-IDF's own built-in app-version stamp, which is set at CMake
    configure time and can go stale across incremental builds without a reconfigure - it is
    NOT reliable evidence of what's running and this deliberately doesn't look at it."""
    for line in log.splitlines():
        if "board " in line and "| firmware " in line and "| build " in line:
            return line.strip()
    return None


def verify_boot(board, python, esptool, port, seconds):
    print("resetting %s to check its CURRENT boot, not whatever it logged earlier ..." % port)
    try:
        if board == "devkit":
            reset_devkit(port)
        else:
            reset_nano(python, esptool, port)
    except Exception as e:
        raise SystemExit("could not reset %s (%s) - is the board plugged in and the port free?" % (port, e))
    print("reading %s for up to %.0fs, watching for BRINGUP SUMMARY ..." % (port, seconds))
    try:
        log = read_serial(port, seconds)
    except Exception as e:
        raise SystemExit("could not open %s (%s) - is the board's USB cable plugged in, and "
                         "is nothing else (a monitor, another instance of this tool) already "
                         "holding the port open?" % (port, e))
    line = check_bringup(log)
    if line is not None:
        ok = ("failed=0" in line)
        identity = find_identity(log)
        if identity:
            print("      " + identity)
        elif ok:
            print("WARN  booted, but the board's own identity line wasn't seen in this window - "
                 "can't confirm which build this actually is (see /id once it's reachable)")
        print(("PASS  " if ok else "FAIL  ") + line.split("bringup:", 1)[-1].strip())
        if not ok:
            print("  the board booted and ran its self-test, but the self-test itself failed - "
                 "see the full log below, not a flashing problem")
        if log.strip():
            print("--- serial log " + "-" * 50)
            print(log.strip())
        return ok

    print("FAIL  no BRINGUP SUMMARY seen in %.0fs of silence-or-noise on %s" % (seconds, port))
    if log.strip():
        print("--- what was on the wire " + "-" * 40)
        print(log.strip())
    else:
        print("  (nothing at all was read - not even boot noise)")
    if board == "nano":
        print("checking whether the chip is parked in the ROM bootloader ...")
        try:
            stuck = nano_is_stuck_in_bootloader(python, esptool, port)
        except subprocess.TimeoutExpired:
            stuck = None
        if stuck:
            print("  CONFIRMED: esptool reached the chip without resetting it, so it never left "
                 "the ROM bootloader. This is the documented intermittent case - unplug and "
                 "replug the USB cable, then re-run with --verify-only.")
        elif stuck is False:
            print("  not in the ROM bootloader either - something else is wrong (power, cable, "
                 "or a firmware crash loop too fast to log). Do not reflash blind; capture serial "
                 "manually and look before trying again (SPEC halt: don't iterate blind on hardware).")
        else:
            print("  probe itself timed out - check the cable and COM port before anything else.")
    else:
        print("  check the USB cable and COM4, and that nothing else has the port open.")
    return False


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("env", help="PlatformIO environment, e.g. nano_esp32_fastdemo_mic or s3_devkit")
    p.add_argument("--port", help="default: COM4 for a DevKit env, COM5 for a Nano env")
    p.add_argument("--seconds", type=float, help="boot-verification window (default: 25 DevKit, 45 Nano)")
    p.add_argument("--no-build", action="store_true", help="flash the existing build output as-is")
    p.add_argument("--verify-only", action="store_true", help="skip build and flash; just check boot")
    p.add_argument("--build-dir", default=os.environ.get(
        "TRUING_BUILD_DIR", os.path.join(os.environ.get("USERPROFILE", ""), "truing_ws")))
    args = p.parse_args()

    board = board_for_env(args.env)
    port = args.port or DEFAULT_PORTS[board]
    seconds = args.seconds if args.seconds is not None else DEFAULT_SECONDS[board]

    pio = resolve_tool("TRUING_PIO", "pio", [".platformio", "penv", "Scripts", "pio.exe"])
    # Only the Nano route ever shells out to esptool directly - the DevKit goes through
    # `pio ... -t upload`, which resolves its own toolchain. Resolving these unconditionally
    # would fail on a machine that has pio but not a matching PlatformIO-managed esptool.
    python = esptool = None
    if board == "nano":
        # Deliberately not `which_name="python"`: whatever "python" resolves to on PATH may
        # not be the interpreter esptool's own pyserial dependency is installed into. The
        # PlatformIO-managed one, which every board/flashing note in docs/DEV_NOTES.md assumes, is
        # what TRUING_PYTHON should point at if this default is ever wrong.
        python = resolve_tool("TRUING_PYTHON", None, [".platformio", "penv", "Scripts", "python.exe"])
        esptool = resolve_tool("TRUING_ESPTOOL", None,
                               [".platformio", "packages", "tool-esptoolpy", "esptool.py"])

    if not args.verify_only:
        if not args.no_build:
            build(pio, args.build_dir, args.env)
        if board == "devkit":
            flash_devkit(pio, args.build_dir, args.env, port)
        else:
            flash_nano(python, esptool, args.build_dir, args.env, port)
        time.sleep(1.5)   # let the reset settle before the serial console is trusted

    ok = verify_boot(board, python, esptool, port, seconds)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
