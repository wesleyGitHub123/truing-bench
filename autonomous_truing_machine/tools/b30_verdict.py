"""B3.0 verdict: do the controls show a false clear or a coherent line?

The rule this implements is the one registered in docs/SOLENOID_CAMPAIGN.md ("B3.0 pre-registration").
Read that first; the short form:

  groups     no_fire (20)  |  air_LEFT (10)  |  air_RIGHT (10)     -- each judged on its OWN, never pooled
  A          zero false clears in every group  (a clear = status suspect, i.e. PROVISIONAL_MODE_ID)
  B          no coherent line in any group. A line is a strong peak the firmware detector finds in the
             f1 band whose SNR reaches the chain profile's own measurement_min_snr_db; it is coherent if
             a peak within +-2 Hz of it appears in >= 50% of the group's captures.
  verdict    FAIL if A or B is violated; else INCONCLUSIVE if the data are short or unreadable; else PASS.

    python tools/b30_verdict.py --dir <bundles> --lines lines.csv
    python tools/b30_verdict.py --dir <bundles> --run          # produce the lines CSV with pio first
    python tools/b30_verdict.py --selftest

`--lines` is the output of `TRUING_SWEEP_MODE=lines pio test -e native_sweep -v` (test/test_acoustic_sweep):
every strong peak the firmware's own detector reports per capture, before any clear/reject gate, with the
onset's absolute floor disabled so a quiet control still has a spectrum. Stdlib only.
"""
import argparse
import csv
import json
import os
import re
import subprocess
import sys
import tempfile

LINE_TOL_HZ = 2.0          # the campaign consistency band, reused for "the same line"
RECURRENCE = 0.5           # plan: >= 50% of a group
MAINS_HZ, MAINS_NEAR_HZ = 360.0, 5.0   # plan: report any line within +-5 Hz of 360 Hz
EXPECTED = {"no_fire": 20, "air_LEFT": 10, "air_RIGHT": 10}
HEADER = ("bundle,onset_with_floor,onset_forced_sample,window_start,window_n,n_fft,n_strong,overflow,idx,freq_hz,"
          "prominence_db,magnitude_db,snr_db,in_f1_band").split(",")


def upper_bound_zero_events(n, confidence=0.95):
    """One-sided upper bound on a rate after 0 events in n trials (exact, the 'rule of three' refined)."""
    return 1.0 - (1.0 - confidence) ** (1.0 / n)


def parse_lines_csv(text):
    """-> (settings, per_bundle). settings: f1_lo, f1_hi, min_snr. per_bundle[name]: dict with the capture
    facts and `peaks` = [(freq, snr)] for every strong peak in the f1 band."""
    settings = {}
    per = {}
    for raw in text.splitlines():
        line = raw.strip()
        m = re.search(r"f1_band ([\d.]+)-([\d.]+) Hz.*min_snr ([\d.]+) dB(?:, chain_digest ([0-9a-f]+))?", line)
        if m and line.startswith("lines dir"):
            settings = {"f1_lo": float(m.group(1)), "f1_hi": float(m.group(2)), "min_snr": float(m.group(3)),
                        "chain_digest": m.group(4)}
            continue
        if not line or line.startswith(("bundle,", "lines ")):
            continue
        row = next(csv.reader([line]))
        # the note column ("NO_ONSET_EVEN_FORCED" ...) rides in the last field of a short row
        if len(row) != len(HEADER):
            continue
        d = dict(zip(HEADER, row))
        b = per.setdefault(d["bundle"], {"note": None, "overflow": False, "n_strong": None, "peaks": [], "max_freq": None})
        if d["in_f1_band"] in ("NO_ONSET_EVEN_FORCED", "NO_WINDOW") or d["onset_with_floor"] in (
                "METADATA_MISSING", "METADATA_BAD", "N_WORDS_MISSING", "PCM_SHORT"):
            b["note"] = d["in_f1_band"] if d["in_f1_band"] else d["onset_with_floor"]
            continue
        b["n_strong"] = int(d["n_strong"])
        b["overflow"] = d["overflow"] == "1"
        if d["idx"] != "":
            freq = float(d["freq_hz"])
            b["max_freq"] = freq if b["max_freq"] is None else max(b["max_freq"], freq)
            if d["in_f1_band"] == "1" and d["snr_db"] != "":
                b["peaks"].append((freq, float(d["snr_db"])))
    return settings, per


def load_bundles(directory):
    names = []
    with open(os.path.join(directory, "index.txt"), encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith("#"):
                names.append(line)
    excluded = set()
    for fn in os.listdir(directory):
        if fn.endswith("_exclusions.jsonl"):
            with open(os.path.join(directory, fn), encoding="utf-8") as f:
                for line in f:
                    if line.strip():
                        b = json.loads(line).get("bundle")
                        if b:
                            excluded.add(b)
    docs = {}
    for n in names:
        path = os.path.join(directory, n + ".json")
        if os.path.exists(path):
            with open(path, encoding="utf-8") as f:
                docs[n] = json.load(f)
    return docs, excluded


def group_of(doc):
    kind = doc.get("campaign_kind")
    if kind == "no_fire":
        return "no_fire"
    if kind == "air_shot":
        return "air_%s" % doc.get("campaign_station")
    return None


def is_clear(doc):
    return doc.get("status") == "suspect"


def coherent_lines(captures, tol=LINE_TOL_HZ, frac=RECURRENCE):
    """captures: {bundle: [freq, ...]} of candidate lines. Every line supported by >= frac of the captures,
    strongest support first, no two within tol of each other."""
    n = len(captures)
    if n == 0:
        return []
    found = []
    for c in sorted(f for fs in captures.values() for f in fs):
        members = {}
        for b, fs in captures.items():
            near = [f for f in fs if abs(f - c) <= tol]
            if near:
                members[b] = min(near, key=lambda f: abs(f - c))
        if len(members) / n >= frac:
            found.append((c, members))
    found.sort(key=lambda t: (-len(t[1]), t[0]))
    lines = []
    for c, members in found:
        if all(abs(c - l["freq_hz"]) > tol for l in lines):
            fs = sorted(members.values())
            lines.append({"freq_hz": fs[len(fs) // 2], "n": len(members), "of": n, "bundles": sorted(members)})
    return lines


def evaluate(docs, excluded, settings, per):
    """The verdict and everything it rests on, as a dict."""
    r = {"groups": {}, "problems": [], "plan_sha256": set(), "chain_digests": set(), "settings": settings}
    min_snr, f1_hi = settings["min_snr"], settings["f1_hi"]
    for name, doc in docs.items():
        g = group_of(doc)
        # only B3.0's own bundles: a later stage's air shots (B3.2 fires them at 40-85 ms) share the directory
        if g is None or doc.get("campaign_stage") != "B3.0" or name in excluded or doc.get("capture_result") not in (None, "OK"):
            continue
        r["plan_sha256"].add(doc.get("campaign_plan_sha256"))
        r["chain_digests"].add(doc.get("chain_digest"))
        grp = r["groups"].setdefault(g, {"bundles": [], "clears": [], "lines": {}, "mains": {}, "unreadable": [], "truncated": []})
        grp["bundles"].append(name)
        if is_clear(doc):
            grp["clears"].append(name)
        a = per.get(name)
        if a is None or a["note"] or a["n_strong"] is None:
            grp["unreadable"].append(name)
            continue
        if a["overflow"] and (a["max_freq"] is None or a["max_freq"] < f1_hi):
            grp["truncated"].append(name)   # the peak list stopped before the top of the f1 band
        grp["lines"][name] = [f for f, snr in a["peaks"] if snr >= min_snr]
        near = [snr for f, snr in a["peaks"] if abs(f - MAINS_HZ) <= MAINS_NEAR_HZ]
        grp["mains"][name] = max(near) if near else None
    fail, incomplete = [], []
    if len(r["plan_sha256"]) > 1:
        incomplete.append("bundles come from %d different plans" % len(r["plan_sha256"]))
    # The analysis constants are the fixture chain profile's; the captures were taken under the board's. If the
    # digests differ the verdict would be judging the wrong constants, so it refuses rather than guess.
    if settings.get("chain_digest") and r["chain_digests"] != {settings["chain_digest"]}:
        incomplete.append("captures were taken under chain profile(s) %s but the analysis uses %s" % (
            sorted(str(x)[:12] for x in r["chain_digests"]), settings["chain_digest"][:12]))
    for g, want in EXPECTED.items():
        grp = r["groups"].get(g)
        n = len(grp["bundles"]) if grp else 0
        if n != want:
            incomplete.append("group %s has %d valid captures, the plan fixes %d" % (g, n, want))
        if not grp:
            continue
        grp["n"] = n
        grp["coherent"] = coherent_lines(grp["lines"])
        if grp["clears"]:
            fail.append("A: %d false clear(s) in %s (%s)" % (len(grp["clears"]), g, ", ".join(grp["clears"])))
        for l in grp["coherent"]:
            fail.append("B: coherent line at %.1f Hz in %s (%d/%d captures)" % (l["freq_hz"], g, l["n"], l["of"]))
        if grp["unreadable"]:
            incomplete.append("%s: %d capture(s) have no readable spectrum (%s)" % (g, len(grp["unreadable"]), ", ".join(grp["unreadable"][:3])))
        if grp["truncated"]:
            incomplete.append("%s: %d capture(s) had the strong-peak list cut off inside the f1 band" % (g, len(grp["truncated"])))
    for g in r["groups"]:
        if g not in EXPECTED:
            incomplete.append("unexpected group %s" % g)
    r["fail"], r["incomplete"] = fail, incomplete
    r["verdict"] = "FAIL" if fail else ("INCONCLUSIVE" if incomplete else "PASS")
    return r


def render(r):
    out = ["B3.0 verdict: %s" % r["verdict"], ""]
    s = r["settings"]
    out.append("detector settings read from the data: f1 band %.0f-%.0f Hz, line = SNR >= %.1f dB (the chain profile's own gate)" % (
        s["f1_lo"], s["f1_hi"], s["min_snr"]))
    out.append("plan sha256: %s" % ", ".join(sorted(str(x) for x in r["plan_sha256"])))
    out.append("chain digest: captures %s, analysis %s" % (
        ", ".join(sorted(str(x)[:12] for x in r["chain_digests"])), (s.get("chain_digest") or "not reported")[:12]))
    out.append("")
    out.append("%-10s %4s %8s %10s %14s %12s" % ("group", "n", "clears", "lines>=gate", "coherent lines", "360Hz max SNR"))
    for g in EXPECTED:
        grp = r["groups"].get(g)
        if not grp or "n" not in grp:
            out.append("%-10s %4s" % (g, "0"))
            continue
        n_with = sum(1 for fs in grp["lines"].values() if fs)
        mains = [v for v in grp["mains"].values() if v is not None]
        out.append("%-10s %4d %8d %10s %14d %12s" % (
            g, grp["n"], len(grp["clears"]), "%d/%d" % (n_with, grp["n"]), len(grp["coherent"]),
            "%.1f dB" % max(mains) if mains else "none"))
    out.append("")
    total = sum(len(g["clears"]) for g in r["groups"].values())
    n_all = sum(g.get("n", 0) for g in r["groups"].values())
    if total == 0 and n_all:
        out.append("0 false clears in %d controls: false-clear rate <= %.1f%% at 95%% confidence; per group "
                   "<= %.1f%% (n=20), <= %.1f%% (n=10)." % (n_all, 100 * upper_bound_zero_events(n_all),
                                                        100 * upper_bound_zero_events(20), 100 * upper_bound_zero_events(10)))
    for msg in r["fail"]:
        out.append("FAIL  " + msg)
    for msg in r["incomplete"]:
        out.append("INCOMPLETE  " + msg)
    if r["verdict"] == "PASS":
        out.append("")
        out.append("PASS means: no false clear, and no line at or above the SNR gate recurring in half of any group.")
        out.append("It does not mean nothing is there: a line below the gate, or recurring in fewer than half of a")
        out.append("group, is not detected by this rule, and 20/10/10 captures cannot rule out a rare one.")
    return "\n".join(out)


def run_lines(directory):
    home = os.environ["USERPROFILE"]
    pio = os.path.join(home, ".platformio", "penv", "Scripts", "pio.exe")
    env = dict(os.environ, TRUING_SWEEP_DIR=os.path.abspath(directory), TRUING_SWEEP_MODE="lines")
    p = subprocess.run([pio, "test", "-d", os.path.join(home, "truing_ws"), "-e", "native_sweep", "-v"],
                       env=env, capture_output=True, text=True, errors="replace")
    if "lines complete" not in p.stdout:
        raise SystemExit("the lines run did not complete:\n" + (p.stdout + p.stderr)[-2000:])
    return p.stdout


# ---------------------------------------------------------------------------------------------------
def selftest():
    import random

    def build(n_nofire=20, n_air=10, clear_in=(), line=None, truncate=(), unreadable=(), excluded_extra=(), digest="d1" * 32):
        """line = (group, freq, snr, how_many_captures) puts a candidate line in that many of the group."""
        d = tempfile.mkdtemp(prefix="b30_selftest_")
        docs, rows, names = {}, [], []
        rng = random.Random(7)
        spec = [("no_fire", "no_fire", None, n_nofire), ("air_LEFT", "air_shot", "LEFT", n_air), ("air_RIGHT", "air_shot", "RIGHT", n_air)]
        for g, kind, st, n in spec:
            for i in range(n):
                name = "%s_%02d" % (g, i)
                names.append(name)
                doc = {"campaign_kind": kind, "campaign_station": st or ("LEFT" if i % 2 == 0 else "RIGHT"), "status": "rejected",
                       "reason": "NO_ONSET_DETECTED", "capture_result": "OK", "campaign_plan_sha256": "abc",
                       "fired": kind != "no_fire", "campaign_stage": "B3.0", "chain_digest": digest}
                if name in clear_in:
                    doc.update(status="suspect", reason="PROVISIONAL_MODE_ID")
                json.dump(doc, open(os.path.join(d, name + ".json"), "w"))
                # a noise-like set of strong peaks in the f1 band, none at or above the gate, random frequencies
                peaks = [(rng.uniform(350, 600), rng.uniform(-3, 10.5)) for _ in range(12)]
                if line and g == line[0] and i < line[3]:
                    peaks.append((line[1] + rng.uniform(-0.8, 0.8), line[2]))
                overflow = 1 if name in truncate else 0
                top = peaks + ([(560.0, 0.0)] if overflow else [(700.0, 0.0)])
                if name in unreadable:
                    rows.append("%s,1,,,,,,,,,,,,NO_WINDOW" % name)
                    continue
                for k, (f, snr) in enumerate(sorted(top)):
                    inband = 1 if 350 <= f <= 600 else 0
                    rows.append("%s,1,2160,4080,24000,262144,%d,%d,%d,%.3f,20.00,-5.00,%.2f,%d" % (
                        name, len(top), overflow, k, f, snr, inband))
        with open(os.path.join(d, "index.txt"), "w") as f:
            f.write("\n".join(names) + "\n")
        if excluded_extra:
            with open(os.path.join(d, "B3.0_exclusions.jsonl"), "w") as f:
                for b in excluded_extra:
                    f.write(json.dumps({"bundle": b}) + "\n")
        text = "lines dir: x   bundles: %d   f1_band 350-600 Hz, prominence 6.0 dB, depth 30.0 dB, min_snr 12.0 dB, chain_digest %s\n" % (len(names), "d1" * 32)
        text += "\n".join(rows) + "\n"
        return d, text

    def verdict(**kw):
        d, text = build(**kw)
        docs, excluded = load_bundles(d)
        settings, per = parse_lines_csv(text)
        return evaluate(docs, excluded, settings, per)

    assert verdict()["verdict"] == "PASS", "a clean set must pass"

    # a later stage's air shots and controls sharing the directory must not touch the B3.0 verdict
    d, text = build()
    docs, excluded = load_bundles(d)
    settings, per = parse_lines_csv(text)
    stray = {"campaign_kind": "air_shot", "campaign_station": "LEFT", "status": "suspect", "reason": "PROVISIONAL_MODE_ID",
             "capture_result": "OK", "campaign_plan_sha256": "other-plan", "fired": True, "campaign_stage": "B3.2",
             "chain_digest": "d1" * 32}
    docs["B3.2_stray_air"] = stray
    r = evaluate(docs, excluded, settings, per)
    assert r["verdict"] == "PASS" and r["plan_sha256"] == {"abc"}, r
    assert len(r["groups"]["air_LEFT"]["bundles"]) == 10, "B3.2 air shot leaked into the B3.0 air_LEFT group"

    # the denominator: a line in 6 of the 10 LEFT air shots is 15% of all 40 controls but 60% of its group
    r = verdict(line=("air_LEFT", 360.4, 15.0, 6))
    assert r["verdict"] == "FAIL" and any("air_LEFT" in m and "360" in m for m in r["fail"]), r["fail"]
    pooled = 6 / 40
    assert pooled < RECURRENCE, "the point: pooling over 40 would have PASSED this"
    assert verdict(line=("air_LEFT", 360.4, 15.0, 5))["verdict"] == "FAIL", "5 of 10 is exactly 50%: fails"
    assert verdict(line=("air_LEFT", 360.4, 15.0, 4))["verdict"] == "PASS", "4 of 10 is under 50%: passes"
    assert verdict(line=("no_fire", 471.0, 14.0, 10))["verdict"] == "FAIL", "10 of 20 no-fire is 50%"
    assert verdict(line=("no_fire", 471.0, 14.0, 9))["verdict"] == "PASS"

    # below the gate: recurring but not at the gate is not detected, and the verdict says so rather than hiding it
    assert verdict(line=("air_RIGHT", 400.0, 9.0, 10))["verdict"] == "PASS"

    # false clears, per group
    r = verdict(clear_in={"air_RIGHT_03"})
    assert r["verdict"] == "FAIL" and r["fail"][0].startswith("A:"), r["fail"]

    # incomplete data is never a PASS
    assert verdict(n_nofire=19)["verdict"] == "INCONCLUSIVE"
    assert verdict(truncate={"no_fire_04"})["verdict"] == "INCONCLUSIVE"
    assert verdict(unreadable={"air_LEFT_00"})["verdict"] == "INCONCLUSIVE"
    # but a fail that is already visible is a FAIL whatever else is missing
    assert verdict(n_nofire=19, clear_in={"air_LEFT_02"})["verdict"] == "FAIL"

    # a ledgered exclusion is not counted, so its group is short
    assert verdict(excluded_extra=["no_fire_00"])["verdict"] == "INCONCLUSIVE"

    # captures taken under other constants than the analysis uses: refuse, do not guess
    r = verdict(digest="ee" * 32)
    assert r["verdict"] == "INCONCLUSIVE" and any("chain profile" in m for m in r["incomplete"]), r["incomplete"]

    # the bound printed for zero events
    assert abs(upper_bound_zero_events(40) - 0.0721) < 5e-4 and abs(upper_bound_zero_events(10) - 0.2589) < 5e-4
    print("selftest ok: pass/fail/inconclusive, per-group denominator (5/10 fails, 4/10 passes; pooled would pass), "
          "sub-gate lines, false clears, short/truncated/unreadable data, exclusions")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dir", help="the campaign bundle directory (has index.txt)")
    ap.add_argument("--lines", help="the lines CSV")
    ap.add_argument("--run", action="store_true", help="produce the lines CSV with pio first")
    ap.add_argument("--out", help="also write the report here")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        selftest()
        return
    if not args.dir or not (args.lines or args.run):
        ap.error("--dir and one of --lines/--run are required")
    text = run_lines(args.dir) if args.run else open(args.lines, encoding="utf-8", errors="replace").read()
    settings, per = parse_lines_csv(text)
    if not settings:
        raise SystemExit("no 'lines dir ... f1_band ... min_snr' header in the lines output")
    docs, excluded = load_bundles(args.dir)
    report = render(evaluate(docs, excluded, settings, per))
    print(report)
    if args.out:
        with open(args.out, "w", encoding="utf-8", newline="\n") as f:
            f.write(report + "\n")


if __name__ == "__main__":
    main()
