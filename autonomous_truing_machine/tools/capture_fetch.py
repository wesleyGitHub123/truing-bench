"""Pull the last acoustic capture off a board and store it as a replayable fixture.

A pluck that goes wrong is unrepeatable: the next one is a different pluck, on a different
part of the same spoke, with the operator's hand in a different place. So the samples are the
only durable evidence there is. This fetches them.

    python tools/capture_fetch.py --name nano_s05_ambiguous --note "AMBIGUOUS_PEAK, rim seam"

writes test/fixtures/acoustic/captures/<name>.pcm and <name>.json, appends <name> to index.txt,
and from then on `pio test -e native -f test_acoustic_replay` reproduces that measurement on
the host with no hardware present.

What it does NOT do is decide anything. The board's own diagnostics come across as the
expectations (expect_origin "board"), so a passing replay means the host reproduced what the
board did on those exact bytes, and a failing one means they diverged - which is a finding
either way, not a tolerance to be relaxed.

The capture buffer is live: the board reuses it for the next measurement. Both endpoints
report X-Truing-Capture-Seq, and this reads metadata -> samples -> metadata again, keeping the
bundle only if all three agree. Fetch while the machine is idle and that never triggers.

Requires nothing outside the standard library. The board serves its AP at 192.168.4.1, so the
laptop has to be joined to it (the SSID is in `GET /id`).
"""
import argparse
import hashlib
import json
import os
import re
import sys
import time
import urllib.error
import urllib.request

DEFAULT_HOST = "192.168.4.1"
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CAPTURES = os.path.join(REPO_ROOT, "test", "fixtures", "acoustic", "captures")
SCHEMA = "truing.acoustic.capture/1"
SEQ_HEADER = "X-Truing-Capture-Seq"


def get(url, timeout):
    req = urllib.request.Request(url, headers={"Connection": "close"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        seq = r.headers.get(SEQ_HEADER)
        # Kept short deliberately: a malformed header once arrived as the entire response body
        # (the board was handing httpd a pointer to a dead stack buffer) and the resulting
        # error message was a wall of JSON that said nothing about what was wrong.
        if seq is not None and not re.fullmatch(r"\d{1,10}", seq.strip()):
            raise SystemExit("%s returned a %s header that is not a number: %.40r" % (url, SEQ_HEADER, seq))
        return r.read(), (seq.strip() if seq else None)


def fetch_bundle(host, timeout):
    """metadata, samples, metadata again - and refuse anything that moved in between."""
    base = "http://%s" % host
    meta_raw, seq_a = get(base + "/debug/capture.json", timeout)
    meta = json.loads(meta_raw.decode("utf-8"))
    if meta.get("schema") != SCHEMA:
        raise SystemExit("board speaks schema %r, this tool speaks %r" % (meta.get("schema"), SCHEMA))
    pcm, seq_b = get(base + "/debug/capture.pcm", timeout)
    _, seq_c = get(base + "/debug/capture.json", timeout)
    if None in (seq_a, seq_b, seq_c):
        raise SystemExit("the board did not report %s; it cannot be told whether the capture "
                         "changed mid-download" % SEQ_HEADER)
    if not (seq_a == seq_b == seq_c):
        raise SystemExit(
            "the board measured again while this was downloading (seq %s -> %s -> %s); "
            "fetch while the machine is idle" % (seq_a, seq_b, seq_c))
    expected = int(meta["n_words"]) * 4
    if len(pcm) != expected:
        raise SystemExit("expected %d bytes of samples, got %d" % (expected, len(pcm)))
    return meta, pcm


def default_name(meta):
    return "%s_s%02d_c%d_a%d_%s" % (
        meta.get("board", "board"), int(meta.get("spoke_id", 0)), int(meta.get("cycle_index", 0)),
        int(meta.get("attempt", 0)), time.strftime("%Y%m%dT%H%M%S"))


def write_bundle(name, meta, pcm, note, out_dir):
    os.makedirs(out_dir, exist_ok=True)
    pcm_name = name + ".pcm"
    with open(os.path.join(out_dir, pcm_name), "wb") as f:
        f.write(pcm)
    doc = dict(meta)
    doc["name"] = name
    doc["pcm"] = pcm_name
    doc["pcm_sha256"] = hashlib.sha256(pcm).hexdigest()
    doc["fetched_at"] = time.strftime("%Y-%m-%dT%H:%M:%S%z")
    doc["origin"] = "board"
    if note:
        doc["note"] = note
    # Flat, and it has to stay flat: the replay harness reads it with the firmware's own JSON
    # reader, which resolves members of the top-level object and nothing deeper.
    for key, value in doc.items():
        if isinstance(value, (dict, list)):
            raise SystemExit("field %r is not a scalar; the capture schema is flat" % key)
    with open(os.path.join(out_dir, name + ".json"), "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=2, sort_keys=True)
        f.write("\n")
    return doc


def add_to_index(name, out_dir):
    path = os.path.join(out_dir, "index.txt")
    existing = []
    if os.path.exists(path):
        with open(path, "r", encoding="utf-8") as f:
            existing = f.read().splitlines()
    if any(line.strip() == name for line in existing):
        return False
    with open(path, "a", encoding="utf-8") as f:
        f.write(name + "\n")
    return True


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--host", default=DEFAULT_HOST, help="board address (default %s)" % DEFAULT_HOST)
    p.add_argument("--name", help="fixture name; defaults to board_spoke_cycle_attempt_timestamp")
    p.add_argument("--note", help="what this capture is evidence of, in your words")
    p.add_argument("--dir", default=CAPTURES, help="where the bundle goes")
    p.add_argument("--timeout", type=float, default=20.0)
    p.add_argument("--no-index", action="store_true", help="write the bundle but do not add it to index.txt")
    args = p.parse_args()

    try:
        meta, pcm = fetch_bundle(args.host, args.timeout)
    except urllib.error.HTTPError as e:
        if e.code == 404:
            raise SystemExit("the board has nothing captured yet - measure a spoke first")
        raise SystemExit("board returned %s %s" % (e.code, e.reason))
    except (urllib.error.URLError, OSError) as e:
        raise SystemExit("cannot reach %s (%s). Joined to the board's AP?" % (args.host, e))

    name = args.name or default_name(meta)
    if not re.fullmatch(r"[A-Za-z0-9._-]{1,80}", name):
        raise SystemExit("name must be 1-80 characters of [A-Za-z0-9._-]")

    doc = write_bundle(name, meta, pcm, args.note, args.dir)
    indexed = False if args.no_index else add_to_index(name, args.dir)

    print("%s: %d words from %s, %s build %s" % (
        name, doc["n_words"], doc.get("source", "?"), doc.get("board", "?"), doc.get("build", "?")))
    print("  spoke %s cycle %s attempt %s -> %s / %s" % (
        doc.get("spoke_id"), doc.get("cycle_index"), doc.get("attempt"),
        doc.get("status"), doc.get("reason")))
    print("  f1 %s Hz  snr %s dB  onset %s  window %s+%s" % (
        doc.get("expect_f1_hz"), doc.get("expect_snr_db"), doc.get("expect_onset_sample"),
        doc.get("expect_window_start_sample"), doc.get("expect_window_n_samples")))
    print("  sha256 %s" % doc["pcm_sha256"])
    if doc.get("source") != "real":
        print("  NOTE: source is %r - these words did not come from the microphone." % doc.get("source"))
    if indexed:
        print("  added to index.txt; replay with: pio test -e native -f test_acoustic_replay")
    return 0


if __name__ == "__main__":
    sys.exit(main())
