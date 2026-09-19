"""Selftest for tools/b32_select.py: the registered B3.2 selection rules, asserted on synthetic campaigns.

Where the registration (docs/SOLENOID_CAMPAIGN.md, "B3.2 pre-registration") decides a case, the expected value
is derived by hand from it in the comment above the assertion. Where the tool applies a reading the text leaves
open (listed in the record under "Readings fixed for the selection tool"), the case says (reading) and the
assertion pins that reading, not the registration. The draw pins are regression values. The synthetic campaign is the real
registered plan (campaign_sequence.b32_plan(): 168 strikes, 56 no-fire, 42 air, seed 20260919) with a
behaviour function per candidate saying which captures that candidate clears and at what f1. Run through
`python tools/b32_select.py --selftest`.
"""
import contextlib
import io
import json
import os
import random
import sys
import tempfile
from fractions import Fraction as F

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import b32_crossspoke as xs      # noqa: E402
import b32_select as S           # noqa: E402
import campaign_sequence as seq  # noqa: E402

PLAN = seq.b32_plan()
SHA = seq.plan_sha256(PLAN)
TRIALS = S.plan_trials(PLAN)
# The validator refuses these seven chain profiles at the current capture length (record, "Candidate set").
NOT_EVAL = {"gate_start_ms=800", "gate_start_ms+window_ms=300+750", "gate_start_ms+window_ms=500+750",
            "gate_start_ms+window_ms=800+250", "gate_start_ms+window_ms=800+750",
            "gate_start_ms+prominence_db=800+9", "gate_start_ms+prominence_db=800+12"}
CONTROL_KIND = {"B3.0": "B3.0", "pass-C": "pass-C", "ambient": "ambient", "B3.2 no-fire": "nofire"}


def base_f1(ctx):
    return 450.0 + 3 * ctx["spoke"] + 0.2 * ctx["k"]      # one spoke's 6 clears spread 1.0 Hz: consistent


def only(kind, fn):
    return lambda ctx: fn(ctx) if ctx["kind"] == kind else None


def first(*fns):
    def f(ctx):
        for g in fns:
            v = g(ctx)
            if v is not None:
                return v
        return None
    return f


clear_all = only("strike", base_f1)


def cells(table, default=6):
    """Clear the first n strikes of every spoke at (station, level), n from the table."""
    return only("strike", lambda c: base_f1(c) if c["k"] < table.get((c["station"], c["level"]), default) else None)


def air_clear(station, level, k=0, f1=500.0):
    return only("air", lambda c: f1 if (c["station"], c["level"], c["k"]) == (station, level, k) else None)


def control_clear(kind, f1=500.0):
    return only(kind, lambda c: f1 if c["k"] == 0 else None)


def build(beh=None, snr=None, mod=None, skip=(), dup=(), skip_ambient=False):
    """-> (docs, rows). beh: {candidate label: ctx -> f1 or None}. snr: {(station, level): dB} for strikes.
    mod(name, doc, ctx) may edit a doc. skip: strike/air/no-fire trial numbers with no bundle. dup: trials that
    get a second bundle, named <name>_r."""
    beh = beh or {}
    snr = snr or {}
    docs, rows, counter = {}, [], {}

    def emit(name, doc, ctx):
        if mod:
            mod(name, doc, ctx)
        docs[name] = doc
        for lab in S.CANDIDATES:
            axis, value = lab.split("=", 1)
            if lab in NOT_EVAL:
                status, f = "OUT_OF_RANGE", None
            else:
                f = beh.get(lab, lambda c: None)(ctx)
                status = "suspect" if f is not None else "rejected"
            rows.append({"bundle": name, "axis": axis, "value": value, "candidate": lab, "status": status,
                         "reason": "x", "f1": f})

    for n, t in TRIALS.items():
        if n in skip:
            continue
        st, kind = t["station"], {"strike": "strike", "air_shot": "air", "no_fire": "nofire"}[t["kind"]]
        key = (kind, st, t["spoke"] if kind == "strike" else None, t["pulse_ms"] if kind != "nofire" else 0)
        k = counter.setdefault(key, 0)
        counter[key] += 1
        ctx = {"kind": kind, "station": st, "spoke": t["spoke"], "level": t["pulse_ms"], "k": k}
        doc = {"campaign_stage": "B3.2", "campaign_kind": t["kind"], "campaign_station": st, "spoke_id": t["spoke"],
               "pulse_ms": float(t["pulse_ms"]), "fired": t["kind"] != "no_fire", "capture_result": "OK",
               "campaign_plan_sha256": SHA, "campaign_trial": n, "station": "acoustic_%s" % st.lower(),
               "expect_snr_db": snr.get((st, t["pulse_ms"]), 15.0), "capture_overrun_events": 0,
               "expect_window_truncated_by": "window_ms"}
        prefix = {"strike": "s", "air": "a", "nofire": "n"}[kind]
        emit("%s%03d" % (prefix, n), doc, ctx)
        if n in dup:
            emit("%s%03d_r" % (prefix, n), dict(doc), ctx)
    ctl = [("B3.0", i, {"campaign_stage": "B3.0", "campaign_kind": "no_fire" if i < 20 else "air_shot"}, "B3.0_%02d" % i)
           for i in range(40)]
    ctl += [("pass-C", i, {}, "C_sp0_a%d_seq%02d" % (i % 3, i)) for i in range(18)]
    if not skip_ambient:
        ctl += [("ambient", 0, {}, "nano_ambient_ambiguous")]
    for kind, i, extra, name in ctl:
        emit(name, dict({"expect_snr_db": 5.0}, **extra), {"kind": kind, "station": None, "spoke": 0, "level": 0, "k": i})
    return docs, rows


def go(*a, excluded=(), ledger=(), lines=None, **kw):
    docs, rows = build(*a, **kw)
    return S.analyse(docs, set(excluded), rows, PLAN, list(ledger), lines)


def cell(R, lab, st, lv):
    return R["cand"][lab]["cells"][st][lv]


def run():
    # ---- the registered list, the classes, the plan's shape (literal spot checks from the registration text)
    assert len(S.CANDIDATES) == 34 and len(set(S.CANDIDATES)) == 34
    assert S.CANDIDATES[0] == "baseline=0" and S.CANDIDATES[1] == "gate_start_ms=100"
    assert S.CANDIDATES[5] == "gate_start_ms=800" and S.CANDIDATES[6] == "window_ms=250" and S.CANDIDATES[9] == "prominence_db=12"
    assert S.CANDIDATES[10] == "gate_start_ms+window_ms=100+250" and S.CANDIDATES[19] == "gate_start_ms+window_ms=800+750"
    assert S.CANDIDATES[20] == "gate_start_ms+prominence_db=100+9" and S.CANDIDATES[29] == "gate_start_ms+prominence_db=800+12"
    assert S.CANDIDATES[30] == "window_ms+prominence_db=250+9" and S.CANDIDATES[33] == "window_ms+prominence_db=750+12"
    assert [S.fields_changed(x) for x in (S.CANDIDATES[0], S.CANDIDATES[1], S.CANDIDATES[10])] == [0, 1, 2]
    assert {sp: S.spoke_class(sp) for sp in (17, 5, 3, 15, 10, 22, 12, 0)} == {
        17: "lead", 5: "lead", 3: "trail", 15: "trail", 10: "trail", 22: "trail", 12: "lead", 0: "lead"}
    assert len(TRIALS) == 266 and sum(1 for t in TRIALS.values() if t["kind"] == "strike") == 168
    assert PLAN["levels"] == {"LEFT": [40, 60, 72, 85], "RIGHT": [60, 72, 85]}

    # ---- 1. one candidate clears every strike consistently. By hand: every spoke 6/6 so W = 1 everywhere; every
    # cell satisfies its constraints so S = all tested levels; score = min(1, 1) = 1. Shared levels within 10 pp
    # of the best at both stations: 60, 72, 85. Plateau interior counts: 60 -> LEFT yes (40 and 72 in S), RIGHT no
    # (edge) = 1; 72 -> LEFT yes, RIGHT yes = 2; 85 -> edge at both = 0. So the shared level is 72.
    R = go({"baseline=0": clear_all})
    assert R["complete"], R["incomplete"][:3]
    c = R["cand"]["baseline=0"]
    assert c["admissible"] and c["eligible"] and c["S"] == {"LEFT": [40, 60, 72, 85], "RIGHT": [60, 72, 85]}
    assert c["score"] == F(1) and R["chosen"] == "baseline=0" and R["tied_on_score"] == ["baseline=0"]
    assert c["levels_chosen"] == {"LEFT": 72, "RIGHT": 72} and c["shared"] is True
    assert all(v == F(1) for v in cell(R, "baseline=0", "RIGHT", 72)["per_spoke"].values())
    assert R["gate"]["LEFT"]["passed"] and R["gate"]["RIGHT"]["passed"] and R["halt"] is None
    # a candidate that never clears has 0 false clears and 0 inconsistent clears: eligible, score 0. The seven
    # the validator refuses are not evaluable and never eligible: 34 - 7 = 27 evaluable candidates.
    assert R["n_eligible"] == 27 and R["cand"]["window_ms=250"]["score"] == F(0)
    assert all(R["cand"][x]["status"] == "not_evaluable" and not R["cand"][x]["eligible"] for x in NOT_EVAL)
    assert "CHOSEN candidate: baseline=0" in S.render(R)

    # ---- 2. tie-breaks after the score: fewest fields changed (0, 1, 2), then the earlier registered candidate.
    pair, one, base = "gate_start_ms+window_ms=100+250", "gate_start_ms=100", "baseline=0"
    R = go({base: clear_all, one: clear_all, pair: clear_all})
    assert R["chosen"] == base and R["tied_on_score"] == [base, one, pair]
    R = go({one: clear_all, pair: clear_all})
    assert R["chosen"] == one and R["tied_on_score"] == [one, pair]
    R = go({"window_ms=250": clear_all, one: clear_all})            # both change one field: gate is registered first
    assert R["chosen"] == one and R["tied_on_score"] == [one, "window_ms=250"]
    R = go({"window_ms=250": clear_all, "prominence_db=9": clear_all})
    assert R["chosen"] == "window_ms=250"
    # The order in the registration is: plateau interior, fewest fields, higher SNR, shorter pulse. Registered order
    # alone cannot show "fewest fields" (fewer fields always come earlier), so here fields must beat pulse length:
    # the one-field candidate clears only at 85 ms (shared 85/85, interior 0 at both edges, pulse sum 170); the
    # two-field candidate clears only LEFT 40 and RIGHT 60 (per-station, interior 0, pulse sum 100). Score, interior
    # and SNR tie; the shorter pulse would pick the pair, the fewer fields picks the one-field candidate.
    only85 = cells({("LEFT", 85): 6, ("RIGHT", 85): 6}, default=0)
    edges = cells({("LEFT", 40): 6, ("RIGHT", 60): 6}, default=0)
    R = go({one: only85, pair: edges})
    assert R["cand"][one]["levels_chosen"] == {"LEFT": 85, "RIGHT": 85} and R["cand"][pair]["levels_chosen"] == {"LEFT": 40, "RIGHT": 60}
    assert R["cand"][one]["score"] == R["cand"][pair]["score"] == F(1) and R["chosen"] == one

    # ---- 3. the consistency band: median over the spoke's clears in the condition INCLUDING the clear under
    # test, +-2 Hz inclusive, and a spoke with under 3 clears is unjudged. RIGHT spoke 17 at 72 ms only.
    def sp17(f1s):
        def f(ctx):
            if ctx["kind"] != "strike":
                return None
            if (ctx["station"], ctx["spoke"], ctx["level"]) == ("RIGHT", 17, 72):
                return f1s[ctx["k"]] if ctx["k"] < len(f1s) else None
            return base_f1(ctx)
        return f
    # 500.0, 502.0, 504.0: median 502.0 including itself, all within 2.0 -> 3 consistent, 0 inconsistent, rate 3/6.
    # (Excluding the clear under test the median of the other two is 503.0 and 500.0 would be 3.0 out: not this.)
    R = go({base: sp17([500.0, 502.0, 504.0])})
    x = cell(R, base, "RIGHT", 72)
    assert x["inconsistent"] == 0 and x["per_spoke"][17] == F(1, 2) and x["ok"] and x["W"] == F(1, 2)
    # 504.01 is 2.01 from the median 502.0: one inconsistent clear, and the cell fails its constraints.
    R = go({base: sp17([500.0, 502.0, 504.01])})
    x = cell(R, base, "RIGHT", 72)
    assert x["inconsistent"] == 1 and not x["ok"] and x["per_spoke"][17] == F(2, 6)
    assert R["cand"][base]["S"]["RIGHT"] == [60, 85]
    # two clears are unjudged: they count as neither consistent nor inconsistent, and add nothing to the rate.
    R = go({base: sp17([500.0, 530.0])})
    x = cell(R, base, "RIGHT", 72)
    assert x["unjudged"] == 2 and x["inconsistent"] == 0 and x["per_spoke"][17] == F(0) and x["ok"] and x["W"] == F(0)
    # 500, 500, 510: median 500, the third is 10 out: 2 consistent, 1 inconsistent.
    R = go({base: sp17([500.0, 500.0, 510.0])})
    x = cell(R, base, "RIGHT", 72)
    assert x["inconsistent"] == 1 and x["per_spoke"][17] == F(2, 6)

    # ---- 4. a false clear among a cell's own 6 air shots fails that cell only; the pooled control sets are separate.
    # Air at RIGHT 72: S(RIGHT) = [60, 85]; the candidate stays admissible. Shared levels are 60 and 85 (LEFT S is
    # everything). Interior: 60 -> LEFT yes, RIGHT no (an edge) = 1; 85 -> 0. So level 60.
    R = go({base: first(clear_all, air_clear("RIGHT", 72))})
    c = R["cand"][base]
    assert c["admissible"] and cell(R, base, "RIGHT", 72)["air_false"] == 1 and not cell(R, base, "RIGHT", 72)["ok"]
    assert cell(R, base, "RIGHT", 60)["air_false"] == 0 and cell(R, base, "LEFT", 72)["air_false"] == 0
    assert c["S"] == {"LEFT": [40, 60, 72, 85], "RIGHT": [60, 85]} and c["levels_chosen"] == {"LEFT": 60, "RIGHT": 60}

    # ---- 5. attribution: fired, station derived from the declared spoke, capture_result OK, pulse to 0.01 ms.
    def breaks(edit):
        def m(name, doc, ctx):
            if ctx["kind"] == "strike" and (ctx["station"], ctx["spoke"], ctx["level"], ctx["k"]) == ("RIGHT", 17, 72, 0):
                edit(doc)
        return m
    R = go({base: clear_all}, mod=breaks(lambda d: d.update(pulse_ms=72.01)))
    assert cell(R, base, "RIGHT", 72)["attribution"] and cell(R, base, "RIGHT", 72)["ok"]      # 0.01 is within
    for what, edit in (("pulse +0.02", lambda d: d.update(pulse_ms=72.02)), ("not fired", lambda d: d.update(fired=False)),
                       ("other station", lambda d: d.update(campaign_station="LEFT")),
                       ("other spoke", lambda d: d.update(spoke_id=5)),
                       ("board station", lambda d: d.update(station="acoustic_left")),
                       ("capture not OK", lambda d: d.update(capture_result="OVERRUN"))):
        R = go({base: clear_all}, mod=breaks(edit))
        x = cell(R, base, "RIGHT", 72)
        assert not x["attribution"] and not x["ok"], what
        assert cell(R, base, "RIGHT", 60)["attribution"], what + ": another cell must be untouched"
        assert R["cand"][base]["S"]["RIGHT"] == [60, 85], what

    # ---- 6. selection shift: both clear and f1 differ by MORE than 2 Hz on any strike capture. LEFT spoke 0's
    # first strike is at 450.0 under the baseline.
    def shifted(delta, base_clears=True):
        def f(ctx):
            v = base_f1(ctx)
            return v + delta if (ctx["station"], ctx["spoke"], ctx["k"]) == ("LEFT", 0, 0) else v
        return only("strike", f)
    beh_base = clear_all
    R = go({base: beh_base, one: shifted(2.5)})
    assert not R["cand"][one]["admissible"] and len(R["cand"][one]["shifts"]) == 4      # one per LEFT level
    assert "selection shift" in R["cand"][one]["reasons"][0]
    R = go({base: beh_base, one: shifted(2.0)})
    assert R["cand"][one]["admissible"] and R["cand"][one]["shifts"] == []                # exactly 2.0 is not more
    R = go({base: beh_base, one: shifted(2.01)})
    assert not R["cand"][one]["admissible"]
    # where the baseline does not clear the capture there is nothing to shift from
    rej_first = only("strike", lambda c: None if (c["station"], c["spoke"], c["k"]) == ("LEFT", 0, 0) else base_f1(c))
    R = go({base: rej_first, one: shifted(9.0)})
    assert R["cand"][one]["shifts"] == [] and R["cand"][one]["admissible"]

    # ---- 7. false clears on any pooled control set make a candidate inadmissible: B3.0's 40, B3.2's no-fire,
    # Phase A pass-C, the ambient bundle. Air shots at B3.2 are not in the pooled sets (case 4).
    for setname, kind in CONTROL_KIND.items():
        R = go({base: first(clear_all, control_clear(kind))})
        c = R["cand"][base]
        want = 2 if setname == "B3.2 no-fire" else 1        # no-fire controls are numbered per station: k == 0 twice
        assert not c["admissible"] and c["false_by_set"][setname] == want and setname in c["reasons"][0], setname
        assert sum(v for k, v in c["false_by_set"].items() if k != setname) == 0
    assert R["control_sizes"] == {"B3.2 no-fire": 56, "B3.0": 40, "pass-C": 18, "ambient": 1}

    # ---- 8. score = the SMALLER over the stations of the largest worst-spoke rate over S.
    # A: LEFT 6/6, RIGHT 5/6 -> 5/6. B: clears only at 85 ms, 6/6 at both stations -> 1. C: LEFT only -> RIGHT best 0 -> 0.
    A = first(only("strike", lambda c: base_f1(c) if c["station"] == "LEFT" or c["k"] < 5 else None))
    B = cells({(st, l): (6 if l == 85 else 0) for st in S.STATIONS for l in (40, 60, 72, 85)}, default=0)
    Cc = only("strike", lambda c: base_f1(c) if c["station"] == "LEFT" else None)
    R = go({base: A, "window_ms=250": B, one: Cc})
    assert R["cand"][base]["score"] == F(5, 6) and R["cand"][base]["best"] == {"LEFT": F(1), "RIGHT": F(5, 6)}
    assert R["cand"]["window_ms=250"]["score"] == F(1) and R["cand"][one]["score"] == F(0)
    assert R["chosen"] == "window_ms=250" and R["tied_on_score"] == ["window_ms=250"]      # later in order, higher score
    assert R["cand"]["window_ms=250"]["levels_chosen"] == {"LEFT": 85, "RIGHT": 85}         # only 85 is within 10 pp

    # ---- 9. eligibility and G-analysis: a candidate needs a non-empty S at BOTH stations. Every evaluable
    # candidate clears RIGHT spoke 17 inconsistently (offsets 0, 3, .. 15 Hz: median 7.5 Hz, four are outside +-2),
    # so S(RIGHT) is empty for all: nothing is eligible.
    def wide(ctx):
        return base_f1(ctx) + 3 * ctx["k"] if (ctx["station"], ctx["spoke"]) == ("RIGHT", 17) else base_f1(ctx)
    R = go({lab: only("strike", wide) for lab in S.CANDIDATES})
    assert R["n_eligible"] == 0 and R["halt"] == "G-analysis" and R["chosen"] is None
    assert all("no admissible cell at RIGHT" in R["cand"][x]["reasons"] for x in S.CANDIDATES if x not in NOT_EVAL)
    assert "HALT G-ANALYSIS" in S.render(R)
    # a candidate with S(RIGHT) empty loses to one with both, however good its LEFT. The baseline clears everything but
    # has an air false clear at each RIGHT width, so no RIGHT cell satisfies its constraints; the rival clears the same
    # f1 at 3 of 6 on RIGHT (no selection shift) and 6 of 6 on LEFT: score min(1, 1/2) = 1/2.
    dead_right = first(clear_all, air_clear("RIGHT", 60), air_clear("RIGHT", 72), air_clear("RIGHT", 85))
    R = go({base: dead_right, "window_ms=250": cells({("RIGHT", 60): 3, ("RIGHT", 72): 3, ("RIGHT", 85): 3})})
    assert R["cand"][base]["S"]["RIGHT"] == [] and not R["cand"][base]["eligible"]
    assert "no admissible cell at RIGHT" in R["cand"][base]["reasons"]
    assert R["cand"]["window_ms=250"]["score"] == F(1, 2) and R["chosen"] == "window_ms=250"

    # ---- 10. rule 4. Worst-spoke rate by cell via the number of clearing strikes.
    # (a) LEFT 40/60/72/85 = 6/6/6/3, RIGHT 60/72/85 = 3/6/6: best is 1 at both; 72 is the only level within 10 pp of
    # the best at both -> shared 72.
    R = go({base: cells({("LEFT", 40): 6, ("LEFT", 60): 6, ("LEFT", 72): 6, ("LEFT", 85): 3,
                         ("RIGHT", 60): 3, ("RIGHT", 72): 6, ("RIGHT", 85): 6})})
    assert R["cand"][base]["levels_chosen"] == {"LEFT": 72, "RIGHT": 72} and R["cand"][base]["shared"]
    # (b) LEFT 4/5/6/3 (best at 72), RIGHT 6/5/3 (best at 60): 5/6 is 16.7 pp below 1, more than 10 pp, so no level
    # is within 10 pp at both -> each station takes its own best: LEFT 72, RIGHT 60.
    R = go({base: cells({("LEFT", 40): 4, ("LEFT", 60): 5, ("LEFT", 72): 6, ("LEFT", 85): 3,
                         ("RIGHT", 60): 6, ("RIGHT", 72): 5, ("RIGHT", 85): 3})})
    c = R["cand"][base]
    assert c["levels_chosen"] == {"LEFT": 72, "RIGHT": 60} and c["shared"] is False and c["score"] == F(1)

    # ---- 11. plateau interior, then higher median SNR, then shorter pulse. Air false clears at LEFT 40 and 85 and
    # RIGHT 85 leave S = [60, 72] at both, and neither 60 nor 72 has both neighbours in S at either station, so
    # interior ties at 0 and the SNR decides. A shared level's SNR is the LOWER of the two stations' medians.
    airs = first(clear_all, air_clear("LEFT", 40), air_clear("LEFT", 85), air_clear("RIGHT", 85))
    R = go({base: airs})
    assert R["cand"][base]["S"] == {"LEFT": [60, 72], "RIGHT": [60, 72]}
    assert R["cand"][base]["levels_chosen"] == {"LEFT": 60, "RIGHT": 60}                    # equal SNR: shorter pulse
    R = go({base: airs}, snr={("LEFT", 72): 20.0, ("RIGHT", 72): 20.0})
    assert R["cand"][base]["levels_chosen"] == {"LEFT": 72, "RIGHT": 72}                    # 72 is louder at both
    R = go({base: airs}, snr={("LEFT", 72): 20.0, ("RIGHT", 72): 12.0})
    assert R["cand"][base]["levels_chosen"] == {"LEFT": 60, "RIGHT": 60}                    # min(20, 12) = 12 < 15

    # ---- 12. exploration gate: an eligible candidate with a cell in S at worst spoke >= 5/6 at each station.
    R = go({base: only("strike", lambda c: base_f1(c) if c["station"] == "LEFT" or c["k"] < 4 else None)})
    assert R["gate"]["RIGHT"] == {"best_W": F(2, 3), "passed": False} and R["gate"]["LEFT"]["passed"]
    R = go({base: only("strike", lambda c: base_f1(c) if c["station"] == "LEFT" or c["k"] < 4 else None),
            "window_ms=250": only("strike", lambda c: base_f1(c) if c["station"] == "LEFT" or (c["level"] == 85 and c["k"] < 5) else None)})
    assert R["gate"]["RIGHT"] == {"best_W": F(5, 6), "passed": True}                       # exactly 5/6 passes

    # ---- 13. class check: at each station, a level in S where one class's worst spoke is more than 10 pp below the
    # other's is a class gap; if every level in S has one that station is G-CLASS. RIGHT: leading 17 and 5, trailing
    # 3 and 15. Trailing at 4/6 everywhere: lead 1, trail 2/3, gap 1/3 > 1/10 at every level.
    seq_class = lambda c: S.spoke_class(c["spoke"])
    R = go({base: only("strike", lambda c: base_f1(c) if c["station"] == "LEFT" or seq_class(c) == "lead" or c["k"] < 4 else None)})
    assert R["class_check"]["RIGHT"]["g_class"] and not R["class_check"]["LEFT"]["g_class"] and R["halt"] == "G-class"
    assert "HALT G-CLASS" in S.render(R)
    # only at 72 ms: gaps at one level, not all -> no halt
    R = go({base: only("strike", lambda c: base_f1(c) if not (c["station"] == "RIGHT" and c["level"] == 72
                                                              and seq_class(c) == "trail" and c["k"] >= 4) else None)})
    assert R["class_check"]["RIGHT"]["levels"] == [(60, False), (72, True), (85, False)] and R["halt"] is None
    # a gap of exactly one step (16.7 pp) is more than 10 pp
    assert S.CLASS_GAP < F(1, 6)

    # ---- 14. refinement (reported, never applied). (a) RIGHT 60/72/85 = 4/5/6, LEFT all 6: the best level is 85, a
    # tested edge that is not the RIGHT bracket edge (95): trigger, whole-ms midpoint of 85 and 95 = 90. LEFT chooses
    # 85, which IS its bracket edge: none. No adjacent RIGHT levels differ by more than 25 pp (1/6 each).
    R = go({base: cells({("RIGHT", 60): 4, ("RIGHT", 72): 5, ("RIGHT", 85): 6})})
    assert R["cand"][base]["levels_chosen"] == {"LEFT": 85, "RIGHT": 85}
    assert R["refinement"]["RIGHT"]["triggers"] == [("edge", 85, 95, [90])] and R["refinement"]["LEFT"]["triggers"] == []
    # (b) RIGHT 6/3/6: chosen 60 (interior count 1 beats 85's 0). 60 is the bracket edge: no edge trigger. Gaps 60-72
    # and 72-85 are 50 pp > 25 pp: midpoints 66, and 78.5 (two whole-ms choices, both listed).
    R = go({base: cells({("RIGHT", 60): 6, ("RIGHT", 72): 3, ("RIGHT", 85): 6})})
    assert R["cand"][base]["levels_chosen"] == {"LEFT": 60, "RIGHT": 60}
    assert R["refinement"]["RIGHT"]["triggers"] == [("gap", 60, 72, [66]), ("gap", 72, 85, [78, 79])]
    assert R["refinement"]["LEFT"]["triggers"] == []
    # (c) rates move in steps of 1/6: one step (16.7 pp) is under 25 pp, two steps (33.3 pp) is over
    assert F(1, 6) <= S.REFINE_GAP < F(2, 6)

    # ---- 15. completeness: selection is computed only when every planned trial has exactly one kept bundle.
    R = go({base: clear_all}, skip={40})
    assert not R["complete"] and R["chosen"] is None and "trial 40 " in R["incomplete"][0] and "0 kept" in R["incomplete"][0]
    assert "NOT COMPUTED" in S.render(R)
    strike_trial = next(n for n, t in TRIALS.items() if t["kind"] == "strike")
    nm = "s%03d" % strike_trial
    R = go({base: clear_all}, dup={strike_trial})                                            # two kept bundles
    assert not R["complete"] and "2 kept" in R["incomplete"][0]
    R = go({base: clear_all}, dup={strike_trial}, excluded={nm})                             # replaced in place
    assert R["complete"] and R["chosen"] == base
    R = go({base: clear_all}, excluded={nm})                                                 # excluded, not replaced
    assert not R["complete"]
    R = go({base: clear_all}, skip_ambient=True)
    assert not R["complete"] and any("ambient has 0" in m for m in R["incomplete"])
    R = go({base: clear_all}, mod=lambda n, d, c: d.update(campaign_plan_sha256="deadbeef" * 8) if n == nm else None)
    assert not R["complete"] and any("another plan" in m for m in R["incomplete"])
    R = go({base: clear_all}, mod=lambda n, d, c: d.update(campaign_kind="air_shot") if n == nm else None)
    assert not R["complete"] and any("planned as strike" in m for m in R["incomplete"])
    docs, rows = build({base: clear_all})
    rows = [r for r in rows if not (r["bundle"] == nm and r["candidate"] == one)]
    R = S.analyse(docs, set(), rows, PLAN)
    assert not R["complete"] and any(one in m and "no sweep row" in m for m in R["incomplete"])
    # a candidate the validator refuses is not evaluable whatever its rows say, and is never chosen
    R = go({"gate_start_ms=800": clear_all})
    assert R["cand"]["gate_start_ms=800"]["status"] == "not_evaluable" and R["chosen"] != "gate_start_ms=800"
    assert "NO SELECTION" in S.render(R)

    # ---- 16. reported, not gating
    R = go({base: only("strike", lambda c: 380.0 + 0.2 * c["k"] if c["spoke"] == 3 else base_f1(c)),
            "window_ms=250": first(control_clear("nofire", 375.0))},
           mod=lambda n, d, c: d.update(capture_overrun_events=1) if n == "s001" else None,
           ledger=[{"plan_sha256": SHA, "kind": "strike", "pulse_ms": 72, "spoke": 17, "code": "FETCH_FAILED"},
                   {"plan_sha256": SHA, "kind": "strike", "pulse_ms": 72, "spoke": 5, "code": "FETCH_FAILED"},
                   {"plan_sha256": "other", "kind": "strike", "pulse_ms": 72, "spoke": 17, "code": "FETCH_FAILED"},
                   {"plan_sha256": SHA, "kind": "air_shot", "pulse_ms": 72, "spoke": 17, "code": "FETCH_FAILED"}])
    assert R["noise_band"]["baseline=0"]["strikes"] == (168, 18)          # RIGHT spoke 3: 6 strikes x 3 levels in 355-395
    assert R["noise_band"]["baseline=0"]["B3.2 no-fire"] == (0, 0)
    assert R["reported"][("RIGHT", 72)]["excluded"] == {"FETCH_FAILED": 2}
    assert R["reported"][("LEFT", 40)]["kept"] == 24 and R["reported"][("LEFT", 40)]["truncated_by"] == {"window_ms": 24}
    assert sum(d["overruns"] for d in R["reported"].values()) == 1
    lines = ({"min_snr": 12.0}, {"a%03d" % n: {"peaks": [(500.0, 14.0), (480.0, 5.0)], "note": None, "n_strong": 2} for n, t in TRIALS.items()
                                 if t["kind"] == "air_shot" and t["station"] == "RIGHT" and t["pulse_ms"] == 72})
    R = go({base: clear_all}, lines=lines)
    assert R["air_lines"][("RIGHT", 72)] == {"n": 6, "with_line": 6, "lines": [(500.0, 14.0)] * 6, "unreadable": 0}
    assert R["air_lines"][("LEFT", 40)]["with_line"] == 0

    # ---- 17. the blind-review draw: 24 = LEFT 3 per level x 4 + RIGHT 4 per level x 3, from kept strikes only, by a
    # fresh random.Random(20260919) per stratum over the sorted trial numbers.
    docs, _ = build()
    ks, why = S.kept_strikes(docs, set(), PLAN)
    assert why == [] and len(ks) == 168
    d = S.draw_review(ks)
    assert sum(len(v) for v in d.values()) == 24 and len(d) == 7
    assert all(len(v) == (3 if st == "LEFT" else 4) for (st, l), v in d.items())
    pools = {}
    for n, st, l in ks:
        pools.setdefault((st, l), []).append(n)
    assert all(set(v) <= set(pools[k]) and v == sorted(v) for k, v in d.items())
    shuffled = ks[:]
    random.Random(1).shuffle(shuffled)
    assert S.draw_review(shuffled) == d and S.draw_review(ks) == d                          # order- and repeat-proof
    # (reading) one generator across the strata, LEFT 40/60/72/85 then RIGHT 60/72/85, `sample` over the sorted pool.
    # Pinned for pools of trials 1..24 (computed with plain random calls; a change here means the draw changed,
    # which must never happen after the data exist). CPython's random.sample under seed 20260919.
    pools24 = [(n, st, l) for st, lv in (("LEFT", (40, 60, 72, 85)), ("RIGHT", (60, 72, 85))) for l in lv for n in range(1, 25)]
    pin = S.draw_review(pools24)
    assert pin == {("LEFT", 40): [4, 12, 21], ("LEFT", 60): [5, 21, 24], ("LEFT", 72): [11, 13, 24],
                   ("LEFT", 85): [2, 9, 18], ("RIGHT", 60): [1, 3, 14, 18], ("RIGHT", 72): [1, 11, 19, 22],
                   ("RIGHT", 85): [1, 2, 13, 22]}, pin
    # why one generator: with a fresh generator per stratum every equal-sized pool would draw the same positions
    # (and so the same blocks, i.e. the same spokes); here the four LEFT strata differ
    assert len({tuple(v) for (st, l), v in pin.items() if st == "LEFT"}) == 4
    excl = "s%03d" % ks[0][0]
    ks2, why2 = S.kept_strikes(docs, {excl}, PLAN)
    assert ks2 is None and "0 kept" in why2[0]
    try:
        S.draw_review([(1, "LEFT", 40)])
        raise AssertionError("a stratum smaller than its draw must refuse")
    except SystemExit:
        pass

    # ---- 17b. boundaries in exact millihertz. 510.003 -> 512.003 is exactly 2.000 Hz apart; as floats it is
    # 2.000000000000057, which "MORE than 2 Hz" would wrongly flag and "within 2 Hz" would wrongly fail.
    def one_strike(first_f1):
        return only("strike", lambda c: first_f1 if (c["station"], c["spoke"], c["k"]) == ("LEFT", 0, 0) else base_f1(c))
    R = go({base: one_strike(510.003), one: one_strike(512.003)})
    assert R["cand"][one]["shifts"] == [] and R["cand"][one]["admissible"]
    R = go({base: one_strike(510.003), one: one_strike(512.004)})
    assert len(R["cand"][one]["shifts"]) == 4 and not R["cand"][one]["admissible"]
    R = go({base: sp17([510.003, 512.003, 514.003])})             # median 512.003, both ends exactly 2.000 out
    assert cell(R, base, "RIGHT", 72)["inconsistent"] == 0

    # ---- 17c. attribution needs the board's own station record
    R = go({base: clear_all}, mod=breaks(lambda d: d.pop("station")))
    assert not cell(R, base, "RIGHT", 72)["attribution"]

    # ---- 17d. a sweep row that appears twice: identical is harmless, different is a finding. INIT_REFUSED is a
    # candidate that cannot run, like OUT_OF_RANGE.
    docs, rows = build({base: clear_all})
    assert S.analyse(docs, set(), rows + [dict(rows[0])], PLAN)["complete"]
    bad = dict(rows[0])
    bad["status"] = "rejected" if rows[0]["status"] == "suspect" else "suspect"
    R = S.analyse(docs, set(), rows + [bad], PLAN)
    assert not R["complete"] and any("appear twice" in m for m in R["incomplete"])
    docs, rows = build({"window_ms=250": clear_all})
    for r in rows:
        if r["candidate"] == "window_ms=250":
            r["status"], r["f1"] = "INIT_REFUSED", None
    assert S.analyse(docs, set(), rows, PLAN)["cand"]["window_ms=250"]["status"] == "not_evaluable"

    # ---- 17e. the exploration gate is per station over ANY eligible candidate. A: LEFT 6/6, RIGHT 4/6 (score 2/3).
    # B: LEFT 4/6, RIGHT 5/6 (score 2/3). No single candidate reaches 5/6 at both stations, but LEFT is reached by A
    # and RIGHT by B, so both stations pass; the chosen candidate is A (equal score and interior, fewer fields).
    A2 = only("strike", lambda c: base_f1(c) if c["k"] < (6 if c["station"] == "LEFT" else 4) else None)
    B2 = only("strike", lambda c: base_f1(c) if c["k"] < (4 if c["station"] == "LEFT" else 5) else None)
    R = go({base: A2, "window_ms=250": B2})
    assert R["chosen"] == base and R["cand"][base]["score"] == R["cand"]["window_ms=250"]["score"] == F(2, 3)
    assert R["gate"] == {"LEFT": {"best_W": F(1), "passed": True}, "RIGHT": {"best_W": F(5, 6), "passed": True}}

    # ---- 17f. tie-break order at candidate level. Plateau interior beats fewer fields: the two-field candidate
    # clears only at 72 ms (interior at both stations), the one-field candidate only at 85 ms (an edge at both).
    only72 = cells({("LEFT", 72): 6, ("RIGHT", 72): 6}, default=0)
    R = go({one: only85, pair: only72})
    assert R["cand"][pair]["levels_chosen"] == {"LEFT": 72, "RIGHT": 72} and R["chosen"] == pair
    # Higher median SNR beats both a shorter pulse and the earlier registered candidate: gate100 clears LEFT 40 /
    # RIGHT 60 (edges, pulse sum 100, earlier), window250 clears 85 ms (edges, pulse sum 170) and 85 ms is louder.
    R = go({one: edges, "window_ms=250": only85}, snr={("LEFT", 85): 20.0, ("RIGHT", 85): 20.0})
    assert R["cand"][one]["levels_chosen"] == {"LEFT": 40, "RIGHT": 60} and R["chosen"] == "window_ms=250"

    # ---- 17g. class gap the other way round (trailing better than leading) is a gap too
    R = go({base: only("strike", lambda c: base_f1(c) if c["station"] == "LEFT" or seq_class(c) == "trail" or c["k"] < 4 else None)})
    assert R["class_check"]["RIGHT"]["g_class"] and R["halt"] == "G-class"

    # ---- 17h. rule 8: an SNR the board did not record (None) is not in the median; a class needs 6 captures
    hide17 = lambda n, d, c: d.update(expect_snr_db=None) if (c["kind"] == "strike" and (c["station"], c["spoke"], c["level"]) == ("RIGHT", 17, 72)) else None
    a = go({base: clear_all}, mod=hide17)["snr"][("RIGHT", 72)]
    assert a["all"] == (15.0, 18) and a["lead"] == (15.0, 6) and a["trail"] == (15.0, 12)
    hide5 = lambda n, d, c: (hide17(n, d, c), d.update(expect_snr_db=None) if (c["kind"] == "strike" and (c["station"], c["spoke"], c["level"], c["k"]) == ("RIGHT", 5, 72, 0)) else None)
    a = go({base: clear_all}, mod=hide5)["snr"][("RIGHT", 72)]
    assert a["all"] == (15.0, 17) and "lead" not in a and a["trail"] == (15.0, 12)      # 5 lead captures: no class figure

    # ---- 17i. reported, not gating: no-fire clears in the noise band; unjudged clears printed; overruns among the
    # excluded bundles; air lines missing or unreadable
    R = go({base: first(clear_all, control_clear("nofire", 375.0))})
    assert R["noise_band"]["baseline=0"]["B3.2 no-fire"] == (2, 2)            # one per station (numbered per station)
    R = go({base: sp17([500.0, 530.0])})
    rep = S.render(R)
    assert "Unjudged clears" in rep and "72 ms: 2" in rep
    t0 = TRIALS[strike_trial]
    R = go({base: clear_all}, dup={strike_trial}, excluded={nm},
           mod=lambda n, d, c: d.update(capture_overrun_events=1 if n == nm else 0))
    assert R["reported"][(t0["station"], t0["pulse_ms"])]["overruns_excluded"] == 1
    assert R["reported"][(t0["station"], t0["pulse_ms"])]["overruns"] == 0
    R = go({base: clear_all}, lines=({"min_snr": 12.0}, {}))
    assert R["air_lines"][("RIGHT", 72)]["unreadable"] == 6 and R["air_lines"][("RIGHT", 72)]["with_line"] == 0
    assert "no readable spectrum" in S.render(R)
    R = go({base: clear_all}, lines=({}, {}))
    assert "error" in R["air_lines"] and "not reported" in S.render(R)
    assert "not reported (no --lines CSV given)" in S.render(go({base: clear_all}))

    # ---- 18. end to end through main(): two directories, two sweep CSVs, a ledger, the CLI's own parsing.
    docs, rows = build({base: clear_all})
    tmp = tempfile.mkdtemp(prefix="b32_select_")
    dirs = [os.path.join(tmp, "campaign"), os.path.join(tmp, "fixtures")]
    for d_ in dirs:
        os.makedirs(d_)
    names = {dirs[0]: [n for n in docs if n != "nano_ambient_ambiguous"], dirs[1]: ["nano_ambient_ambiguous"]}
    sweeps = []
    for d_ in dirs:
        with open(os.path.join(d_, "index.txt"), "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(names[d_]) + "\n")
        for n in names[d_]:
            with open(os.path.join(d_, n + ".json"), "w", encoding="utf-8") as f:
                json.dump(docs[n], f)
        p = os.path.join(tmp, os.path.basename(d_) + "_sweep.csv")
        with open(p, "w", encoding="utf-8", newline="\n") as f:
            f.write("sweep dir: x   bundles: %d\n" % len(names[d_]))
            f.write("bundle,axis,value,status,reason,n_strong_peaks,n_peaks_in_band,f1_hz,f2_hz,snr_db,clears_min_snr\n")
            for r in rows:
                if r["bundle"] in names[d_]:
                    f.write("%s,%s,%s,%s,%s,0,0,%s,,,0\n" % (r["bundle"], r["axis"], r["value"], r["status"], r["reason"],
                                                             "%.3f" % r["f1"] if r["f1"] is not None else ""))
        sweeps.append(p)
    plan_path = os.path.join(tmp, "plan.json")
    with open(plan_path, "w", encoding="utf-8") as f:
        json.dump(PLAN, f)
    argv = ["--plan", plan_path, "--dir", dirs[0], "--dir", dirs[1], "--sweep", sweeps[0], "--sweep", sweeps[1]]

    def cli(a):
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            rc = S.main(a)
        assert rc == 0
        return buf.getvalue()
    out = cli(argv)
    assert "CHOSEN candidate: baseline=0" in out and "NOT COMPUTED" not in out, out[:400]
    assert "  LEFT  level 72 ms (shared)" in out and "not evaluable" in out
    out = cli(["--plan", plan_path, "--dir", dirs[0], "--dir", dirs[1], "--draw"])
    assert "total 24" in out and "no machine verdict was read" in out, out
    out = cli(["--plan", plan_path, "--dir", dirs[0], "--draw"])                            # the ambient bundle is in dirs[1]
    assert "DRAW NOT MADE" in out and "ambient has 0" in out, out
    overlap = os.path.join(tmp, "overlap")
    os.makedirs(overlap)
    with open(os.path.join(overlap, "index.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("s001\n")
    with open(os.path.join(overlap, "s001.json"), "w", encoding="utf-8") as f:
        json.dump(docs["s001"], f)
    try:
        S.main(["--plan", plan_path, "--dir", dirs[0], "--dir", overlap, "--draw"])
        raise AssertionError("the same bundle name in two directories must refuse")
    except SystemExit as e:
        assert "more than one --dir" in str(e), e
    out = cli(["--plan", plan_path, "--dir", dirs[0], "--sweep", sweeps[0]])                # ambient set missing
    assert "SELECTION: NOT COMPUTED" in out and "ambient has 0" in out

    print("selftest ok: registered order of the 34 candidates and the spoke classes; consistency band (median "
          "includes the clear under test, +-2 Hz inclusive, under 3 clears unjudged); cell constraints (inconsistent, "
          "own air shots, attribution to 0.01 ms); admissibility (each pooled control set, selection shift > 2 Hz "
          "exclusive); score = min over stations; eligibility and G-analysis; shared level within 10 pp and "
          "per-station fallback; tie-breaks (interior, SNR with the lower station, shorter pulse, fewest fields, "
          "registered order); exploration gate at exactly 5/6; class check and G-class; refinement triggers and "
          "midpoints; completeness and replacement in place; not-evaluable candidates; reported counts; the "
          "seeded draw; the CLI end to end")
