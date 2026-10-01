"""Drive the demonstration image's acoustic part end to end over /ws, as the operator would, and report every
spoke's reading. For checking a demo build on the bench with nobody at the page.

    python tools/probe/demo_drive.py                 # confirm the reference, start, confirm each struck spoke
    python tools/probe/demo_drive.py --spokes 3 --out run.json

It answers the reference confirmation (spoke 0 at LEFT) and every positioning wait at an acoustic station
with the intent the wait names -- i.e. it CLAIMS the spoke is at the station; it does not move the wheel
(the manual roller is the operator's, through the page). It stops with ABORT at the first wait that is not
a positioning or reference confirmation (the adjustment step), or after --spokes MEASUREMENT_RESULTs for
distinct spokes, or at --seconds. Prints the last result per spoke; --out keeps every frame.

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

ANSWERABLE = ("CONFIRM_SPOKE0_AT_STATION", "POSITION_TO_SPOKE")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--host", default="192.168.4.1")
    p.add_argument("--spokes", type=int, default=3, help="stop after this many distinct spokes reported")
    p.add_argument("--seconds", type=float, default=240.0)
    p.add_argument("--out")
    p.add_argument("--jog-test", type=int, default=-1, metavar="N",
                   help="at the N-th positioning wait (0-based): jog +steps, then -steps, and confirm while that "
                        "return jog is still moving (the strike must wait for it); then try a jog during the "
                        "measurement (must be refused). MOVES THE WHEEL by about +-steps")
    p.add_argument("--jog-steps", type=int, default=200)
    a = p.parse_args()
    jog_log = []

    def jog(query):
        with urllib.request.urlopen("http://%s/demo/jog?%s" % (a.host, query), timeout=5) as r:
            d = json.loads(r.read().decode("utf-8"))
        jog_log.append((query, d))
        print("   JOG %-12s -> ok=%s moving=%s enabled=%s detail=%s" % (query, d.get("ok"), d.get("moving"),
                                                                       d.get("enabled"), d.get("detail")), flush=True)
        return d

    def wait_still(limit=10.0):
        t = time.time() + limit
        while time.time() < t:
            if not jog("")["moving"]:
                return True
            time.sleep(0.2)
        return False
    positioning_seen = [0]
    jog_pending_refusal = [False]
    ws = WS(a.host, 80, "/ws")
    seq = [0]
    answered = set()

    def send(cmd, **kw):
        seq[0] += 1
        ws.send_text(dict(cmd=cmd, seq=seq[0], **kw))
        print("-> %s %s" % (cmd, kw or ""), flush=True)

    def answer(w):
        if not w or w.get("wait_id") in answered:
            return False
        if w.get("kind") not in ANSWERABLE:
            return None
        answered.add(w.get("wait_id"))
        if w.get("kind") == "POSITION_TO_SPOKE":
            if positioning_seen[0] == a.jog_test:
                jog("steps=%d" % a.jog_steps)
                wait_still()
                jog("steps=%d" % -a.jog_steps)          # confirm while this one is still moving
                jog_pending_refusal[0] = True
            positioning_seen[0] += 1
        send(w.get("expected_intent"), wait_id=w.get("wait_id"))
        return True

    frames, results, started, stop_at, done = [], {}, False, time.time() + a.seconds, False
    send("GET_CURRENT_STATE")
    while time.time() < stop_at and not done:
        raw = ws.recv()
        if not raw:
            continue
        try:
            f = json.loads(raw)
        except ValueError:
            continue
        frames.append(f)
        if f.get("t") == "state":
            st = f.get("current_state")
            r = answer(f.get("active_wait"))
            if r is None:
                send("ABORT")
                done = True
            elif not started and st == "READY" and not f.get("active_wait"):
                send("START_TRUING")
                started = True
        kind = f.get("kind")
        if kind == "WAIT_ISSUED":
            r = answer(f.get("wait"))
            if r is None:
                print("   stopping at a %s wait" % f["wait"].get("kind"), flush=True)
                send("ABORT")
                done = True
            send("GET_CURRENT_STATE")
        elif kind == "STATE_TRANSITION":
            send("GET_CURRENT_STATE")
        elif kind == "ACOUSTIC_PHASE":
            if jog_pending_refusal[0] and f.get("phase") == "LISTENING":
                jog_pending_refusal[0] = False
                d = jog("steps=%d" % a.jog_steps)      # mid-measurement: must be refused
                if d.get("ok"):
                    print("   !! a jog was ACCEPTED during a measurement", flush=True)
            print("   ** %-14s sp%s attempt %s fired=%s" % (f.get("phase"), f.get("spoke_index"), f.get("attempt"),
                                                         f.get("fired")), flush=True)
        elif kind == "MEASUREMENT_RESULT" and f.get("channel") == "TENSION":
            results[f.get("index")] = f
            print("   RESULT sp%s %s/%s f=%s Hz T=%s N" % (f.get("index"), f.get("status"), f.get("reason"),
                                                         f.get("selected_frequency_hz"), f.get("tension_n")), flush=True)
            if len(results) >= a.spokes:
                send("ABORT")
                done = True
        elif kind == "TERMINAL_RESULT":
            done = True
    ws.close()
    print("\nper spoke (last frame wins):")
    for i in sorted(results):
        f = results[i]
        print("  spoke %2d  %-9s %-20s %8s Hz  %8s N" % (i, f.get("status"), f.get("reason"),
                                                         f.get("selected_frequency_hz"), f.get("tension_n")))
    if a.out:
        with open(a.out, "w", encoding="utf-8") as fh:
            json.dump({"frames": frames}, fh, indent=1)
    ok = len(results) >= a.spokes and all(r.get("selected_frequency_hz") is not None for r in results.values())
    if a.jog_test >= 0:
        refused = [d for q, d in jog_log if q == "steps=%d" % a.jog_steps and not d.get("ok")]
        print("jog test: %d jog calls; the mid-measurement jog was %s" % (
            len(jog_log), "REFUSED (good)" if refused else "NOT refused or not attempted"))
        ok = ok and bool(refused)
    print("ALL SPOKES READ" if ok else "NOT every spoke produced a reading")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
