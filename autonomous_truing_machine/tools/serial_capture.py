"""Reset the ESP32-S3-DevKitC-1 through the CH343 auto-reset lines and capture its console for N seconds.

Usage: python tools/serial_capture.py COM4 20 capture.txt   (pyserial; the PlatformIO penv has it)
"""
import sys
import time

import serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM4"
seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 20.0
out_path = sys.argv[3] if len(sys.argv) > 3 else None

with serial.Serial(port, 115200, timeout=0.2) as ser:
    # Classic ESP auto-reset: hold EN (RTS) low briefly with IO0 (DTR) released.
    ser.setDTR(False)
    ser.setRTS(True)
    time.sleep(0.1)
    ser.setRTS(False)
    deadline = time.time() + seconds
    chunks = []
    while time.time() < deadline:
        data = ser.read(4096)
        if data:
            chunks.append(data)
text = b"".join(chunks).decode("utf-8", errors="replace")
if out_path:
    with open(out_path, "w", encoding="utf-8") as f:
        f.write(text)
sys.stdout.write(text)
