"""Record a live /ws session as a verbatim frame array, for replay in tools/ui_check.js.

ui_check.js runs the web UI's script against hand-built frames. Those catch the cases a
person thought to write; they do not catch "the board sent a sequence the page mishandles".
This connects to /ws, optionally drives a full cycle, and writes every frame exactly as it
arrived - so ui_check.js can push a real session through the page and assert the operator
never sees a raw enum, a stuck card, or a crash.

    python tools/probe/session_capture.py --start --auto --out test/fixtures/ui/session_fastdemo_auto.json

--start issues START_TRUING once connected. --auto answers every wait with the
expected_intent the WAIT_ISSUED frame names (so it works whatever the acquisition mode is).
Stops a second after TERMINAL_RESULT, or at --seconds. Frames are stored under "frames"; a
"meta" block records which board and build produced them.

Stdlib only. The machine must be joined to the board's AP (192.168.4.1).
"""
import argparse
import json
import os
import sys
import time
import urllib.request

sys.path.insert(0, os.path.dirname(__file__))
from ws_client import WS  # noqa: E402


def board_id(host, timeout):
    try:
        with urllib.request.urlopen("http://%s/id" % host, timeout=timeout) as r:
            return json.loads(r.read().decode("utf-8"))
    except Exception:  # noqa: BLE001 - metadata only, never fatal
        return {}


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--host", default="192.168.4.1")
    p.add_argument("--out", required=True, help="where to write the JSON frame log")
    p.add_argument("--start", action="store_true", help="issue START_TRUING once connected")
    p.add_argument("--auto", action="store_true", help="answer waits with the intent they name")
    p.add_argument("--seconds", type=float, default=180.0, help="hard stop (default 180)")
    args = p.parse_args()

    meta = board_id(args.host, 5.0)
    try:
        ws = WS(args.host, 80, "/ws")
    except OSError as e:
        raise SystemExit("could not reach %s:80 (%s). Joined to the board's AP?" % (args.host, e))

    seq = 0

    def send(cmd, **kw):
        nonlocal seq
        seq += 1
        ws.send_text(dict(cmd=cmd, seq=seq, **kw))
        print("-> %s %s" % (cmd, kw or ""), flush=True)

    frames = []
    if args.start:
        send("START_TRUING")
    else:
        send("GET_CURRENT_STATE")

    stop_at = time.time() + args.seconds
    grace_until = None
    while time.time() < stop_at:
        if grace_until is not None and time.time() > grace_until:
            break
        try:
            raw = ws.recv()
        except ConnectionError as e:
            print("!! %s" % e, flush=True)
            break
        if not raw:
            continue
        try:
            f = json.loads(raw)
        except ValueError:
            print("   (unparsed) %.120r" % raw, flush=True)
            continue
        frames.append(f)

        kind = f.get("kind", "")
        if kind == "ACOUSTIC_PHASE":
            print("   ** %-14s sp%s a%s exc=%s cmd=%s" % (
                f.get("phase"), f.get("spoke_index"), f.get("attempt"),
                f.get("excitation"), f.get("pluck_commanded")), flush=True)
        elif kind:
            print("   <- %s" % kind, flush=True)

        if args.auto and kind == "WAIT_ISSUED":
            w = f.get("wait", {})
            ei = w.get("expected_intent")
            if ei:
                extra = {}
                if ei == "SUBMIT_RUNOUT":
                    extra = {"lateral_mm": 0.05, "radial_mm": 0.03}
                send(ei, wait_id=w.get("wait_id"), **extra)
        elif kind == "TERMINAL_RESULT":
            print("   TERMINAL %s" % f.get("result"), flush=True)
            grace_until = time.time() + 1.5

    ws.close()

    doc = {
        "meta": {
            "captured_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
            "host": args.host,
            "board": meta.get("board"),
            "build": meta.get("build"),
            "ui": meta.get("ui"),
            "mode": meta.get("mode"),
            "acquisition": meta.get("acquisition"),
            "started_here": args.start,
            "n_frames": len(frames),
        },
        "frames": frames,
    }
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as fh:
        json.dump(doc, fh, indent=1)
        fh.write("\n")
    print("wrote %d frames -> %s" % (len(frames), args.out), flush=True)


if __name__ == "__main__":
    main()
