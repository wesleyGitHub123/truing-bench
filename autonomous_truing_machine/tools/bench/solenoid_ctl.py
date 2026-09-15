"""Keyboard console for the solenoid bench sketch (tools/bench/solenoid_smoke) on the Nano.

Each keypress goes straight to the board as one command (l/r shot, L/R 500 ms hold, a alternating
run, +/- shot width, {/} push ramp, [/] release ramp, k/K brake duty, m/M brake ms, g/G catch
duty, t/T catch delay, w/W catch width, s status, c/u/d/x manual jog, ? help). The board's
replies print here and are appended to a log file, together with anything you note, so what you
saw and what the board says it did end up side by side in one timestamped record.

Local keys, never sent to the board:
    n     type an observation note (Enter to finish), logged as  NOTE: ...
    Esc   quit (Ctrl+C also works)

The Nano's DTR/RTS reach GPIO0/reset, so both are set low before open(), the same discipline as
tools/nano_serial.py. A dropped port (board reset, replug) is reopened and marked in the log.

Usage: python tools/bench/solenoid_ctl.py [log path]   (Windows console; pyserial from the PlatformIO penv)
Default log: %TEMP%\solenoid_ctl.log (appended).
"""
import msvcrt
import os
import sys
import threading
import time

import serial

PORT = "COM5"
log_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.environ.get("TEMP", "."), "solenoid_ctl.log")
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
emit("keys go to the board: l r L R a + - { } [ ] k K m M g G t T w W s c u d x ?"
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
