"""B3.2 selection: apply the registered rules to the strike data and say what they select.

The rules are the ones in docs/SOLENOID_CAMPAIGN.md, "B3.2 pre-registration" ("Vocabulary, made
operational", "Candidate set", "Selection, made operational", "Gates", "Blind review", "Reported, not
gating") and its Amendments 1 and 2. Read those first. This tool decides nothing the registration does
not: where the text admits more than one mechanical reading, the reading used is listed in the record under
"Readings fixed for the selection tool" (committed before any strike) and marked (reading) where it is applied.

  1  a candidate is admissible: 0 false clears across every control set, and no selection shift
  2  a cell (candidate, station, level) satisfies its constraints: 0 inconsistent clears, 0 false clears
     among its own 6 air shots, 100 % attribution. S(c, s) = the levels whose cell does. c is eligible
     only if S(c, LEFT) and S(c, RIGHT) are both non-empty; none eligible -> G-analysis, halt
  3  score(c) = the smaller over the two stations of the largest worst-spoke rate over S(c, s)
  4  the level(s): a shared level if one is within 10 pp of the best at both stations, else each station's
  5  tie-breaks; 6 refinement trigger (reported, never applied); 7 class check (G-class, halt)
  8  SNR per level (reported, used only as a tie-break)

    python tools/b32_select.py --plan docs/campaign_plans/b32.json \\
        --dir <_campaign> --sweep sweep_campaign.csv [--dir <captures> --sweep sweep_fixtures.csv] [--lines lines.csv]
    python tools/b32_select.py --plan docs/campaign_plans/b32.json --dir <_campaign> --draw
    python tools/b32_select.py --selftest

The sweep CSVs are `pio test -e native_sweep -v` (TRUING_SWEEP_MODE unset) over each directory, with the
onset floor as it is. The lines CSV is the same run with TRUING_SWEEP_MODE=lines. Stdlib only.

Selection is computed only when every planned trial has exactly one kept bundle and every needed sweep row
exists; otherwise the report says INCOMPLETE and lists why, and still prints what can be counted. (reading) The
record's "all 224 trials" predates Amendment 1's air shots: it is read as all 266 planned trials. The draw for
the blind review needs the same trial completeness (it needs no sweep rows).
"""
import argparse
import json
import math
import os
import random
import statistics
import sys
from fractions import Fraction

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import b30_verdict as b30          # noqa: E402  (load_bundles, parse_lines_csv)
import b32_crossspoke as xs        # noqa: E402  (parse_sweep_csv, is_clear)
import campaign_sequence as seq    # noqa: E402  (plan_sha256, station_for_spoke)

STATIONS = ("LEFT", "RIGHT")
REPS = 6                        # strikes per (spoke, level); the denominator of every rate ("n stays 6")
BAND_HZ = b30.LINE_TOL_HZ       # consistency band and "the same f1": +-2 Hz, inclusive
MIN_TO_JUDGE = 3                # a spoke with fewer clears in a condition is unjudged
PULSE_TOL_MS = 0.01 + 1e-9      # attribution: pulse_ms equals the requested level to 0.01 ms (float slack only)
SHIFT_HZ = 2.0                  # selection shift: both clear and f1 differ by MORE than this
SHARED_TOL = Fraction(1, 10)    # 10 pp
GATE_W = Fraction(5, 6)         # exploration gate
CLASS_GAP = Fraction(1, 10)     # 10 pp
REFINE_GAP = Fraction(1, 4)     # 25 pp
CLASS_SNR_MIN = 6               # per-class SNR only where a class has at least this many captures
BRACKET = {"LEFT": (40, 85), "RIGHT": (60, 95)}     # the M3 brackets
NOISE_BAND = (355.0, 395.0)     # B3.0's quiet-control lines at the gate
DRAW_SEED = 20260919
DRAW_PER_LEVEL = {"LEFT": 3, "RIGHT": 4}
CONTROL_EXPECTED = {"B3.0": 40, "pass-C": 18, "ambient": 1}   # the pooled control sets besides B3.2's no-fire
REFUSED = ("OUT_OF_RANGE", "INIT_REFUSED")

GATE, WIN, PROM = (100, 200, 300, 500, 800), (250, 750), (9, 12)


def registered_candidates():
    """The 34 candidates in tie-break order: baseline; the 9 single-field rows (gate ascending, then window,
    then prominence); the 24 two-field rows (gate x window, gate x prominence, window x prominence)."""
    c = ["baseline=0"]
    c += ["gate_start_ms=%d" % g for g in GATE] + ["window_ms=%d" % w for w in WIN]
    c += ["prominence_db=%d" % p for p in PROM]
    c += ["gate_start_ms+window_ms=%d+%d" % (g, w) for g in GATE for w in WIN]
    c += ["gate_start_ms+prominence_db=%d+%d" % (g, p) for g in GATE for p in PROM]
    c += ["window_ms+prominence_db=%d+%d" % (w, p) for w in WIN for p in PROM]
    return c


CANDIDATES = registered_candidates()


def fields_changed(label):
    return 0 if label.startswith("baseline") else label.split("=")[0].count("+") + 1


def spoke_class(spoke):
    """The rig's period-4 spoke classes (plan, Amendment 1): 0 LEFT-leading, 1 RIGHT-leading, 2 LEFT-trailing,
    3 RIGHT-trailing. Within a station the class is leading or trailing."""
    return "lead" if spoke % 4 in (0, 1) else "trail"


def finite(v):
    try:
        return v is not None and math.isfinite(float(v))
    except (TypeError, ValueError):
        return False


def mhz(x):
    """An f1 as an exact rational in Hz. The sweep prints three decimals, so the value IS a whole number of
    millihertz; comparing the floats would misread a 2.000 Hz difference as 2.000000000000057 (510.003 vs 512.003)."""
    return Fraction(round(float(x) * 1000), 1000)


def pp(fr):
    return "%.0f%%" % (100 * float(fr))


# ---------------------------------------------------------------------------------------------------
# what the plan says
def plan_trials(plan):
    """{global trial number: trial} exactly as the runner numbers them (1-based, block order)."""
    out, n = {}, 0
    for b in plan["blocks"]:
        for t in b["trials"]:
            n += 1
            out[n] = dict(t, block=b["name"], station=seq.station_for_spoke(t["spoke"]))
    return out


def attribution_ok(doc, t):
    """100 % attribution, per strike record: fired, the station derived from the declared spoke, capture_result
    OK, pulse_ms equal to the requested level to 0.01 ms."""
    st = seq.station_for_spoke(t["spoke"])
    if doc.get("fired") is not True or doc.get("capture_result") != "OK":
        return False
    if doc.get("spoke_id") != t["spoke"] or doc.get("campaign_station") != st:
        return False
    if doc.get("station") != "acoustic_%s" % st.lower():      # the board's own record; absent is not attributed
        return False
    try:
        return abs(float(doc.get("pulse_ms")) - t["pulse_ms"]) <= PULSE_TOL_MS
    except (TypeError, ValueError):
        return False


def control_set(name, doc):
    """Which pooled control set a bundle belongs to, or None. B3.2's own no-fire controls come from the plan."""
    stage, kind = doc.get("campaign_stage"), doc.get("campaign_kind")
    if stage == "B3.0" and kind in ("no_fire", "air_shot"):
        return "B3.0"
    if name.startswith("C_"):
        return "pass-C"
    if name == "nano_ambient_ambiguous":
        return "ambient"
    return None


def median_snr(docs_of):
    """(reading) "the strike captures that produced a spectrum": the board records an SNR only when its baseline
    analysis found an f1, so this is the captures that carry an SNR."""
    v = sorted(float(d["expect_snr_db"]) for d in docs_of if finite(d.get("expect_snr_db")))
    return (statistics.median(v), len(v)) if v else (None, 0)


# ---------------------------------------------------------------------------------------------------
def judge_spoke(clears):
    """clears: [f1] of one spoke in one condition. -> (consistent, inconsistent, unjudged). The median is over
    the spoke's clears in the condition INCLUDING the clear under test; fewer than 3 clears is unjudged."""
    if len(clears) < MIN_TO_JUDGE:
        return 0, 0, len(clears)
    fs = [mhz(f) for f in clears]
    med = statistics.median(fs)
    ok = sum(1 for f in fs if abs(f - med) <= Fraction(BAND_HZ))
    return ok, len(clears) - ok, 0


def interior(levels, S, level):
    """Both neighbours in the tested list exist and are in S."""
    i = levels.index(level)
    return 0 < i < len(levels) - 1 and levels[i - 1] in S and levels[i + 1] in S


def pick_levels(levels, S, W, snr):
    """Rule 4 with the rule 5 level tie-breaks. S[st]: levels in S; W[st][l]: worst-spoke rate; snr[st][l]:
    median SNR or None. -> ({station: level}, shared?). A shared level is one in both S with W within 10 pp
    of that station's best at both. Ties: plateau interior (count of stations), higher SNR (reading: the lower
    of the two stations' medians for a shared level), shorter pulse."""
    best = {st: max(W[st][l] for l in S[st]) for st in STATIONS}

    def sn(st, l):
        v = snr[st].get(l)
        return v if v is not None else float("-inf")

    shared = [l for l in S["LEFT"] if l in S["RIGHT"] and all(best[st] - W[st][l] <= SHARED_TOL for st in STATIONS)]
    if shared:
        l = min(shared, key=lambda x: (-sum(interior(levels[st], S[st], x) for st in STATIONS),
                                       -min(sn(st, x) for st in STATIONS), x))
        return {st: l for st in STATIONS}, True
    out = {}
    for st in STATIONS:
        cand = [l for l in S[st] if W[st][l] == best[st]]
        out[st] = min(cand, key=lambda x: (-interior(levels[st], S[st], x), -sn(st, x), x))
    return out, False


def analyse(docs, excluded, sweep_rows, plan, ledger=(), lines=None):
    """Everything the report says, as a dict. docs: {bundle: doc}. excluded: ledgered bundle names.
    sweep_rows: xs.parse_sweep_csv rows. ledger: the exclusion ledger entries. lines: (settings, per) or None."""
    plan_sha = seq.plan_sha256(plan)
    trials = plan_trials(plan)
    levels = {st: [int(x) for x in plan["levels"][st]] for st in STATIONS}
    R = {"plan_sha": plan_sha, "incomplete": [], "notes": [], "levels": levels}
    inc = R["incomplete"]

    # --- which kept bundle is which planned trial
    kept = {}
    for name, doc in docs.items():
        if name in excluded or doc.get("campaign_stage") != "B3.2":
            continue
        if doc.get("campaign_plan_sha256") != plan_sha:
            inc.append("bundle %s carries another plan (%s)" % (name, str(doc.get("campaign_plan_sha256"))[:12]))
            continue
        kept.setdefault(doc.get("campaign_trial"), []).append(name)
    strike, air, nofire = {}, {}, {}
    for n, t in trials.items():
        names = kept.get(n, [])
        if len(names) != 1:
            inc.append("trial %d (%s, spoke %d, %d ms) has %d kept bundle(s), needs 1" % (
                n, t["kind"], t["spoke"], t["pulse_ms"], len(names)))
            continue
        doc = docs[names[0]]
        if doc.get("campaign_kind") != t["kind"]:
            inc.append("trial %d is planned as %s but %s says %s" % (n, t["kind"], names[0], doc.get("campaign_kind")))
            continue
        rec = {"name": names[0], "doc": doc, "trial": n, "station": t["station"], "level": t["pulse_ms"],
               "spoke": t["spoke"], "ok": attribution_ok(doc, t) if t["kind"] == "strike" else True}
        if t["kind"] != "strike" and doc.get("capture_result") not in (None, "OK"):
            inc.append("kept bundle %s has capture_result %s" % (names[0], doc.get("capture_result")))
        {"strike": strike, "air_shot": air, "no_fire": nofire}[t["kind"]][n] = rec
    R["n_strike"], R["n_air"], R["n_nofire"] = len(strike), len(air), len(nofire)

    ctrl = {"B3.2 no-fire": [r["name"] for r in nofire.values()]}
    for name, doc in docs.items():
        s = control_set(name, doc)
        if s and name not in excluded:
            ctrl.setdefault(s, []).append(name)
    for s, want in CONTROL_EXPECTED.items():
        if len(ctrl.get(s, [])) != want:
            inc.append("control set %s has %d kept bundle(s), the registration counts %d" % (s, len(ctrl.get(s, [])), want))
    R["control_sizes"] = {s: len(v) for s, v in ctrl.items()}

    # --- the plan's cells
    spokes = {st: sorted({t["spoke"] for t in trials.values() if t["kind"] == "strike" and t["station"] == st})
              for st in STATIONS}
    R["spokes"] = spokes
    air_per = int(plan.get("air_per_level", 0))
    cellstrikes = {(st, l, sp): [] for st in STATIONS for l in levels[st] for sp in spokes[st]}
    for r in strike.values():
        cellstrikes.get((r["station"], r["level"], r["spoke"]), []).append(r)
    cellair = {(st, l): [] for st in STATIONS for l in levels[st]}
    for r in air.values():
        cellair.get((r["station"], r["level"]), []).append(r)
    cellattr = {}
    for st in STATIONS:
        for l in levels[st]:
            rs = [r for sp in spokes[st] for r in cellstrikes[(st, l, sp)]]
            cellattr[(st, l)] = bool(rs) and all(r["ok"] for r in rs)
            for sp in spokes[st]:
                if len(cellstrikes[(st, l, sp)]) != REPS:
                    inc.append("%s spoke %d at %d ms has %d kept strikes, the plan fixes %d" % (
                        st, sp, l, len(cellstrikes[(st, l, sp)]), REPS))
            if air_per and len(cellair[(st, l)]) != air_per:
                inc.append("%s air shots at %d ms: %d kept, the plan fixes %d" % (st, l, len(cellair[(st, l)]), air_per))

    # --- the sweep rows every bundle needs
    rows, conflicts = {}, []
    for row in sweep_rows:
        prev = rows.setdefault((row["bundle"], row["candidate"]), row)
        if prev is not row and (prev["status"], prev["f1"]) != (row["status"], row["f1"]):
            conflicts.append("%s / %s" % (row["bundle"], row["candidate"]))
    if conflicts:
        inc.append("%d sweep row(s) appear twice with different results (first %s)" % (len(conflicts), conflicts[0]))
    used = [r["name"] for r in list(strike.values()) + list(air.values())] + [n for v in ctrl.values() for n in v]
    used = sorted(set(used))
    status = {}
    for lab in CANDIDATES:
        got = [rows.get((b, lab)) for b in used]
        refused = any(r is not None and r["status"] in REFUSED for r in got)
        missing = sum(1 for r in got if r is None)
        status[lab] = "not_evaluable" if refused else ("missing" if missing else "ok")
        if status[lab] == "missing":
            inc.append("candidate %s has no sweep row for %d of %d bundles" % (lab, missing, len(used)))
    R["cand"] = {}
    R["snr"] = {}
    for st in STATIONS:
        for l in levels[st]:
            rs = [r["doc"] for sp in spokes[st] for r in cellstrikes[(st, l, sp)]]
            R["snr"][(st, l)] = {"all": median_snr(rs)}
            for cl in ("lead", "trail"):
                sub = [r["doc"] for sp in spokes[st] if spoke_class(sp) == cl for r in cellstrikes[(st, l, sp)]]
                m = median_snr(sub)
                if m[1] >= CLASS_SNR_MIN:
                    R["snr"][(st, l)][cl] = m
    R["complete"] = not inc
    R["reported"] = report_counts(docs, excluded, strike, ledger, plan_sha, levels)
    R["air_lines"] = air_line_report(air, lines) if lines else None
    if inc:
        R["halt"], R["chosen"] = None, None
        return R

    def clears(name, lab):
        return xs.is_clear(rows[(name, lab)])

    def f1(name, lab):
        return rows[(name, lab)]["f1"]

    snr = {st: {l: R["snr"][(st, l)]["all"][0] for l in levels[st]} for st in STATIONS}
    for lab in CANDIDATES:
        c = R["cand"][lab] = {"label": lab, "status": status[lab], "fields": fields_changed(lab),
                              "index": CANDIDATES.index(lab), "reasons": [], "eligible": False}
        if status[lab] == "not_evaluable":
            continue
        fc = {s: sum(1 for n in v if clears(n, lab)) for s, v in ctrl.items()}
        c["false_by_set"] = fc
        if sum(fc.values()):
            c["reasons"].append("false clears on controls: " + ", ".join("%s %d" % (s, k) for s, k in sorted(fc.items()) if k))
        shifts = [r["name"] for r in strike.values() if lab != "baseline=0" and clears(r["name"], "baseline=0")
                  and clears(r["name"], lab) and abs(mhz(f1(r["name"], lab)) - mhz(f1(r["name"], "baseline=0"))) > Fraction(SHIFT_HZ)]
        c["shifts"] = shifts
        if shifts:
            c["reasons"].append("selection shift on %d strike(s), first %s" % (len(shifts), shifts[0]))
        c["admissible"] = not c["reasons"]
        cells = c["cells"] = {st: {} for st in STATIONS}
        for st in STATIONS:
            for l in levels[st]:
                per = {}
                inconsistent = unjudged = 0
                for sp in spokes[st]:
                    cl = [f1(r["name"], lab) for r in cellstrikes[(st, l, sp)] if clears(r["name"], lab)]
                    ok, bad, un = judge_spoke(cl)
                    per[sp] = Fraction(ok, REPS)
                    inconsistent += bad
                    unjudged += un
                airfalse = sum(1 for r in cellair[(st, l)] if clears(r["name"], lab))
                cw = {k: min(per[sp] for sp in spokes[st] if spoke_class(sp) == k) for k in ("lead", "trail")}
                cells[st][l] = {"per_spoke": per, "W": min(per.values()), "class_W": cw, "inconsistent": inconsistent,
                                "unjudged": unjudged, "air_false": airfalse, "attribution": cellattr[(st, l)],
                                "ok": inconsistent == 0 and airfalse == 0 and cellattr[(st, l)]}
        c["S"] = {st: [l for l in levels[st] if cells[st][l]["ok"]] for st in STATIONS}
        if not c["admissible"]:
            continue
        c["eligible"] = all(c["S"][st] for st in STATIONS)
        if not c["eligible"]:
            c["reasons"].append("no admissible cell at " + " and ".join(st for st in STATIONS if not c["S"][st]))
            continue
        W = {st: {l: cells[st][l]["W"] for l in levels[st]} for st in STATIONS}
        c["best"] = {st: max(W[st][l] for l in c["S"][st]) for st in STATIONS}
        c["score"] = min(c["best"].values())
        c["levels_chosen"], c["shared"] = pick_levels(levels, c["S"], W, snr)
        ch = c["levels_chosen"]
        c["tie_key"] = (-c["score"], -sum(interior(levels[st], c["S"][st], ch[st]) for st in STATIONS), c["fields"],
                        -min(snr[st][ch[st]] if snr[st][ch[st]] is not None else float("-inf") for st in STATIONS),
                        sum(ch.values()), c["index"])
    eligible = [c for c in R["cand"].values() if c["eligible"]]
    R["n_eligible"] = len(eligible)
    if not eligible:
        R["halt"], R["chosen"] = "G-analysis", None
        return R
    top = min(eligible, key=lambda c: c["tie_key"])
    R["chosen"] = top["label"]
    R["tied_on_score"] = [c["label"] for c in sorted(eligible, key=lambda c: c["tie_key"]) if c["score"] == top["score"]]
    R["halt"] = None

    # exploration gate: each station needs an eligible candidate with a cell in S at W >= 5/6
    R["gate"] = {}
    for st in STATIONS:
        best = max((c["cells"][st][l]["W"] for c in eligible for l in c["S"][st]), default=Fraction(0))
        R["gate"][st] = {"best_W": best, "passed": best >= GATE_W}
    # class check (rule 7) at the chosen candidate
    R["class_check"] = {}
    for st in STATIONS:
        gaps = []
        for l in top["S"][st]:
            cw = top["cells"][st][l]["class_W"]
            gaps.append((l, abs(cw["lead"] - cw["trail"]) > CLASS_GAP))
        R["class_check"][st] = {"levels": gaps, "g_class": all(g for _, g in gaps)}
    if any(v["g_class"] for v in R["class_check"].values()):
        R["halt"] = "G-class"
    R["refinement"] = refinement(top, levels)
    b30_nofire = [n for n in ctrl.get("B3.0", []) if docs[n].get("campaign_kind") == "no_fire"]
    R["noise_band"] = noise_band(top["label"], strike, nofire, b30_nofire, rows)
    return R


def refinement(top, levels):
    """Rule 6, reported and never applied: the trigger, and the whole-ms midpoint(s) that would be added.
    An odd sum has two whole-ms midpoints; both are listed and the lead picks when it registers the level."""
    out = {}
    for st in STATIONS:
        ch = top["levels_chosen"][st]
        lo, hi = BRACKET[st]
        tested = levels[st]
        trig = []
        if ch == tested[0] and tested[0] != lo:
            trig.append(("edge", tested[0], lo))
        if ch == tested[-1] and tested[-1] != hi:
            trig.append(("edge", tested[-1], hi))
        for a, b in zip(tested, tested[1:]):
            if abs(top["cells"][st][a]["W"] - top["cells"][st][b]["W"]) > REFINE_GAP:
                trig.append(("gap", a, b))
        mids = []
        for kind, a, b in trig:
            s = a + b
            mids.append((kind, a, b, [s // 2] if s % 2 == 0 else [s // 2, s // 2 + 1]))
        out[st] = {"chosen_level": ch, "triggers": mids}
    return out


def noise_band(lab, strike, nofire, b30_nofire, rows):
    lo, hi = NOISE_BAND
    out = {}
    for cand in ("baseline=0", lab):
        def count(names):
            cl = [rows[(n, cand)] for n in names if xs.is_clear(rows[(n, cand)])]
            return len(cl), sum(1 for r in cl if lo <= r["f1"] <= hi)
        out[cand] = {"strikes": count([r["name"] for r in strike.values()]),
                     "B3.2 no-fire": count([r["name"] for r in nofire.values()]),
                     "B3.0 no-fire": count(b30_nofire)}
    return out


def report_counts(docs, excluded, strike, ledger, plan_sha, levels):
    """Reported, not gating: per (station, level) strikes kept, exclusions by code, overruns, truncations."""
    out = {}
    for st in STATIONS:
        for l in levels[st]:
            rs = [r for r in strike.values() if r["station"] == st and r["level"] == l]
            trunc = {}
            for r in rs:
                k = r["doc"].get("expect_window_truncated_by") or "not reported"
                trunc[k] = trunc.get(k, 0) + 1
            codes = {}
            for e in ledger:
                if e.get("plan_sha256") == plan_sha and e.get("kind") == "strike" and e.get("pulse_ms") == l \
                        and seq.station_for_spoke(e.get("spoke", 0)) == st:
                    codes[e["code"]] = codes.get(e["code"], 0) + 1
            over = sum(1 for r in rs if (r["doc"].get("capture_overrun_events") or 0) > 0)
            # an overrun is excluded by CAPTURE_NOT_OK, so the ones that matter are among the excluded bundles
            over_ex = sum(1 for n, d in docs.items() if n in excluded and d.get("campaign_stage") == "B3.2"
                          and d.get("campaign_kind") == "strike" and d.get("campaign_station") == st
                          and d.get("pulse_ms") is not None and round(float(d["pulse_ms"])) == l
                          and (d.get("capture_overrun_events") or 0) > 0)
            out[(st, l)] = {"kept": len(rs), "excluded": codes, "overruns": over, "overruns_excluded": over_ex,
                            "truncated_by": trunc}
    return out


def air_line_report(air, lines):
    """Per air group (station, width): captures with an in-band line at or above the gate, and the lines, as
    B3.0 reported them, with no recurrence rule."""
    settings, per = lines
    if "min_snr" not in settings:
        return {"error": "the lines CSV has no f1_band / min_snr header"}
    groups = {}
    for r in air.values():
        groups.setdefault((r["station"], r["level"]), []).append(r["name"])
    out = {}
    for key, names in sorted(groups.items()):
        with_line, found, unread = 0, [], 0
        for n in sorted(names):
            a = per.get(n)
            if a is None or a["note"] or a["n_strong"] is None:
                unread += 1                     # absent from the lines CSV, or a note row: no spectrum, not "no line"
                continue
            pk = [(f, s) for f, s in a["peaks"] if s >= settings["min_snr"]]
            with_line += 1 if pk else 0
            found += pk
        out[key] = {"n": len(names), "with_line": with_line, "lines": sorted(found), "unreadable": unread}
    return out


# ---------------------------------------------------------------------------------------------------
def draw_review(strike_trials):
    """The blind-review draw. strike_trials: [(trial number, station, level)] of KEPT strikes. One draw per
    stratum (station, level): LEFT 3 per level, RIGHT 4 per level, over the stratum's sorted trial numbers.
    (reading) ONE random.Random(20260919) consumed across the strata in a fixed order (LEFT then RIGHT, levels
    ascending), using `sample`. A fresh generator per stratum was rejected: every stratum has the same pool size,
    so each would draw the same pool POSITIONS, and a pool sorted by trial number is in block order, so the same
    blocks (spokes) would be drawn in every stratum and one spoke per station could never be reviewed.
    -> {(station, level): [trial numbers, sorted]}."""
    strata = {}
    for n, st, l in strike_trials:
        strata.setdefault((st, l), []).append(n)
    rng = random.Random(DRAW_SEED)
    out = {}
    for (st, l), pool in sorted(strata.items(), key=lambda kv: (STATIONS.index(kv[0][0]), kv[0][1])):
        pool = sorted(pool)
        k = DRAW_PER_LEVEL[st]
        if len(pool) < k:
            raise SystemExit("stratum %s %d ms has %d kept strikes, the draw needs %d" % (st, l, len(pool), k))
        out[(st, l)] = sorted(rng.sample(pool, k))
    return out


def kept_strikes(docs, excluded, plan):
    """[(trial, station, level)] of kept strikes, or (None, [why]) if any planned strike trial is not kept once."""
    sha, trials, why, kept = seq.plan_sha256(plan), plan_trials(plan), [], {}
    for name, doc in docs.items():
        if name not in excluded and doc.get("campaign_stage") == "B3.2" and doc.get("campaign_plan_sha256") == sha:
            kept.setdefault(doc.get("campaign_trial"), []).append(name)
    out = []
    for n, t in trials.items():
        if t["kind"] != "strike":
            continue
        if len(kept.get(n, [])) != 1 or docs[kept[n][0]].get("campaign_kind") != "strike":
            why.append("strike trial %d has %d kept bundle(s)" % (n, len(kept.get(n, []))))
        else:
            out.append((n, t["station"], t["pulse_ms"]))
    return (None, why) if why else (out, [])


# ---------------------------------------------------------------------------------------------------
def render(R):
    o = ["B3.2 selection report", "plan sha256 %s" % R["plan_sha"], ""]
    o.append("kept trials: %d strikes, %d air shots, %d no-fire; control sets %s" % (
        R["n_strike"], R["n_air"], R["n_nofire"], ", ".join("%s %d" % kv for kv in sorted(R["control_sizes"].items()))))
    if not R["complete"]:
        o.append("")
        o.append("SELECTION: NOT COMPUTED. The data are incomplete (%d finding(s)); the rules apply to the data as it" % len(R["incomplete"]))
        o.append("stands when every planned trial is kept.")
        o += ["  INCOMPLETE  " + m for m in R["incomplete"][:25]]
        if len(R["incomplete"]) > 25:
            o.append("  ... and %d more" % (len(R["incomplete"]) - 25))
    else:
        o.append("")
        o.append("%-34s %-14s %s" % ("candidate (registered order)", "state", "detail"))
        for lab in CANDIDATES:
            c = R["cand"][lab]
            if c["status"] == "not_evaluable":
                o.append("%-34s %-14s %s" % (lab, "not evaluable", "the validator refuses this chain profile"))
            elif c["eligible"]:
                o.append("%-34s %-14s score %s (LEFT best %s, RIGHT best %s), levels %s%s" % (
                    lab, "eligible", pp(c["score"]), pp(c["best"]["LEFT"]), pp(c["best"]["RIGHT"]),
                    "/".join("%s %d" % (st[0], c["levels_chosen"][st]) for st in STATIONS),
                    " (shared)" if c["shared"] else ""))
            else:
                o.append("%-34s %-14s %s" % (lab, "admissible" if c["admissible"] else "inadmissible", "; ".join(c["reasons"]) or "-"))
        o.append("")
        if R["halt"] == "G-analysis":
            o.append("HALT G-ANALYSIS: no candidate is eligible. A per-station analysis profile is an architecture decision.")
        else:
            c = R["cand"][R["chosen"]]
            if c["score"] == 0:
                o.append("NO SELECTION: the best score is 0. No candidate produced a consistent clear at both stations; %s is only" % R["chosen"])
                o.append("the tie-break winner among %d candidates that never cleared, and the exploration gate below is not met." % len(R["tied_on_score"]))
            else:
                o.append("CHOSEN candidate: %s   score %s   (%d eligible; tied on score before tie-breaks: %s)" % (
                    R["chosen"], pp(c["score"]), R["n_eligible"], ", ".join(R["tied_on_score"])))
            for st in STATIONS:
                lv = c["levels_chosen"][st]
                cell = c["cells"][st][lv]
                o.append("  %-5s level %d ms%s   worst spoke %s   per spoke %s   S = %s" % (
                    st, lv, " (shared)" if c["shared"] else "", pp(cell["W"]),
                    ", ".join("sp%d %s" % (sp, pp(v)) for sp, v in sorted(cell["per_spoke"].items())),
                    "/".join(str(x) for x in c["S"][st])))
            o.append("")
            o.append("Exploration gate (each station needs an eligible candidate with a cell in S at worst spoke >= 5/6):")
            for st in STATIONS:
                g = R["gate"][st]
                o.append("  %-5s best %s  %s" % (st, pp(g["best_W"]), "passed" if g["passed"] else "NOT MET: one bounded mechanical lever, then stop"))
            o.append("Class check at the chosen candidate:")
            for st in STATIONS:
                cc = R["class_check"][st]
                o.append("  %-5s %s%s" % (st, ", ".join("%d ms %s" % (l, "class gap" if g else "no gap") for l, g in cc["levels"]),
                                          "   -> HALT G-CLASS" if cc["g_class"] else ""))
            o.append("Refinement (rule 6; reported, never applied: it needs a new plan and its own registration commit. At")
            o.append("most one extra level per station is allowed; which trigger to use is not decided here):")
            for st in STATIONS:
                rf = R["refinement"][st]
                if not rf["triggers"]:
                    o.append("  %-5s no trigger at the chosen level %d ms" % (st, rf["chosen_level"]))
                for kind, a, b, mids in rf["triggers"]:
                    o.append("  %-5s %s trigger between %d and %d ms: whole-ms midpoint %s" % (
                        st, kind, a, b, " or ".join(str(m) for m in mids)))
            o.append("Unjudged clears (a spoke with under 3 clears in a condition; they add 0 to its rate), by level:")
            for cand in (("baseline=0", R["chosen"]) if R["chosen"] != "baseline=0" else ("baseline=0",)):
                cc = R["cand"][cand]
                if "cells" in cc:
                    o.append("  %-28s %s" % (cand, "; ".join("%s %s" % (st, ", ".join(
                        "%d ms: %d" % (l, cc["cells"][st][l]["unjudged"]) for l in R["levels"][st])) for st in STATIONS)))
            o.append("Noise-band report (strike clears with f1 in %.0f-%.0f Hz / clears; reported, not a threshold):" % NOISE_BAND)
            for cand, d in R["noise_band"].items():
                o.append("  %-28s strikes %d/%d   B3.2 no-fire %d/%d   B3.0 no-fire %d/%d" % (
                    cand, d["strikes"][1], d["strikes"][0], d["B3.2 no-fire"][1], d["B3.2 no-fire"][0],
                    d["B3.0 no-fire"][1], d["B3.0 no-fire"][0]))
    o.append("")
    o.append("Median SNR per level (dB; the board's own expect_snr_db, recorded only where its baseline analysis found an f1; n):")
    for (st, l), d in sorted(R["snr"].items()):
        a = d["all"]
        o.append("  %-5s %d ms  %s  n=%d%s" % (st, l, "%.1f" % a[0] if a[0] is not None else "none", a[1],
                                              "".join("   %s %.1f (n=%d)" % (k, v[0], v[1]) for k, v in d.items() if k != "all")))
    o.append("Reported, not gating (per station and level):")
    for (st, l), d in sorted(R["reported"].items()):
        o.append("  %-5s %d ms  kept %d  exclusions %s  overruns kept %d / excluded %d  window truncated by %s" % (
            st, l, d["kept"], d["excluded"] or "none", d["overruns"], d["overruns_excluded"],
            ", ".join("%s %d" % kv for kv in sorted(d["truncated_by"].items())) or "n/a"))
    if R["air_lines"] is None:
        o.append("Air groups, in-band lines: not reported (no --lines CSV given)")
    elif "error" in R["air_lines"]:
        o.append("Air groups, in-band lines: not reported (%s)" % R["air_lines"]["error"])
    else:
        o.append("Air groups, in-band lines at or above the gate (no recurrence rule):")
        for (st, w), d in R["air_lines"].items():
            o.append("  %-5s %d ms  %d of %d captures have a line%s: %s" % (
                st, w, d["with_line"], d["n"], " (%d with no readable spectrum in the lines CSV)" % d["unreadable"]
                if d["unreadable"] else "", ", ".join("%.1f Hz @ %.1f dB" % l for l in d["lines"]) or "none"))
    o.append("")
    o.append("This report applies the registered rules and changes no constant. A choice here is a candidate for the")
    o.append("B3.3 freeze, which is halt-and-ask; a flagged cell in the cross-spoke report (tools/b32_crossspoke.py)")
    o.append("holds that approval until the operator has looked at it.")
    return "\n".join(o)


def render_draw(d):
    o = ["Blind-review draw: one random.Random(%d) across the strata (LEFT then RIGHT, levels ascending), sample over "
         "sorted kept-strike trial numbers (Python %s)" % (DRAW_SEED, ".".join(str(x) for x in sys.version_info[:3])),
         "Drawn from trial metadata only; no machine verdict was read. Keep it sealed until the responses are filed."]
    for (st, l), tr in sorted(d.items()):
        o.append("  %-5s %d ms  %s" % (st, l, " ".join(str(t) for t in tr)))
    o.append("total %d" % sum(len(v) for v in d.values()))
    return "\n".join(o)


def load_ledger(dirs):
    out = []
    for d in dirs:
        for fn in sorted(os.listdir(d)):
            if fn.endswith("_exclusions.jsonl"):
                with open(os.path.join(d, fn), encoding="utf-8") as f:
                    out += [json.loads(x) for x in f if x.strip()]
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--plan", help="docs/campaign_plans/b32.json")
    ap.add_argument("--dir", action="append", default=[], help="a bundle directory (has index.txt); repeatable")
    ap.add_argument("--sweep", action="append", default=[], help="a native_sweep CSV; repeatable, one per directory")
    ap.add_argument("--lines", help="the lines CSV (optional; only for the air-group line report)")
    ap.add_argument("--draw", action="store_true", help="print the blind-review draw and nothing else")
    ap.add_argument("--out", help="also write the report here")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args(argv)
    if args.selftest:
        selftest()
        return 0
    if not args.plan or not args.dir or (not args.draw and not args.sweep):
        ap.error("--plan, at least one --dir, and (unless --draw) at least one --sweep are required")
    with open(args.plan, encoding="utf-8") as f:
        plan = json.load(f)
    docs, excluded = {}, set()
    for d in args.dir:
        dd, ex = b30.load_bundles(d)
        dup = sorted(set(dd) & set(docs))
        if dup:
            raise SystemExit("bundle name(s) in more than one --dir (%s): the directories must not overlap" % ", ".join(dup[:3]))
        docs.update(dd)
        excluded |= ex
    if args.draw:
        ks, why = kept_strikes(docs, excluded, plan)
        # drawn after ALL the data is in: the same trial completeness as the selection, which needs no sweep rows
        why = why + [m for m in analyse(docs, excluded, [], plan)["incomplete"] if "no sweep row" not in m and m not in why]
        if why:
            ks = None
        if ks is None:
            report = "DRAW NOT MADE: the strike data are incomplete\n" + "\n".join("  " + w for w in why[:20])
        else:
            report = render_draw(draw_review(ks))
    else:
        rows = []
        for p in args.sweep:
            with open(p, encoding="utf-8", errors="replace") as f:
                rows += xs.parse_sweep_csv(f.read())
        lines = None
        if args.lines:
            with open(args.lines, encoding="utf-8", errors="replace") as f:
                lines = b30.parse_lines_csv(f.read())
        report = render(analyse(docs, excluded, rows, plan, load_ledger(args.dir), lines))
    print(report)
    if args.out:
        with open(args.out, "w", encoding="utf-8", newline="\n") as f:
            f.write(report + "\n")
    return 0


# ---------------------------------------------------------------------------------------------------
def selftest():
    import b32_select_selftest
    b32_select_selftest.run()


if __name__ == "__main__":
    sys.exit(main())
