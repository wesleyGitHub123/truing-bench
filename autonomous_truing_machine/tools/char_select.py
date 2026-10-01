"""Which in-band line would each f1 SELECTION RULE report? Host emulation over the firmware's own peak list.

EXPLORATORY (frozen-rig characterization, docs/SOLENOID_CAMPAIGN.md). The firmware's rule is "the lowest strong
peak in the f1 band whose SNR clears measurement_min_snr_db" (analysis.h, TRUING_DSP_SELECTION_RULE_VERSION 1).
This reads the strong peaks the firmware's own detector found (native_sweep TRUING_SWEEP_MODE=lines: frequency,
SNR, and the analysis window) and applies alternative rules to the SAME peak list, so a difference between rules is
the rule and nothing else:

    lowest     the firmware's rule (parity check: should match the board's clears where the windows agree)
    strongest  the in-band peak with the highest SNR
    local      the in-band peak most concentrated at the struck station's mic: highest local/far magnitude ratio
               over the same window (needs <name>.far.pcm); a spoke beside the local mic should win over the rim,
               the frame or a neighbouring spoke, which both mics hear

Only peaks at or above min_snr count as lines (B3.0's definition). A bundle with none reports nothing (a reject).

    python tools/char_select.py --root R --tag E1 [--min-snr 12]
    python tools/char_select.py --selftest
"""
import argparse
import csv
import os
import statistics
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import b30_verdict as bv          # noqa: E402
import wb_analysis as wa          # noqa: E402

FS = 48000
RULES = ("lowest", "strongest", "local")


def read_lines_rows(path):
    """{bundle: {"start": int, "n": int, "peaks": [(freq, snr)] in the f1 band}} from a lines CSV."""
    out = {}
    with open(path, encoding="utf-8") as f:
        for raw in f:
            if not raw.strip() or raw.startswith(("bundle,", "lines ")) or "," not in raw:
                continue
            row = next(csv.reader([raw.strip()]))
            if len(row) != len(bv.HEADER):
                continue
            d = dict(zip(bv.HEADER, row))
            if not d["window_start"].isdigit():
                continue
            b = out.setdefault(d["bundle"], {"start": int(d["window_start"]), "n": int(d["window_n"]), "peaks": [],
                                             "overflow": d["overflow"] == "1"})
            if d["idx"] != "" and d["in_f1_band"] == "1" and d["snr_db"] != "":
                b["peaks"].append((float(d["freq_hz"]), float(d["snr_db"])))
    return out


def mag_db_at(x, start, n, freqs, nfft=1 << 17, tol_hz=1.5):
    seg = x[start:start + n]
    if len(seg) < 64:
        return [None] * len(freqs)
    m = np.abs(np.fft.rfft(seg * np.hanning(len(seg)), nfft))
    fr = np.fft.rfftfreq(nfft, 1.0 / FS)
    out = []
    for f in freqs:
        k = (fr >= f - tol_hz) & (fr <= f + tol_hz)
        out.append(float(20 * np.log10(m[k].max() + 1e-12)) if k.any() else None)
    return out


def select(peaks, min_snr, ratios=None, overflow=False):
    """{rule: f1 or None} for one bundle. ratios: {freq: local-far dB} for the `local` rule. overflow: the detector
    found more strong peaks than TRUING_DSP_MAX_STRONG_PEAKS, which the firmware rejects as AMBIGUOUS_PEAK before any
    rule runs -- so every rule abstains."""
    lines = [(f, s) for f, s in peaks if s >= min_snr]
    if not lines or overflow:
        return {r: None for r in RULES}
    out = {"lowest": min(lines)[0], "strongest": max(lines, key=lambda t: t[1])[0], "local": None}
    if ratios:
        scored = [(ratios.get(f), f) for f, _ in lines if ratios.get(f) is not None]
        if scored:
            out["local"] = max(scored)[1]
    return out


def summarise(values):
    """clears, median, share within +-2 Hz of the median, spread."""
    v = [x for x in values if x is not None]
    if not v:
        return {"clears": 0, "median": None, "within": 0, "spread": None}
    med = statistics.median(v)
    return {"clears": len(v), "median": med, "within": sum(1 for x in v if abs(x - med) <= 2.0),
            "spread": max(v) - min(v)}


def run(root, tag, min_snr):
    d = os.path.join(root, tag)
    docs, _ = bv.load_bundles(d)
    rows = read_lines_rows(os.path.join(root, "iter", "%s.lines.local.csv" % tag))
    picks = {}
    for b, doc in docs.items():
        if b not in rows:
            continue
        r = rows[b]
        ratios = None
        if doc.get("far_pcm"):
            loc = wa.words_to_float(np.fromfile(os.path.join(d, doc["pcm"]), dtype="<i4"))
            far = wa.words_to_float(np.fromfile(os.path.join(d, doc["far_pcm"]), dtype="<i4"))
            fs = [f for f, s in r["peaks"] if s >= min_snr]
            ml, mf = mag_db_at(loc, r["start"], r["n"], fs), mag_db_at(far, r["start"], r["n"], fs)
            ratios = {f: (a - c) for f, a, c in zip(fs, ml, mf) if a is not None and c is not None}
        picks[b] = (doc, select(r["peaks"], min_snr, ratios, r["overflow"]))
    lines = ["selection rules on %s (lines >= %g dB SNR; same peak list for every rule)" % (tag, min_snr)]
    agree = [abs(p["lowest"] - doc["expect_f1_hz"]) <= 0.5 for doc, p in picks.values()
             if p["lowest"] is not None and doc.get("status") == "suspect" and doc.get("expect_f1_hz")]
    both_reject = sum(1 for doc, p in picks.values() if p["lowest"] is None and doc.get("status") != "suspect")
    lines.append("parity with the board ('lowest' vs its own f1, where both clear): %d/%d within 0.5 Hz; both reject %d; "
                 "the board clears %d, the emulation %d (the lines window is the full window_ms, the board's may be cut "
                 "short at the decay floor)" % (sum(agree), len(agree), both_reject,
                                                 sum(1 for doc, _ in picks.values() if doc.get("status") == "suspect"),
                                                 sum(1 for _, p in picks.values() if p["lowest"] is not None)))
    for rule in RULES:
        nf = [p[rule] for doc, p in picks.values() if doc.get("campaign_kind") == "no_fire"]
        nf_clears = sum(1 for x in nf if x is not None)
        lines.append("%-9s no-fire clears %d/%d" % (rule, nf_clears, len(nf)))
        by_p = {}
        for doc, p in picks.values():
            if doc.get("campaign_kind") == "strike":
                by_p.setdefault(int(doc.get("char_pulse_ms") or 0), []).append(p[rule])
        for pw, vals in sorted(by_p.items()):
            s = summarise(vals)
            hist = {}
            for x in vals:
                if x is not None:
                    k = int(round(x / 5.0) * 5)
                    hist[k] = hist.get(k, 0) + 1
            lines.append("    %4d ms  clears %2d/%-2d  within +-2 Hz %2d  median %7s  spread %6s   picks ~%s" % (
                pw, s["clears"], len(vals), s["within"], "%.1f" % s["median"] if s["median"] else "-",
                "%.1f" % s["spread"] if s["spread"] is not None else "-",
                ", ".join("%d Hz x%d" % kv for kv in sorted(hist.items()))))
    return "\n".join(lines)


def selftest():
    peaks = [(362.0, 15.0), (406.0, 13.0), (426.0, 20.0), (380.0, 5.0)]
    s = select(peaks, 12.0, {362.0: 3.0, 406.0: 6.0, 426.0: 11.0})
    assert s == {"lowest": 362.0, "strongest": 426.0, "local": 426.0}, s
    assert select([(380.0, 5.0)], 12.0) == {"lowest": None, "strongest": None, "local": None}
    assert select(peaks, 12.0)["local"] is None                      # no far channel: the rule abstains
    assert select(peaks, 12.0, overflow=True)["lowest"] is None       # AMBIGUOUS_PEAK: nothing is selected
    assert summarise([362.6, 363.0, 362.8, None, 400.0]) == {"clears": 4, "median": 362.9, "within": 3,
                                                             "spread": 400.0 - 362.6}
    fs = 48000
    t = np.arange(fs // 2) / fs
    x = 0.1 * np.sin(2 * np.pi * 426.0 * t) + 0.01 * np.sin(2 * np.pi * 362.0 * t)
    a, b = mag_db_at(x, 0, len(x), [426.0, 362.0])
    assert abs((a - b) - 20.0) < 1.0, (a, b)
    print("selftest ok")


def main():
    if "--selftest" in sys.argv:
        selftest()
        return 0
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--root", required=True)
    p.add_argument("--tag", required=True)
    p.add_argument("--min-snr", type=float, default=12.0)
    a = p.parse_args()
    print(run(a.root, a.tag, a.min_snr))
    return 0


if __name__ == "__main__":
    sys.exit(main())
