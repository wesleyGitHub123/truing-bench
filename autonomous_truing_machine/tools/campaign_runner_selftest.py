"""Runs campaign_runner.run_plan against a fake board, no hardware and no network.

    python tools/campaign_runner_selftest.py

The fake honours MEASURE_ONCE's packed arg the way the firmware does (no_fire bit -> fired=false,
pulse_ms 0; a pulse override -> that width), and injects the failures the runner has to survive: a
refused ack, an accepted intent with no new capture, a capture whose fired flag contradicts the request, and an
overrun capture. It checks that each is excluded and replaced in place, ledgered under its code, that the
executed sequence is still exactly the plan, and that every kept bundle carries the plan hash.
"""
import argparse
import json
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import campaign_runner as cr        # noqa: E402
import campaign_sequence as cs      # noqa: E402


class FakeBoard:
    def __init__(self, failures):
        self.seq = 0
        self.failures = failures          # {call_number: kind}
        self.calls = 0

    def fire_once(self, ws, host, timeout, seq_counter, spoke_id, arg=None):
        self.calls += 1
        fail = self.failures.get(self.calls)
        no_fire = bool(arg & cr.MEASURE_ONCE_NO_FIRE_BIT)
        pulse = arg >> cr.MEASURE_ONCE_PULSE_SHIFT
        if fail == "ack":
            return {"verdict": "REJECT_SESSION_ADMISSION", "reason": "VALUE_OUT_OF_RANGE"}, None, None
        if fail == "stale":
            return {"verdict": "ACCEPT"}, "stale", None
        self.seq += 1
        meta = {"seq": self.seq, "spoke_id": spoke_id, "cycle_index": 0, "attempt": 1, "status": "rejected",
                "reason": "NO_ONSET_DETECTED", "debug_triggered": True, "n_words": 4, "source": "real",
                "fired": not no_fire, "pulse_ms": 0.0 if no_fire else float(pulse or 20), "capture_result": "OK",
                "station": "LEFT" if spoke_id % 2 == 0 else "RIGHT"}
        if fail == "contradict":
            meta["fired"] = no_fire                    # the record says the opposite of what was asked
        if fail == "overrun":
            meta["capture_result"] = "OVERRUN"
        return {"verdict": "ACCEPT"}, meta, b"\x00" * 12


def main():
    plan = cs.b30_plan(1)
    sha = cs.plan_sha256(plan)
    total = sum(len(b["trials"]) for b in plan["blocks"])
    failures = {3: "ack", 9: "stale", 15: "overrun", 22: "contradict"}
    board = FakeBoard(failures)
    cr.fire_once = board.fire_once
    cr.time.sleep = lambda s: None
    out = tempfile.mkdtemp(prefix="runner_selftest_")
    args = argparse.Namespace(host="x", timeout=1, out=out, interval=0, start_block=0, end_block=None, no_prompt=True)
    ctx = {"seq_counter": [0], "rig_id_by_station": {"LEFT": "LEFT/rig-1", "RIGHT": "RIGHT/rig-1"},
           "label": "B3.0", "session": "test", "note": "selftest"}
    kept, excluded = cr.run_plan(None, args, ctx, plan, sha)

    # 1. every planned trial was eventually kept exactly once; the four failures were replaced in place
    assert kept == total == 40, (kept, total)
    assert excluded == len(failures) == 4, excluded
    assert board.calls == total + len(failures)

    # 2. the ledger names each failure under its pre-declared code
    ledger = [json.loads(l) for l in open(os.path.join(out, "B3.0_exclusions.jsonl"), encoding="utf-8")]
    assert sorted(e["code"] for e in ledger) == sorted([
        cr.EXCLUSION_ACK_REJECTED, cr.EXCLUSION_NO_NEW_CAPTURE, cr.EXCLUSION_CAPTURE_NOT_OK, cr.EXCLUSION_RECORD_CONTRADICTS]), ledger
    assert all(e["plan_sha256"] == sha for e in ledger)

    # 3. the kept trials, in trial order, are exactly the plan's sequence
    docs = {}
    for fn in os.listdir(out):
        if fn.endswith(".json") and fn not in ("B3.0_exclusions.jsonl",):
            d = json.load(open(os.path.join(out, fn), encoding="utf-8"))
            docs.setdefault(d["campaign_trial"], []).append(d)
    planned = [t for b in plan["blocks"] for t in b["trials"]]
    assert sorted(docs) == list(range(1, total + 1)), "a trial number is missing"
    for i, t in enumerate(planned, start=1):
        good = [d for d in docs[i] if d["capture_result"] == "OK" and (d["fired"] is False) == (t["kind"] == "no_fire")]
        assert len(good) == 1, (i, t, docs[i])
        d = good[0]
        assert d["campaign_kind"] == t["kind"] and d["spoke_id"] == t["spoke"], (i, t, d["campaign_kind"])
        assert d["campaign_plan_sha256"] == sha and d["campaign_plan_seed"] == 1 and d["campaign_stage"] == "B3.0"
        assert d["campaign_rig_id"].startswith(d["campaign_station"] + "/rig-")
        assert d["campaign_selection"] == ("no_fire" if t["kind"] == "no_fire" else "derived")

    # 4. an excluded trial that produced a bundle keeps it (nothing deleted), and it is not the counted one
    kept_names = sum(1 for v in docs.values() for d in v)
    assert kept_names == total + 2, kept_names       # overrun + contradict wrote a bundle; ack + stale wrote none

    # 5. the same trial failing three times in a row stops the run rather than routing around it
    stuck = FakeBoard({n: "ack" for n in range(1, 10)})
    cr.fire_once = stuck.fire_once
    try:
        cr.run_plan(None, args, dict(ctx, label="B3.0stuck"), plan, sha)
    except SystemExit as e:
        assert "failed 3 times" in str(e), e
    else:
        raise AssertionError("a trial failing three times must stop the run")
    # 6. one block at a time (the operator turns the wheel in between) numbers trials exactly as a single run does
    n0 = len(plan["blocks"][0]["trials"])
    cr.fire_once = FakeBoard({}).fire_once
    out2 = tempfile.mkdtemp(prefix="runner_selftest_blocks_")
    a1 = argparse.Namespace(host="x", timeout=1, out=out2, interval=0, start_block=0, end_block=1, no_prompt=True)
    a2 = argparse.Namespace(host="x", timeout=1, out=out2, interval=0, start_block=1, end_block=None, no_prompt=True)
    k1, x1 = cr.run_plan(None, a1, dict(ctx, label="B3.0blocks"), plan, sha)
    assert (k1, x1) == (n0, 0), (k1, x1)
    k2, x2 = cr.run_plan(None, a2, dict(ctx, label="B3.0blocks"), plan, sha)
    assert (k2, x2) == (total - n0, 0), (k2, x2)
    nums = sorted(json.load(open(os.path.join(out2, fn), encoding="utf-8"))["campaign_trial"]
                  for fn in os.listdir(out2) if fn.endswith(".json"))
    assert nums == list(range(1, total + 1)), nums
    # 7. a board that has never captured anything answers 404 "nothing captured yet"; the sequence read
    # before the first shot must treat that as "no previous capture", not as a failed trial
    import urllib.error
    real_fetch = cr.fetch_bundle
    def never_captured(host, timeout):
        raise urllib.error.HTTPError("http://x/debug/capture.json", 404, "Not Found", None, None)
    cr.fetch_bundle = never_captured
    try:
        assert cr.current_seq("x", 1) == 0
        def refused(host, timeout):
            raise urllib.error.HTTPError("http://x/debug/capture.json", 500, "Server Error", None, None)
        cr.fetch_bundle = refused
        try:
            cr.current_seq("x", 1)
        except urllib.error.HTTPError:
            pass
        else:
            raise AssertionError("a 500 must not be read as 'nothing captured'")
    finally:
        cr.fetch_bundle = real_fetch
    print("selftest ok: %d trials kept, 4 exclusions ledgered and replaced in place, plan order preserved, "
          "hash stamped on every bundle, persistent failure stops the run" % kept)


if __name__ == "__main__":
    main()
