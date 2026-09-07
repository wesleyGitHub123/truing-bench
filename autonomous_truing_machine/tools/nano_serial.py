"""Read the Nano's console without knocking it into download mode.

tools/serial_capture.py resets the DevKit by asserting RTS through the CH343 bridge - the
classic ESP auto-reset lines. That does NOT apply here: the Nano's native USB-Serial/JTAG wires
DTR straight to GPIO0, so a monitor that asserts DTR (or RTS) on open reboots the chip into the
ROM bootloader instead of showing its console (see the Nano flashing notes in docs/DEV_NOTES.md). The
two boards need opposite handling, which is why this is a second small script rather than a
shared one with a board flag.

pyserial applies dtr/rts as the INITIAL line state, set before open() - the only order that
doesn't touch the pins on the way in.

Usage: python tools/nano_serial.py [seconds] [out.txt]   (pyserial; the PlatformIO penv has it)
"""
import sys
import time

import serial

seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 15.0
out_path = sys.argv[2] if len(sys.argv) > 2 else None

ser = serial.Serial()
ser.port = "COM5"
ser.baudrate = 115200
ser.timeout = 0.2
ser.dtr = False
ser.rts = False
ser.open()

chunks = []
deadline = time.time() + seconds
while time.time() < deadline:
    data = ser.read(4096)
    if data:
        chunks.append(data)
ser.close()

text = b"".join(chunks).decode("utf-8", errors="replace")
if out_path:
    with open(out_path, "w", encoding="utf-8") as f:
        f.write(text)
sys.stdout.write(text if text else "(silence: 0 bytes in %.0f s)\n" % seconds)
