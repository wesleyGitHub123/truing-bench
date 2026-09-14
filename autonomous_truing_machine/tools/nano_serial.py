"""Read the Nano's console without knocking it into download mode.

tools/serial_capture.py resets the DevKit by asserting RTS through the CH343 bridge - the
classic ESP auto-reset lines. That does NOT apply here: the Nano's native USB-Serial/JTAG wires
DTR straight to GPIO0, so a monitor that asserts DTR (or RTS) on open reboots the chip into the
ROM bootloader instead of showing its console (see the Nano flashing notes in docs/DEV_NOTES.md). The
two boards need opposite handling, which is why this is a second small script rather than a
shared one with a board flag.

pyserial applies dtr/rts as the INITIAL line state, set before open() - the only order that
doesn't touch the pins on the way in.

The port also disappears whenever the board resets or is replugged: USB-Serial/JTAG
re-enumerates, and a read on the dead handle raises ClearCommError. A capture meant to catch a
boot log has to span exactly that gap, so a dropped port is reopened (same line discipline)
until the deadline instead of ending the capture. Each drop and reopen is marked in the output.

Usage: python tools/nano_serial.py [seconds] [out.txt]   (pyserial; the PlatformIO penv has it)
"""
import sys
import time

import serial

seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 15.0
out_path = sys.argv[2] if len(sys.argv) > 2 else None

chunks = []
deadline = time.time() + seconds
ser = None
state = None


def mark(msg):
    line = "\n--- [%s] %s ---\n" % (time.strftime("%H:%M:%S"), msg)
    chunks.append(line.encode("utf-8"))
    sys.stdout.write(line)
    sys.stdout.flush()


def open_port():
    s = serial.Serial()
    s.port = "COM5"
    s.baudrate = 115200
    s.timeout = 0.2
    s.dtr = False
    s.rts = False
    s.open()
    return s


while time.time() < deadline:
    if ser is None:
        try:
            ser = open_port()
            if state != "up":
                if state is not None:
                    mark("port reopened")
                state = "up"
        except serial.SerialException as e:
            if state != "down":
                mark("port unavailable (%s), waiting" % e)
                state = "down"
            time.sleep(0.3)
            continue
    try:
        data = ser.read(4096)
    except serial.SerialException as e:
        mark("port dropped (%s), reconnecting" % e)
        try:
            ser.close()
        except serial.SerialException:
            pass
        ser = None
        state = "down"
        time.sleep(0.3)
        continue
    if data:
        chunks.append(data)
        sys.stdout.write(data.decode("utf-8", errors="replace"))
        sys.stdout.flush()

if ser is not None:
    ser.close()

text = b"".join(chunks).decode("utf-8", errors="replace")
if out_path:
    with open(out_path, "w", encoding="utf-8") as f:
        f.write(text)
if not text:
    sys.stdout.write("(silence: 0 bytes in %.0f s)\n" % seconds)
