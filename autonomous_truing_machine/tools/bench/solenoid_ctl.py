"""Keyboard console for the solenoid bench sketch (tools/bench/solenoid_smoke), Nano or DevKit.

Each keypress goes straight to the board as one command (l/r shot, b/v baseline (raw) shot,
L/R 500 ms hold, a alternating run, A one-person V_DS check (lead-in + long holds), +/- shot
width, {/} push ramp, [/] release ramp, k/K brake duty, m/M brake ms, g/G catch duty, t/T catch
delay, w/W catch width, s status, c/u/d/x manual jog, ? help). The board's replies print here
and are appended to a log file, together with anything you note, so what you saw and what the
board says it did end up side by side in one timestamped record.

Local keys, never sent to the board:
    n     type an observation note (Enter to finish), logged as  NOTE: ...
    Esc   quit (Ctrl+C also works)

On the Nano, DTR/RTS reach GPIO0/reset, so both are set low before open(), the same discipline as
tools/nano_serial.py; this is a no-op on the DevKit's CH343 bridge (`pio device monitor` also works
there). A dropped port (board reset, replug) is reopened and marked in the log.

Usage: python tools/bench/solenoid_ctl.py [--port COMx] [log path]
Default port COM5 (the Nano). For the DevKit, `pio device list` first -- COM4 is the roller
board's, not necessarily this one's -- then pass its actual port explicitly.
Default log: %TEMP%\solenoid_ctl.log (appended).
"""
import msvcrt
import os
import sys
import threading
import time

import serial

args = sys.argv[1:]
PORT = "COM5"
if args and args[0] == "--port":
    PORT = args[1]
    args = args[2:]
log_path = args[0] if args else os.path.join(os.environ.get("TEMP", "."), "solenoid_ctl.log")
log = open(log_path, "a", encoding="utf-8", buffering=1)
lock = threading.Lock()
stop = threading.Event()
ser = None


def emit(text):
    with lock:
        sys.stdout.write(text)
        sys.stdout.flush()
        log.write(text)


def stamp(msg):
    emit("[%s] %s\n" % (time.strftime("%H:%M:%S"), msg))


def open_port():
    s = serial.Serial()
    s.port = PORT
    s.baudrate = 115200
    s.timeout = 0.2
    s.dtr = False
    s.rts = False
    s.open()
    return s


def reader():
    global ser
    down = False
    buf = b""
    while not stop.is_set():
        if ser is None:
            try:
                ser = open_port()
                if down:
                    stamp("--- port reopened ---")
                down = False
            except serial.SerialException:
                if not down:
                    stamp("--- port unavailable, retrying ---")
                    down = True
                time.sleep(0.5)
                continue
        try:
            data = ser.read(256)
        except serial.SerialException:
            try:
                ser.close()
            except Exception:
                pass
            ser = None
            continue
        buf += data
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            stamp("board: " + line.decode("utf-8", "replace").rstrip("\r"))


stamp("=== solenoid_ctl session start, log %s ===" % log_path)
threading.Thread(target=reader, daemon=True).start()
emit("keys go to the board: l r b v L R a A + - { } [ ] k K m M g G t T w W s c u d x ?"
     "   |   n = note, Esc = quit\n")
try:
    while True:
        ch = msvcrt.getwch()
        if ch == "\x1b":
            break
        if ch in ("\x00", "\xe0"):
            msvcrt.getwch()
            continue
        if ch == "n":
            with lock:
                sys.stdout.write("note> ")
                sys.stdout.flush()
            note = input()
            stamp("NOTE: " + note)
            continue
        if ser is None:
            stamp("(not connected, key %r dropped)" % ch)
            continue
        stamp("key: %s" % ch)
        try:
            ser.write(ch.encode("ascii", "ignore"))
        except serial.SerialException:
            stamp("(write failed, key %r dropped)" % ch)
except KeyboardInterrupt:
    pass
stop.set()
stamp("=== session end ===")
