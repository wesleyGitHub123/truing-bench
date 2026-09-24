"""Keyboard console for the roller bench sketch (tools/bench/roller_smoke) on the DevKit.

Each keypress goes straight to the board as one command (e/d enable/disable, f/b jog, F/B run,
x or space stop, 1..5 jog size, j exact jog size, r/R repeat test, k repeat count, +/- cruise,
]/[ accel, z zero, p position, s status, u registers, a node scan, m microsteps, i/I IRUN,
c clear GSTAT, ? help). The board's replies print here and are appended to a log file, together
with anything you note, so what you saw and what the board says it did end up side by side in one
timestamped record -- the raw material docs/ROLLER_BENCH.md entries cite.

Local keys, never sent to the board:
    n     type an observation note (Enter to finish), logged as  NOTE: ...
    Esc   quit (Ctrl+C also works)

The DevKit's CH343 bridge wires DTR/RTS to IO0/EN, so both are set low before open() -- the port
opens without resetting the board. A dropped port (board reset, replug) is reopened and marked.

DevKit only: COM4 by default. Never point this at COM5 -- that is the Nano and the acoustic
campaign's board.

Usage: python tools/bench/roller_ctl.py [log path] [port]   (Windows console; pyserial from the PlatformIO penv)
Default log: %TEMP%\\roller_ctl.log (appended).
"""
import msvcrt
import os
import sys
import threading
import time

import serial

log_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.environ.get("TEMP", "."), "roller_ctl.log")
PORT = sys.argv[2] if len(sys.argv) > 2 else "COM4"
if PORT.upper() == "COM5":
    sys.exit("COM5 is the Nano (acoustic campaign). The roller bench is the DevKit.")
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


stamp("=== roller_ctl session start on %s, log %s ===" % (PORT, log_path))
threading.Thread(target=reader, daemon=True).start()
emit("keys go to the board: e d f b F B x 1-5 j r R k + - ] [ z p s u a m i I c ?"
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
        if ch not in "\r\n0123456789":
            stamp("key: %s" % ch)
        try:
            ser.write(ch.encode("ascii", "ignore"))
        except serial.SerialException:
            stamp("(write failed, key %r dropped)" % ch)
except KeyboardInterrupt:
    pass
stop.set()
stamp("=== session end ===")
