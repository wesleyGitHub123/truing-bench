"""A minimal WebSocket client for the board's /ws endpoint - stdlib only, no npm/ws needed.

This is one class doing what half a dozen throwaway scripts each did ad hoc during
development: open /ws, send a command, print what comes back. Import `WS` from here instead
of writing another one - it's what socket_pressure.py builds its multi-client test on, and
what this file's own CLI (below) uses to drive or watch a session interactively.

    python tools/probe/ws_client.py [seconds] [--host IP] [--start] [--auto]

--start issues START_TRUING once connected. --auto answers positioning and adjustment waits
so the workflow keeps moving on its own. Acoustic phase events (the windows a person has to
pluck into) are marked with `**` instead of `<-`.
"""
import argparse
import base64
import json
import os
import socket
import struct
import sys
import time


class WS:
    """One /ws connection: handshake in __init__, then send_text()/recv()."""

    def __init__(self, host, port=80, path="/ws", timeout=1.0):
        self.sock = socket.create_connection((host, port), timeout=5.0)
        self.sock.settimeout(timeout)
        key = base64.b64encode(os.urandom(16)).decode()
        req = ("GET %s HTTP/1.1\r\nHost: %s:%d\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
               "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n" % (path, host, port, key))
        self.sock.sendall(req.encode())
        self.buf = b""
        while b"\r\n\r\n" not in self.buf:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise SystemExit("board closed the connection during the handshake")
            self.buf += chunk
        head, self.buf = self.buf.split(b"\r\n\r\n", 1)
        if b"101" not in head.split(b"\r\n")[0]:
            raise SystemExit("handshake refused: %s" % head.split(b"\r\n")[0])

    def send_text(self, obj):
        payload = json.dumps(obj).encode()
        mask = os.urandom(4)
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        n = len(payload)
        if n < 126:
            header = struct.pack("!BB", 0x81, 0x80 | n)
        elif n < 65536:
            header = struct.pack("!BBH", 0x81, 0x80 | 126, n)
        else:
            header = struct.pack("!BBQ", 0x81, 0x80 | 127, n)
        self.sock.sendall(header + mask + masked)

    def _need(self, n):
        while len(self.buf) < n:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("closed")
            self.buf += chunk

    def recv(self):
        """One text frame, or None on timeout / a control frame handled internally."""
        try:
            self._need(2)
        except socket.timeout:
            return None
        b0, b1 = self.buf[0], self.buf[1]
        opcode, masked, ln = b0 & 0x0F, b1 & 0x80, b1 & 0x7F
        off = 2
        if ln == 126:
            self._need(4)
            ln = struct.unpack("!H", self.buf[2:4])[0]
            off = 4
        elif ln == 127:
            self._need(10)
            ln = struct.unpack("!Q", self.buf[2:10])[0]
            off = 10
        if masked:
            self._need(off + 4)
            mask = self.buf[off:off + 4]
            off += 4
        self._need(off + ln)
        data = self.buf[off:off + ln]
        self.buf = self.buf[off + ln:]
        if masked:
            data = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
        if opcode == 0x9:                      # ping -> pong
            self.sock.sendall(struct.pack("!BB", 0x8A, 0x80) + os.urandom(4))
            return None
        if opcode == 0x8:
            raise ConnectionError("board closed the socket")
        if opcode not in (0x1, 0x0):
            return None
        return data.decode("utf-8", "replace")

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def _cli():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("seconds", nargs="?", type=float, default=30.0, help="how long to watch (default 30)")
    p.add_argument("--host", default="192.168.4.1")
    p.add_argument("--start", action="store_true", help="issue START_TRUING once connected")
    p.add_argument("--auto", action="store_true", help="answer positioning/adjustment/runout waits")
    args = p.parse_args()

    try:
        ws = WS(args.host, 80, "/ws")
    except OSError as e:
        raise SystemExit("could not reach %s:80 (%s). Is this machine joined to the board's AP?" % (args.host, e))
    print("connected to %s" % args.host, flush=True)
    seq = 0
    if args.start:
        seq += 1
        ws.send_text({"cmd": "START_TRUING", "seq": seq})
        print("-> START_TRUING", flush=True)

    deadline = time.time() + args.seconds
    while time.time() < deadline:
        try:
            frame = ws.recv()
        except ConnectionError as e:
            print("!! %s" % e, flush=True)
            break
        if frame is None:
            continue
        try:
            f = json.loads(frame)
        except ValueError:
            print("<- (unparsed) %s" % frame[:200], flush=True)
            continue

        marker = "**" if f.get("kind") == "ACOUSTIC_PHASE" else "<-"
        print("%s  %s" % (marker, json.dumps(f)[:400]), flush=True)

        if args.auto and f.get("wait_id"):
            wait = f.get("wait", {}) if isinstance(f.get("wait"), dict) else f
            kind = str(wait.get("kind") or "")
            intent = None
            if "POSITION" in kind or "SPOKE0" in kind:
                intent = "CONFIRM_POSITIONED"
            elif "APPLY_ADJUSTMENT" in kind:
                intent = "CONFIRM_ADJUSTMENT_DONE"
            elif "RUNOUT" in kind:
                intent = "SUBMIT_RUNOUT"
            if intent:
                seq += 1
                wait_id = wait.get("wait_id", f.get("wait_id"))
                ws.send_text({"cmd": intent, "wait_id": wait_id, "seq": seq})
                print("-> %s (wait_id=%s)" % (intent, wait_id), flush=True)
    print("done", flush=True)


if __name__ == "__main__":
    _cli()
