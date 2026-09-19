"""B3.2 cross-spoke coincidence report: does one tone show up on every spoke?

Implements the cross-spoke report registered in docs/SOLENOID_CAMPAIGN.md ("Amendment 2 to
this registration (2026-09-19, after the air blocks, before any strike)"). Short form:

  The B3.2 strike data are judged per spoke, so a tone that repeats at the same frequency on
  every spoke would pass the registered per-spoke test. This tool lists, per station, width
  and candidate, every cell where clears from at least 3 of the station's 4 exploration
  spokes share an f1 (within +-2 Hz, the same band the consistency rule uses; the common
  frequency is their median), and whether that frequency is within +-2 Hz of a coherent line
  in that station's air shots at the same width.

  REPORT ONLY. A flag is a prompt for a human to look, not a verdict: nothing is gated,
  filtered or removed, and no threshold moves. The report never says PASS or FAIL and the
  exit code does not depend on what it found.

    python tools/b32_crossspoke.py --dir <bundles> --sweep sweep.csv [--lines lines.csv]
    python tools/b32_crossspoke.py --selftest

The sweep CSV is the output of `pio test -e native_sweep -v` with TRUING_SWEEP_MODE unset
(test/test_acoustic_sweep; sweep is its default mode). The lines CSV is the same run with
TRUING_SWEEP_MODE=lines, produced under the baseline chain profile only, not under each
candidate, and the report says so. Stdlib only; the lines/ledger machinery is
imported from b30_verdict, not reimplemented.
"""
import argparse
import contextlib
import hashlib
import io
import json
import os
import statistics
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import b30_verdict as b30  # noqa: E402  (load_bundles, parse_lines_csv, coherent_lines, LINE_TOL_HZ)

LINE_TOL_HZ = b30.LINE_TOL_HZ   # "within +-2 Hz of one another" is the same band as "the same line"
FLAG_SPOKES = 3                 # a cell is flagged when clears from at least this many distinct spokes share an f1
EXPECTED_SPOKES = 4             # the registration's exploration spokes per station
# All parsing of the sweep CSV lives here: the format is expected to gain two-field candidate
# rows, and when it does this is the one function to touch.
SWEEP_HEADER = ("bundle,axis,value,status,reason,n_strong_peaks,n_peaks_in_band,f1_hz,f2_hz,snr_db,"
                "clears_min_snr").split(",")


def parse_sweep_csv(text):
    """The one place that knows the sweep row format. -> list of row dicts; "f1" is a float or None.
    Skips the preamble, the header row, any row that is not 11 fields, and any row with an empty axis."""
    out = []
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith(("bundle,", "sweep ")):
            continue
        fields = line.split(",")
        if len(fields) != len(SWEEP_HEADER) or not fields[1].strip():
            continue
        row = dict(zip(SWEEP_HEADER, fields))
        f1 = row["f1_hz"].strip()
        row["f1"] = float(f1) if f1 else None
        row["candidate"] = "%s=%s" % (row["axis"].strip(), row["value"].strip())
        out.append(row)
    return out


def width_ms_of(doc):
    """The strike width in whole ms, or None if the bundle does not say."""
    pm = doc.get("pulse_ms")
    try:
        return int(round(float(pm))) if pm is not None else None
    except (TypeError, ValueError):
        return None


def strike_cell(doc):
    """A B3.2 strike bundle this report may count, as (station, width_ms, spoke), else None.
    An air_shot or no_fire bundle is never a strike; a ledgered bundle is not counted."""
    if doc.get("campaign_stage") != "B3.2" or doc.get("campaign_kind") != "strike":
        return None
    if doc.get("capture_result") not in (None, "OK"):
        return None
    station, spoke = doc.get("campaign_station"), doc.get("spoke_id")
    width = width_ms_of(doc)
    if station is None or width is None or spoke is None:
        return None
    return station, width, int(spoke)


def is_clear(row):
    """Clear = the sweep row's status is suspect and f1_hz is non-empty. A rejected row with an
    f1 is not a clear."""
    return row["status"] == "suspect" and row["f1"] is not None


def coincidence_of(clears, tol=LINE_TOL_HZ, need=FLAG_SPOKES):
    """clears: [(spoke, f1)]. Try every clear's f1 as a centre; the members are the clears with
    abs(f1 - c) <= tol (inclusive); keep the centre with the most distinct spokes (ties: the
    lowest centre - scanning the centres in sorted order and replacing only on strictly more
    does that). -> None, or a dict with the members, their spokes and the common frequency
    (the median of the member f1 values)."""
    best = None
    for c in sorted(f for _, f in clears):
        members = [(s, f) for s, f in clears if abs(f - c) <= tol]
        spokes = sorted(set(s for s, _ in members))
        if best is None or len(spokes) > len(best["spokes"]):
            best = {"centre": c, "members": members, "spokes": spokes}
    if best is None or len(best["spokes"]) < need:
        return None
    best["common"] = statistics.median(f for _, f in best["members"])
    return best


def air_lines_for(station, width, docs, excluded, lines):
    """The coherent lines of that station's air shots at that width, from the lines CSV: the
    strong in-band peaks whose SNR reaches the min SNR read from the CSV. lines is None or
    (settings, per) from b30_verdict.parse_lines_csv."""
    settings, per = lines
    captures = {}
    for name, doc in docs.items():
        if doc.get("campaign_kind") != "air_shot" or name in excluded:
            continue
        if doc.get("campaign_station") != station or doc.get("capture_result") not in (None, "OK"):
            continue
        w = width_ms_of(doc)
        if w is None or w != width:
            continue
        a = per.get(name)
        captures[name] = [f for f, snr in (a["peaks"] if a else []) if snr >= settings["min_snr"]]
    return b30.coherent_lines(captures)


def analyse(docs, excluded, sweep_rows, lines):
    """Everything the report says, as a dict. lines is None or (settings, per) from the lines CSV."""
    r = {"cells": {}, "stations": {}, "n_strike": 0, "unknown": [], "n_flagged": 0,
         "lines_used": lines is not None}
    strikes = {}
    for name, doc in docs.items():
        if name in excluded:
            continue
        s = strike_cell(doc)
        if s is not None:
            strikes[name] = s
            r["n_strike"] += 1
            r["stations"].setdefault(s[0], set()).add(s[2])
    for row in sweep_rows:
        s = strikes.get(row["bundle"])
        if s is None:
            if row["bundle"] not in docs:
                r["unknown"].append(row["bundle"])
            continue
        key = (s[0], s[1], row["candidate"])
        cell = r["cells"].setdefault(key, {"clears": []})
        if is_clear(row):
            cell["clears"].append((s[2], row["f1"]))
    for key in sorted(r["cells"]):
        cell = r["cells"][key]
        cell["flag"] = coincidence_of(cell["clears"])
        if cell["flag"] is not None and lines is not None:
            lines_found = air_lines_for(key[0], key[1], docs, excluded, lines)
            match = None
            for l in lines_found:
                if abs(l["freq_hz"] - cell["flag"]["common"]) <= LINE_TOL_HZ:
                    match = l
                    break
            cell["air"] = {"performed": True, "lines": lines_found, "match": match}
        else:
            cell["air"] = None
        if cell["flag"] is not None:
            r["n_flagged"] += 1
    r["stations"] = {st: sorted(sp) for st, sp in r["stations"].items()}
    return r


def render(r):
    out = ["B3.2 cross-spoke coincidence report", "",
           "Report only: a flag is a prompt to look, not a verdict. Nothing here gates, filters or",
           "removes anything, and no threshold moves. Spokes on a real wheel differ in tension, so a",
           "common f1 is suspicious, but several spokes can genuinely share a pitch."]
    if r["n_strike"] == 0:
        out.append("no B3.2 strike bundles in the bundle directory: nothing to report")
    for st in sorted(r["stations"]):
        spokes = r["stations"][st]
        out.append("station %s: strike data from %d of %d exploration spokes (spokes %s)" % (
            st, len(spokes), EXPECTED_SPOKES, ", ".join(str(s) for s in spokes)))
    out.append("")
    if r["lines_used"]:
        out.append("air lines come from the lines CSV, which is produced under the baseline chain")
        out.append("profile only, not under each candidate; a line is coherent if a peak within")
        out.append("+-%.1f Hz of it appears in at least half of that station's air shots at the width" % LINE_TOL_HZ)
    else:
        out.append("no lines CSV given: the air-shot check was not performed")
    out.append("")
    if r["unknown"]:
        out.append("sweep rows for bundles not in the bundle directory (not counted): %s" %
                   ", ".join(sorted(set(r["unknown"]))))
        out.append("")
    for key in sorted(r["cells"]):
        st, width, cand = key
        cell = r["cells"][key]
        if not cell["clears"]:
            continue
        out.append("%s / %d ms / %s" % key)
        out.append("  clears: %s" % ", ".join("sp%d %.3f Hz" % (s, f) for s, f in sorted(cell["clears"])))
        flag = cell["flag"]
        if flag is None:
            n_spokes = len(set(s for s, _ in cell["clears"]))
            out.append("  not flagged: clears from %d distinct spoke(s), a flag needs at least %d "
                       "within +-%.1f Hz" % (n_spokes, FLAG_SPOKES, LINE_TOL_HZ))
        else:
            out.append("  FLAGGED: clears from %d spokes (%s) within +-%.1f Hz of one another; common "
                       "frequency %.1f Hz (median of the members)" % (
                           len(flag["spokes"]), ", ".join(str(s) for s in flag["spokes"]),
                           LINE_TOL_HZ, flag["common"]))
            air = cell.get("air")
            if air is None:
                out.append("  air check (%s, %d ms): not performed (no lines CSV given)" % (st, width))
            elif not air["lines"]:
                out.append("  air check (%s, %d ms): no coherent air line at this station and width "
                           "(a line must recur in at least half of the air shots)" % (st, width))
            elif air["match"] is not None:
                m = air["match"]
                out.append("  air check (%s, %d ms): line at %.1f Hz in %d/%d air shots -> within +-%.1f Hz "
                           "of %.1f Hz: the air line matches the common frequency" % (
                               st, width, m["freq_hz"], m["n"], m["of"], LINE_TOL_HZ, flag["common"]))
            else:
                out.append("  air check (%s, %d ms): air line(s) at %s, none within +-%.1f Hz of %.1f Hz: "
                           "no air line matches" % (st, width,
                                                    ", ".join("%.1f" % l["freq_hz"] for l in air["lines"]),
                                                    LINE_TOL_HZ, flag["common"]))
        out.append("")
    n_with = sum(1 for c in r["cells"].values() if c["clears"])
    out.append("cells flagged: %d of %d with clears" % (r["n_flagged"], n_with))
    return "\n".join(out)


def selftest():
    st = {"docs": {}, "rows": [], "air": {}}

    def reset():
        st["docs"], st["rows"], st["air"] = {}, [], {}

    def strike(spoke, f1, status="suspect", station="RIGHT", width=72.0, cand=("baseline", "0"), stage="B3.2"):
        name = "B3.2_%s%s_strike_sp%d_%s_%d" % (cand[0], cand[1], spoke, station, len(st["docs"]))
        st["docs"][name] = {"campaign_stage": stage, "campaign_kind": "strike", "campaign_station": station,
                            "pulse_ms": width, "spoke_id": spoke, "capture_result": "OK"}
        st["rows"].append("%s,%s,%s,%s,%s,12,3,%s,1010.200,15.300,1" % (
            name, cand[0], cand[1], status, "PROVISIONAL_MODE_ID" if status == "suspect" else "LOW_SNR",
            "%.3f" % f1 if f1 is not None else ""))
        return name

    def other(kind, stage, station="RIGHT", width=72.0, spoke=3):
        name = "B3.x_%s_%s_%d" % (kind, station, len(st["docs"]))
        st["docs"][name] = {"campaign_stage": stage, "campaign_kind": kind, "campaign_station": station,
                            "pulse_ms": width, "spoke_id": spoke, "capture_result": "OK"}
        return name

    def air(i, station="RIGHT", width=72.0, peaks=((480.0, 5.0),)):
        name = "B3.2_air_%s_%d_%02d" % (station, width, i)
        st["docs"][name] = {"campaign_stage": "B3.2", "campaign_kind": "air_shot", "campaign_station": station,
                            "pulse_ms": width, "capture_result": "OK"}
        st["air"][name] = list(peaks)
        return name

    def run(excluded=()):
        d = tempfile.mkdtemp(prefix="b32_selftest_")
        with open(os.path.join(d, "index.txt"), "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(sorted(st["docs"])) + "\n")
        for n, doc in st["docs"].items():
            with open(os.path.join(d, n + ".json"), "w", encoding="utf-8") as f:
                json.dump(doc, f)
        with open(os.path.join(d, "B3.2_exclusions.jsonl"), "w", encoding="utf-8") as f:
            for b in excluded:
                f.write(json.dumps({"bundle": b}) + "\n")
        sweep_path = os.path.join(d, "sweep.csv")
        with open(sweep_path, "w", encoding="utf-8", newline="\n") as f:
            f.write("sweep dir: %s   bundles: %d\n" % (d, len(st["docs"])))
            f.write(",".join(SWEEP_HEADER) + "\n")
            f.write("\n".join(st["rows"]) + "\n")
            f.write("too,few,fields,must,be,skipped\n")                                  # wrong field count
            f.write("B3.2_ghost_strike_sp9_seq9,,0,suspect,PROVISIONAL_MODE_ID,12,3,500.0,,,,\n")  # empty axis
        lines_path = None
        if st["air"]:
            out = ["lines dir: x   bundles: %d   f1_band 350-600 Hz, prominence 6.0 dB, depth 30.0 dB, "
                   "min_snr 12.0 dB, chain_digest %s" % (len(st["air"]), "d1" * 32)]
            for name in sorted(st["air"]):
                peaks = st["air"][name]
                for k, (fq, snr) in enumerate(sorted(peaks)):
                    out.append("%s,1,2160,4080,24000,262144,%d,0,%d,%.3f,20.00,-5.00,%.2f,%d" % (
                        name, len(peaks), k, fq, snr, 1))
            lines_path = os.path.join(d, "lines.csv")
            with open(lines_path, "w", encoding="utf-8", newline="\n") as f:
                f.write("\n".join(out) + "\n")
        docs2, excl = b30.load_bundles(d)
        with open(sweep_path, encoding="utf-8") as f:
            sweep_rows = parse_sweep_csv(f.read())
        lines = b30.parse_lines_csv(open(lines_path, encoding="utf-8").read()) if lines_path else None
        return d, sweep_path, lines_path, analyse(docs2, excl, sweep_rows, lines)

    def cell1(r):
        return r["cells"][("RIGHT", 72, "baseline=0")]

    def flagged_with_air(air_kwargs):
        """A flagged RIGHT / 72 ms cell with common frequency 501.0, over the given air shots."""
        reset()
        for i, kw in enumerate(air_kwargs):
            air(i, **kw)
        strike(17, 500.0)
        strike(5, 501.0)
        strike(3, 502.5)
        return run()

    # 1. three spokes within 2 Hz of 501.0 (the fourth clear is far away): flagged, common 501.0, spokes [3, 5, 17]
    reset()
    strike(17, 500.0)
    strike(5, 501.0)
    strike(3, 502.5)
    strike(15, 470.0)
    _, _, _, r = run()
    f = cell1(r)["flag"]
    assert f is not None, "case 1: three spokes within 2 Hz must be flagged"
    assert abs(f["common"] - 501.0) < 1e-9, "case 1: the common frequency must be the median 501.0, got %s" % f["common"]
    assert f["spokes"] == [3, 5, 17], "case 1: the spokes must be [3, 5, 17], got %s" % f["spokes"]

    # 2. only 2 clears within 2 Hz of any centre: not flagged
    reset()
    strike(17, 500.0)
    strike(5, 501.0)
    strike(3, 503.5)
    _, _, _, r = run()
    assert cell1(r)["flag"] is None, "case 2: only 2 within 2 Hz of any centre must not be flagged"

    # 3. the boundary is inclusive
    reset()
    strike(17, 500.0)
    strike(5, 502.0)
    strike(3, 504.0)
    _, _, _, r = run()
    assert cell1(r)["flag"] is not None, "case 3: 500.0/502.0/504.0 spans exactly +-2 Hz: flagged"
    reset()
    strike(17, 500.0)
    strike(5, 502.0)
    strike(3, 504.01)
    _, _, _, r = run()
    assert cell1(r)["flag"] is None, "case 3: 2.01 Hz from the centre is outside the band: not flagged"

    # 4. several clears from one spoke are still one spoke
    reset()
    strike(17, 500.0)
    strike(17, 500.1)
    strike(17, 500.2)
    strike(5, 500.0)
    _, _, _, r = run()
    assert cell1(r)["flag"] is None, "case 4: 4 clears from 2 spokes must not be flagged"

    # 5. a rejected row with an f1 is not a clear
    reset()
    strike(17, 500.0)
    strike(5, 500.0)
    strike(3, 500.0, status="rejected")
    _, _, _, r = run()
    assert cell1(r)["flag"] is None, "case 5: a rejected row is not a clear"

    # 6. a bundle in the exclusion ledger is not counted
    reset()
    strike(17, 500.0)
    strike(5, 500.0)
    n3 = strike(3, 500.0)
    _, _, _, r = run(excluded=[n3])
    assert cell1(r)["flag"] is None, "case 6: a ledgered bundle is not counted"

    # 7. a cell never mixes stations, widths or candidates
    reset()
    strike(17, 500.0, width=60.0)
    strike(5, 500.0, width=72.0)
    strike(3, 500.0, width=85.0)
    _, _, _, r = run()
    assert len(r["cells"]) == 3 and all(not c["flag"] for c in r["cells"].values()), \
        "case 7: different widths are different cells"
    reset()
    strike(17, 500.0, cand=("baseline", "0"))
    strike(5, 500.0, cand=("tension", "-2"))
    strike(3, 500.0, cand=("runout", "1.5"))
    _, _, _, r = run()
    assert len(r["cells"]) == 3 and all(not c["flag"] for c in r["cells"].values()), \
        "case 7: different candidates are different cells"
    reset()
    strike(17, 500.0, station="RIGHT")
    strike(5, 500.0, station="LEFT")
    strike(3, 500.0, station="RIGHT")
    _, _, _, r = run()
    assert len(r["cells"]) == 2 and all(not c["flag"] for c in r["cells"].values()), \
        "case 7: different stations are different cells"

    # 8. an air_shot bundle, a no_fire bundle and a B3.0 strike are never counted
    for kind, stage in (("air_shot", "B3.2"), ("no_fire", "B3.2"), ("strike", "B3.0")):
        reset()
        strike(17, 500.0)
        strike(5, 501.0)
        n = other(kind, stage)
        st["rows"].append("%s,baseline,0,suspect,PROVISIONAL_MODE_ID,12,3,502.500,1010.200,15.300,1" % n)
        _, _, _, r = run()
        assert cell1(r)["flag"] is None, "case 8: a %s/%s bundle is never counted" % (kind, stage)

    # 9. the air check, per case, on a flagged RIGHT / 72 ms cell whose common frequency is 501.0
    # 9a. a coherent air line at 501.4 in 3 of 6 air shots: matched
    _, _, _, r = flagged_with_air([{"peaks": ([(501.4, 14.0)] if i < 3 else []) + [(480.0, 5.0)]}
                                   for i in range(6)])
    res = cell1(r)["air"]
    assert res["match"] is not None and abs(res["match"]["freq_hz"] - 501.4) < 1e-9, \
        "case 9a: a coherent air line at 501.4 in 3/6 must match 501.0, got %s" % res

    # 9b. the same line in 2 of 6: no air line
    _, _, _, r = flagged_with_air([{"peaks": ([(501.4, 14.0)] if i < 2 else []) + [(480.0, 5.0)]}
                                   for i in range(6)])
    res = cell1(r)["air"]
    assert res["match"] is None and not res["lines"], "case 9b: 2/6 is under half: no air line"

    # 9c. a line 2.5 Hz away in 6 of 6: listed, but not matched
    _, _, _, r = flagged_with_air([{"peaks": [(503.5, 14.0), (480.0, 5.0)]} for i in range(6)])
    res = cell1(r)["air"]
    assert res["match"] is None and res["lines"], "case 9c: the 503.5 line is listed but does not match"

    # 9d. a line at another width only: not matched
    air_kwargs = [{"width": 60.0, "peaks": [(501.4, 14.0), (480.0, 5.0)]} for _ in range(6)]
    air_kwargs += [{"width": 72.0} for _ in range(6)]
    _, _, _, r = flagged_with_air(air_kwargs)
    res = cell1(r)["air"]
    assert res["match"] is None and not res["lines"], \
        "case 9d: an air line at RIGHT/60 only must not match a RIGHT/72 cell"

    # 9e. a peak below the min SNR in 6 of 6: not matched
    _, _, _, r = flagged_with_air([{"peaks": [(501.4, 10.0), (480.0, 5.0)]} for _ in range(6)])
    res = cell1(r)["air"]
    assert res["match"] is None and not res["lines"], "case 9e: below the min SNR there is no air line"

    # 9f. no lines CSV: the air check is reported as not performed
    reset()
    strike(17, 500.0)
    strike(5, 501.0)
    strike(3, 502.5)
    _, _, lines_path, r = run()
    assert lines_path is None and cell1(r)["air"] is None, \
        "case 9f: without a lines CSV the air check is not performed"
    rep = render(r)
    assert "not performed" in rep, "case 9f: the report must say the air check was not performed"

    # 10. no PASS/FAIL anywhere, exit code 0 either way, inputs untouched
    reset()
    for i in range(3):
        air(i, peaks=[(501.4, 14.0), (480.0, 5.0)])
    for i in range(3, 6):
        air(i)
    strike(17, 500.0)
    strike(5, 501.0)
    strike(3, 502.5)
    d, sweep_path, lines_path, r = run()
    assert cell1(r)["flag"] is not None, "case 10: the fixture cell must be flagged"
    before = {fn: hashlib.sha256(open(os.path.join(d, fn), "rb").read()).hexdigest()
              for fn in sorted(os.listdir(d)) if os.path.isfile(os.path.join(d, fn))}
    for argv in (["--dir", d, "--sweep", sweep_path, "--lines", lines_path], ["--dir", d, "--sweep", sweep_path]):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = main(argv)
        assert rc == 0, "case 10: main() must return 0, got %s" % rc
        assert "PASS" not in buf.getvalue() and "FAIL" not in buf.getvalue(), \
            "case 10: the report must not say PASS or FAIL"
    after = {fn: hashlib.sha256(open(os.path.join(d, fn), "rb").read()).hexdigest()
             for fn in sorted(os.listdir(d))}
    assert after == before, "case 10: the input files must be byte-identical after a run"

    # 11. empty data and partial spoke coverage are reported, not errors
    reset()
    for i in range(2):
        air(i)
    _, _, _, r = run()
    rep = render(r)
    assert r["n_strike"] == 0 and "no B3.2 strike bundles" in rep, "case 11: no strike bundles must be said"
    reset()
    strike(17, 500.0)
    strike(5, 500.0)
    _, _, _, r = run()
    rep = render(r)
    assert "strike data from 2 of 4 exploration spokes" in rep, \
        "case 11: only 2 of 4 spokes present must be said, got: %s" % [l for l in rep.splitlines() if "spokes" in l]

    print("selftest ok: flag needs 3 distinct spokes within +-2 Hz (inclusive, ties to the lowest centre), "
          "common = median, one spoke counted once, rejected/ledgered rows and non-strike bundles never count, "
          "cells never mix station/width/candidate, air lines at the same station and width only (from the "
          "baseline-profile lines CSV), no PASS/FAIL, exit 0, inputs untouched, empty and partial data reported")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dir", help="the campaign bundle directory (has index.txt)")
    ap.add_argument("--sweep", help="the sweep CSV (native_sweep output, TRUING_SWEEP_MODE unset)")
    ap.add_argument("--lines", help="the lines CSV (baseline chain profile only); without it the air "
                                    "check is reported as not performed")
    ap.add_argument("--out", help="also write the report here")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args(argv)
    if args.selftest:
        selftest()
        return 0
    if not args.dir or not args.sweep:
        ap.error("--dir and --sweep are required")
    docs, excluded = b30.load_bundles(args.dir)
    with open(args.sweep, encoding="utf-8", errors="replace") as f:
        sweep_rows = parse_sweep_csv(f.read())
    lines = None
    if args.lines:
        with open(args.lines, encoding="utf-8", errors="replace") as f:
            lines = b30.parse_lines_csv(f.read())
    report = render(analyse(docs, excluded, sweep_rows, lines))
    print(report)
    if args.out:
        with open(args.out, "w", encoding="utf-8", newline="\n") as f:
            f.write(report + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
