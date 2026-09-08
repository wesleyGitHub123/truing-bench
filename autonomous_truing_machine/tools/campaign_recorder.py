"""Harvest every acoustic capture of a session into bundles, unattended.

The board's debug buffer is one measurement deep and the next one overwrites it, so a
characterisation campaign cannot fetch at the end -- it has to fetch between attempts. This
attaches a passive /ws listener (sends only GET_CURRENT_STATE polls, never an intent), and on
every MEASUREMENT_RESULT it pulls the capture that produced it: metadata, samples, metadata
again, refusing anything whose X-Truing-Capture-Seq moved in between (the same guard
tools/capture_fetch.py uses). Each attempt lands as its own bundle.

    python tools/campaign_recorder.py --pass A --note "pluck ~0.5s before the window opens"

Writes <out>/<pass>_sp<N>_a<M>_seq<S>.{pcm,json} and appends nothing to index.txt -- a
campaign is 20-45 bundles at ~230 KB and does not belong in git. Promote the few that become
regression fixtures with tools/capture_fetch.py (or by hand + capture_accept.py) afterwards.

Run from autonomous_truing_machine/ with this machine joined to the board's AP. One --pass per
technique; restart the tool between passes. Ctrl-C to stop; the frame log it prints is meant
to be interleaved with the operator's narration.

Known gap: the board exposes only the last capture, and real_last_capture starts returning
404 the moment the *next* measurement zeroes diag.n_captured (~3.5 s per measurement). An
attempt whose result is immediately followed by another measurement -- the 3rd attempt of a
spoke, where the retry budget is spent and the orchestrator moves straight on -- can lose the
race against a WiFi fetch. Attempts 1 and 2 of every spoke harvest reliably; a hand-plucked
campaign that pauses between strikes loses almost nothing. If yield is short, run more
sessions rather than reaching for the 3rd attempt.
"""
import argparse
import json
import os
import sys
import time
import urllib.error

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "probe"))
from ws_client import WS  # noqa: E402

# capture_fetch lives one directory up from probe/; import its bundle helpers rather than
# reimplement the seq-consistency dance.
sys.path.insert(0, os.path.dirname(__file__))
from capture_fetch import fetch_bundle, write_bundle  # noqa: E402

DEFAULT_HOST = "192.168.4.1"
DEFAULT_OUT = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "test", "fixtures", "acoustic", "captures", "_campaign")


def ts():
    return time.strftime("%H:%M:%S")


def log(m):
    print("%s  %s" % (ts(), m), flush=True)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--pass", dest="pass_", required=True, help="technique label for this run, e.g. A / B / C")
    p.add_argument("--note", default="", help="what this pass is testing, in your words")
    p.add_argument("--host", default=DEFAULT_HOST)
    p.add_argument("--out", default=DEFAULT_OUT, help="bundle directory (default test/fixtures/acoustic/captures/_campaign)")
    p.add_argument("--timeout", type=float, default=20.0)
    p.add_argument("--seconds", type=float, default=1800.0, help="stop after this long (default 30 min)")
    args = p.parse_args()

    os.makedirs(args.out, exist_ok=True)
    try:
        ws = WS(args.host, 80, "/ws")
    except OSError as e:
        raise SystemExit("could not reach %s (%s). Joined to the board's AP?" % (args.host, e))
    log("campaign recorder attached  pass=%s  out=%s" % (args.pass_, args.out))
    log("passive: sends only GET_CURRENT_STATE; harvests on every MEASUREMENT_RESULT")

    seen_seq = set()
    kept = 0
    seq = 0
    last_poll = 0.0
    deadline = time.time() + args.seconds
    try:
        while time.time() < deadline:
            if time.time() - last_poll > 5.0:
                seq += 1
                try:
                    ws.send_text({"cmd": "GET_CURRENT_STATE", "seq": seq})
                except OSError as e:
                    log("poll send failed: %r" % e)
                    break
                last_poll = time.time()
            try:
                fr = ws.recv()
            except ConnectionError as e:
                log("!! socket closed: %s" % e)
                break
            if not fr:
                continue
            try:
                f = json.loads(fr)
            except ValueError:
                continue

            k = f.get("kind") or ""
            if k == "ACOUSTIC_PHASE":
                log("** %-14s sp%s a%s win=%sms" % (f.get("phase"), f.get("spoke_index"),
                                                    f.get("attempt"), f.get("window_ms")))
                continue
            if k in ("STATE_TRANSITION", "WAIT_ISSUED", "TERMINAL_RESULT"):
                log("<- %s" % json.dumps({x: f[x] for x in ("kind", "from", "to", "result", "wait")
                                          if x in f})[:160])
                continue
            if k != "MEASUREMENT_RESULT":
                continue

            ch = f.get("channel")
            log("<- MEAS %s idx%s %s/%s" % (ch, f.get("index"), f.get("status"), f.get("reason")))
            if ch != "TENSION":
                continue  # runout captures nothing acoustic

            # harvest whatever is in the buffer right now. One quick retry only: a transient
            # "downloading" seq-moved race can settle, but a "nothing captured" 404 means the
            # next measurement already zeroed the buffer and this seq is gone for good.
            meta = pcm = None
            for _ in range(2):
                try:
                    meta, pcm = fetch_bundle(args.host, args.timeout)
                    break
                except SystemExit as e:
                    if "downloading" in str(e):
                        time.sleep(0.3)
                        continue
                    log("   skip: %s" % e)
                    break
                except (urllib.error.URLError, OSError) as e:
                    log("   skip: cannot fetch (%s)" % e)
                    break
            if meta is None:
                continue

            s = int(meta.get("seq", 0))
            if s in seen_seq:
                continue
            seen_seq.add(s)
            if meta.get("source") != "real":
                log("   seq %d source=%r -- not the mic; skipped" % (s, meta.get("source")))
                continue

            name = "%s_sp%s_a%s_seq%d" % (args.pass_, meta.get("spoke_id"), meta.get("attempt"), s)
            note = "campaign pass %s. %s" % (args.pass_, args.note) if args.note else "campaign pass %s" % args.pass_
            doc = write_bundle(name, meta, pcm, note, args.out)
            kept += 1
            log("   kept %s  %s/%s  snr=%s f1=%s onset=%s" % (
                name, doc.get("status"), doc.get("reason"),
                doc.get("expect_snr_db"), doc.get("expect_f1_hz"), doc.get("expect_onset_sample")))
    except KeyboardInterrupt:
        log("stopped by operator")
    finally:
        ws.close()
    log("pass %s: %d bundles in %s" % (args.pass_, kept, args.out))


if __name__ == "__main__":
    main()
