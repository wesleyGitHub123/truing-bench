"""Two-mic characterization: is a "clear" line the struck spoke, or the room / the whole rig?

EXPLORATORY. Nothing here is a verdict or an acceptance criterion; it describes captures.

One spoke is struck at one station; every capture carries the struck station's own mic ("local",
<name>.pcm) and the other station's mic ("far", <name>.far.pcm), sampled on the same frames. A spoke's
ring should be relatively louder beside the strike than at the far mic; a room line or a whole-rig ring
should not favour the local mic the same way. Three looks at that, each per bundle over ONE fixed ring
window (default 250-750 ms into the capture, Hann, bins below 200 Hz high-passed out and excluded):

  ratio      peaks of the LOCAL spectrum in the band; local/far level at each peak, dB
             (wb_analysis.delta_at, the workbench `loc` quantity). The no-fire median local-far
             offset (wb_analysis.delta_floor) is printed as the mics' baseline.
  coherence  Welch magnitude-squared coherence local vs far over sub-segments of the window, and the
             cross-spectrum magnitude; cross-spectrum peaks in the band whose coherence >= --coh-min.
             Coherence says the two mics hear the SAME source; it does not say where it is.
  sum        delay-and-sum: inter-mic lag by cross-correlation (bounded +-2 ms), align, add, peaks of
             the summed spectrum; annulus SNR (peak minus median of bins 100-300 Hz either side, the
             firmware's S3.8 SNR) of the sum vs local alone. Equal weights: a line heard 12 dB weaker
             at the far mic LOSES about 1 dB (1.25x amplitude vs sqrt(2)x noise); a line equally loud
             at both gains about 3 dB. So the gain is itself a localisation read-out, not a free boost.

Lines are clustered across bundles with b30_verdict.coherent_lines (+-2 Hz, >= --min-share of a group).
Strikes (campaign_kind "strike") and controls (campaign_kind "no_fire") are clustered separately and each
strike line says whether the controls show it too.

    python tools/char_twomic.py --dir <bundle dir> [--method ratio|coherence|sum|all] [--ref-f1 406.2] [--csv out.csv]
    python tools/char_twomic.py --selftest

numpy + stdlib, no board, no network.
"""
import argparse
import csv
import json
import os
import shutil
import sys
import tempfile

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import wb_analysis as wa  # noqa: E402
from b30_verdict import coherent_lines, load_bundles  # noqa: E402

FS = wa.FS
WIN_START_MS, WIN_LEN_MS = 250.0, 500.0
BAND = (300.0, 1500.0)
COH_MIN = 0.7
N_SEG = 8
MAX_LAG_MS = 2.0
REF_TOL_HZ = 5.0
SNR_ANNULUS_HZ = (100.0, 300.0)      # the firmware's snr_noise_offset_lo..hi
K_PEAKS = 10
LATE_MARGIN_MS = 20.0                # an impact closer than this to the window start is "late"
METHODS = ("ratio", "coherence", "sum")
# per method: (value columns printed, key the clusters are ranked by)
COLUMNS = {
    "ratio": ([("ratio_db", "ratio", "%+6.1f"), ("local_db", "local", "%6.1f")], "local_db"),
    "coherence": ([("coherence", "coh", "%6.2f"), ("cross_db", "xspec", "%6.1f")], "cross_db"),
    "sum": ([("snr_sum_db", "SNRsum", "%6.1f"), ("gain_db", "gain", "%+6.1f")], "sum_db"),
}
CSV_FIELDS = ["bundle", "group", "method", "freq_hz", "ratio_db", "local_db", "far_db", "prom_db", "coherence",
              "cross_db", "sum_db", "snr_sum_db", "snr_local_db", "gain_db", "lag_samples", "impact_ms"]


# ------------------------------------------------------------------ helpers

def snr_db(freqs, P_db, f0, tol=wa.LINE_TOL_HZ, annulus=SNR_ANNULUS_HZ):
    """Strongest bin within +-tol of f0 minus the median over lo <= |f - f_peak| <= hi (bins below the
    high-pass excluded). Mirrors truing_peaks_snr_db on a host spectrum."""
    m = np.where((freqs >= f0 - tol) & (freqs <= f0 + tol))[0]
    if len(m) == 0:
        return float("nan")
    i = m[int(np.argmax(P_db[m]))]
    d = np.abs(freqs - freqs[i])
    ann = (d >= annulus[0]) & (d <= annulus[1]) & (freqs >= wa.HP_HZ)
    return float(P_db[i] - np.median(P_db[ann])) if ann.any() else float("nan")


def welch_coherence(xl, xf, n_seg):
    """(freqs, magnitude-squared coherence, cross-spectrum dB) from n_seg Hann segments at 50 % overlap.
    Cross-spectrum is amplitude-corrected so two sines of amplitude a, b read about 20log10(sqrt(a*b))."""
    n = len(xl)
    seg = int(2 * n / (n_seg + 1))
    step = seg // 2
    w = np.hanning(seg)
    nfft = 1
    while nfft < 2 * seg:
        nfft *= 2
    Sll = Sff = Slf = 0
    for k in range(n_seg):
        a = k * step
        L = np.fft.rfft(xl[a:a + seg] * w, nfft)
        F = np.fft.rfft(xf[a:a + seg] * w, nfft)
        Sll = Sll + np.abs(L) ** 2
        Sff = Sff + np.abs(F) ** 2
        Slf = Slf + np.conj(L) * F
    coh = np.abs(Slf) ** 2 / np.maximum(Sll * Sff, 1e-300)
    cross = np.abs(Slf) / n_seg * (2.0 / w.sum()) ** 2
    return np.fft.rfftfreq(nfft, 1.0 / FS), coh, 10.0 * np.log10(np.maximum(cross, 1e-24)), FS / float(seg)


def xcorr_lag(xl, xf, max_lag):
    """Lag k (samples) maximising sum xl[n] * xf[n + k], |k| <= max_lag. Positive: far hears it later."""
    n = len(xl)
    nfft = 1
    while nfft < 2 * n:
        nfft *= 2
    r = np.fft.irfft(np.conj(np.fft.rfft(xl, nfft)) * np.fft.rfft(xf, nfft), nfft)
    ks = np.arange(-max_lag, max_lag + 1)
    return int(ks[int(np.argmax(r[ks % nfft]))])


def peaks(freqs, P_db, band, k=K_PEAKS):
    lo = max(band[0], wa.HP_HZ)
    return [(f, lv, pr) for f, lv, pr, _ in wa.top_lines(freqs, P_db, k=k, lo=lo, hi=band[1])
            if pr >= wa.PRESENCE_PROM_DB]


def med(recs, key):
    v = [r[key] for r in recs if r.get(key) is not None and np.isfinite(r[key])]
    return float(np.median(v)) if v else float("nan")


def fmt(v, spec):
    return spec % v if v is not None and np.isfinite(v) else " " * (len(spec % 0.0) - 1) + "-"


# ------------------------------------------------------------------ one bundle

def analyse(local_words, far_words, opt):
    """Per-bundle candidate lines for each method, plus the --ref-f1 read-outs."""
    raw_l = wa.words_to_float(local_words)
    xl, xf = wa.highpass(raw_l), wa.highpass(wa.words_to_float(far_words))
    n = min(len(xl), len(xf))
    a = int(round(opt.win_start_ms * FS / 1000.0))
    b = a + int(round(opt.win_len_ms * FS / 1000.0))
    if b > n:
        return None
    r = {"impact_ms": 1000.0 * wa.impact(raw_l)[1]}
    band = tuple(opt.band)

    # 1. ratio
    spectra = {"local": wa.spectrum(xl, a, b), "far": wa.spectrum(xf, a, b)}
    f, Pl = spectra["local"]
    Pf = spectra["far"][1]
    r["spectra"] = spectra
    r["ratio"] = [{"freq": fr, "ratio_db": wa.delta_at(spectra, fr), "local_db": lv,
                   "far_db": wa.line_level_db(f, Pf, fr), "prom_db": pr} for fr, lv, pr in peaks(f, Pl, band)]

    # 2. coherence
    fc, coh, xdb, res_hz = welch_coherence(xl[a:b], xf[a:b], opt.segments)
    r["coh_res_hz"] = res_hz
    r["coherence"] = []
    for fr, lv, pr in peaks(fc, xdb, band):
        c = float(coh[int(np.argmin(np.abs(fc - fr)))])
        if c >= opt.coh_min:
            r["coherence"].append({"freq": fr, "coherence": c, "cross_db": lv})

    # 3. delay-and-sum
    lag = xcorr_lag(xl[a:b], xf[a:b], int(round(opt.max_lag_ms * FS / 1000.0)))
    a2, b2 = max(a, -lag), min(b, n - lag)
    s = xl[a2:b2] + xf[a2 + lag:b2 + lag]
    fs_, Ps = wa.spectrum(s, 0, len(s))
    Pl2 = wa.spectrum(xl, a2, b2)[1]
    r["lag"] = lag
    r["sum"] = []
    for fr, lv, pr in peaks(fs_, Ps, band):
        ss, sl = snr_db(fs_, Ps, fr), snr_db(fs_, Pl2, fr)
        r["sum"].append({"freq": fr, "sum_db": lv, "snr_sum_db": ss, "snr_local_db": sl, "gain_db": ss - sl})
    r["top"] = max(r["sum"], key=lambda c: c["sum_db"]) if r["sum"] else None

    if opt.ref_f1 is not None:
        f0, t = opt.ref_f1, REF_TOL_HZ
        near = np.abs(fc - f0) <= t
        ss, sl = snr_db(fs_, Ps, f0, t), snr_db(fs_, Pl2, f0, t)
        r["ref"] = {"ratio_db": wa.line_level_db(f, Pl, f0, t) - wa.line_level_db(f, Pf, f0, t),
                    "coherence": float(coh[near].max()) if near.any() else float("nan"),
                    "snr_sum_db": ss, "snr_local_db": sl, "gain_db": ss - sl}
        r["ref_present"] = {m: any(abs(c["freq"] - f0) <= t for c in r[m]) for m in METHODS}
    return r


# ------------------------------------------------------------------ across bundles

def run(directory, opt):
    docs, excluded = load_bundles(directory)
    out = {"groups": {"strike": {}, "no_fire": {}}, "skipped": {"no far_pcm": 0, "dead mic": 0, "other kind": 0,
                                                               "excluded": 0, "short": 0},
           "pulses": set(), "clipping": 0, "n_listed": len(docs)}
    for name, doc in docs.items():
        g = {"strike": "strike", "no_fire": "no_fire"}.get(doc.get("campaign_kind"))
        if name in excluded:
            out["skipped"]["excluded"] += 1
            continue
        if g is None:
            out["skipped"]["other kind"] += 1
            continue
        meta, lw, fw = wa.load_bundle(os.path.join(directory, name + ".json"))
        if fw is None:
            out["skipped"]["no far_pcm"] += 1
            continue
        health = [wa.mic_health(w)["state"] for w in (lw, fw)]
        if "DEAD" in health or "NO DATA" in health:
            out["skipped"]["dead mic"] += 1
            continue
        out["clipping"] += "CLIPPING" in health
        r = analyse(lw, fw, opt)
        if r is None:
            out["skipped"]["short"] += 1
            continue
        if g == "strike":
            out["pulses"].add(meta.get("char_pulse_ms"))
        out["groups"][g][name] = r
    ctrl = [r["spectra"] for r in out["groups"]["no_fire"].values()]
    out["delta_floor_db"] = wa.delta_floor(ctrl) if ctrl else None
    return out


def cluster(res, method, tol, frac):
    caps = {n: [c["freq"] for c in r[method]] for n, r in res.items()}
    out = []
    for l in coherent_lines(caps, tol=tol, frac=frac):
        recs = [min(res[b][method], key=lambda c: abs(c["freq"] - l["freq_hz"])) for b in l["bundles"]]
        out.append({"freq": l["freq_hz"], "n": l["n"], "of": l["of"], "recs": recs})
    key = COLUMNS[method][1]
    out.sort(key=lambda c: (-c["n"], -med(c["recs"], key)))
    return out


def support(res, method, f, tol):
    recs = []
    for r in res.values():
        near = [c for c in r[method] if abs(c["freq"] - f) <= tol]
        if near:
            recs.append(min(near, key=lambda c: abs(c["freq"] - f)))
    return recs


def summarise(out, opt):
    """Per method: clustered strike lines (or control lines when there are no strikes) with control support."""
    S, C = out["groups"]["strike"], out["groups"]["no_fire"]
    summ = {}
    for m in opt.methods:
        tol = wa.LINE_TOL_HZ
        if m == "coherence":
            res = [r["coh_res_hz"] for r in list(S.values()) + list(C.values())]
            tol = max(tol, 0.5 * res[0]) if res else tol     # Welch segments are coarser than +-2 Hz
        prim, other = (S, C) if S else (C, {})
        lines = cluster(prim, m, tol, opt.min_share)
        for l in lines:
            l["ctrl"] = support(other, m, l["freq"], tol) if other else None
        ctrl_only = [l for l in cluster(C, m, tol, opt.min_share)
                     if S and all(abs(l["freq"] - s["freq"]) > tol for s in lines)]
        summ[m] = {"tol": tol, "primary": "strike" if S else "no_fire", "lines": lines, "ctrl_only": ctrl_only}
    return summ


def render(out, summ, opt):
    S, C = out["groups"]["strike"], out["groups"]["no_fire"]
    sk = ", ".join("%d %s" % (v, k) for k, v in out["skipped"].items() if v) or "none"
    o = ["char_twomic (EXPLORATORY): %s" % opt.dir,
         "bundles: %d listed; analysed strike %d, no_fire %d; skipped: %s%s" % (
             out["n_listed"], len(S), len(C), sk, "; %d with a clipping mic" % out["clipping"] if out["clipping"] else ""),
         "window %.0f-%.0f ms (Hann), band %.0f-%.0f Hz, bins < %.0f Hz excluded, line = prominence >= %.0f dB, "
         "cluster support >= %.0f%%" % (opt.win_start_ms, opt.win_start_ms + opt.win_len_ms, opt.band[0], opt.band[1],
                                       wa.HP_HZ, wa.PRESENCE_PROM_DB, 100 * opt.min_share)]
    if S:
        imp = [r["impact_ms"] for r in S.values()]
        late = sum(1 for t in imp if t > opt.win_start_ms - LATE_MARGIN_MS)
        o.append("strike impact (loudest 5 ms, local): median %.0f ms, range %.0f-%.0f; %d strike(s) within %.0f ms "
                 "of / after the window start%s" % (np.median(imp), min(imp), max(imp), late, LATE_MARGIN_MS,
                                                   "  <- window may hold the impact, not the ring" if late else ""))
        o.append("char_pulse_ms: %s" % ", ".join(str(p) for p in sorted(out["pulses"], key=str)))
    if out["delta_floor_db"] is not None:
        o.append("mic baseline (no-fire median local-far, 150-2500 Hz): %+.1f dB" % out["delta_floor_db"])
    if not S and not C:
        o.append("nothing to analyse")
        return "\n".join(o)
    for m in opt.methods:
        sm = summ[m]
        cols = COLUMNS[m][0]
        o.append("")
        head = {"ratio": "local/far level at local-spectrum peaks, dB",
                "coherence": "cross-spectrum peaks with coherence >= %.2f (cluster tol +-%.1f Hz)" % (opt.coh_min, sm["tol"]),
                "sum": "delay-and-sum peaks; annulus SNR of the sum, gain vs local alone, dB"}[m]
        o.append("[%s] %s" % (m, head))
        if m == "sum":
            groups = [("strike", S), ("no_fire", C)]
            for gname, res in groups:
                if not res:
                    continue
                lags = [r["lag"] for r in res.values()]
                tops = [r["top"] for r in res.values() if r["top"]]
                edge = sum(1 for v in lags if abs(v) >= int(round(opt.max_lag_ms * FS / 1000.0)))
                o.append("  %s: lag median %+d samples (range %+d..%+d%s)%s" % (
                    gname, int(np.median(lags)), min(lags), max(lags),
                    ", %d at the +-%.1f ms bound: no real alignment" % (edge, opt.max_lag_ms) if edge else "",
                    "; top peak median %.1f Hz, SNR sum %.1f vs local %.1f, gain %+.1f dB" % (
                        med(tops, "freq"), med(tops, "snr_sum_db"), med(tops, "snr_local_db"), med(tops, "gain_db"))
                    if tops else ""))
        prim = sm["primary"]
        o.append("  %8s  %7s  %s  | %s" % ("freq Hz", prim[:7], "  ".join("%6s" % c[1] for c in cols),
                                            "no-fire (n, " + cols[0][1] + ")" if prim == "strike" else ""))
        if not sm["lines"]:
            o.append("  (no line recurs in >= %.0f%% of the %s bundles)" % (100 * opt.min_share, prim))
        for l in sm["lines"][:opt.top]:
            vals = "  ".join(fmt(med(l["recs"], k), f) for k, _, f in cols)
            ctrl = ""
            if l["ctrl"] is not None:
                ctrl = "%d/%d %s" % (len(l["ctrl"]), len(C), fmt(med(l["ctrl"], cols[0][0]), cols[0][2]).strip()
                                     if l["ctrl"] else "")
            o.append("  %8.1f  %7s  %s  | %s" % (l["freq"], "%d/%d" % (l["n"], l["of"]), vals, ctrl))
        if sm["ctrl_only"]:
            o.append("  no-fire-only lines: " + ", ".join("%.1f (%d/%d, %s %s)" % (
                l["freq"], l["n"], l["of"], cols[0][1], fmt(med(l["recs"], cols[0][0]), cols[0][2]).strip())
                for l in sm["ctrl_only"][:opt.top]))
    if opt.ref_f1 is not None:
        o.append("")
        o.append("ref f1 %.1f Hz (+-%.0f Hz):" % (opt.ref_f1, REF_TOL_HZ))
        for gname, res in (("strike", S), ("no_fire", C)):
            if not res:
                continue
            refs = [r["ref"] for r in res.values()]
            pres = {m: sum(r["ref_present"][m] for r in res.values()) for m in METHODS}
            N = len(res)
            parts = []
            if "ratio" in opt.methods:
                parts.append("ratio %+.1f dB (local peak there %d/%d)" % (med(refs, "ratio_db"), pres["ratio"], N))
            if "coherence" in opt.methods:
                parts.append("coherence %.2f (passes %d/%d)" % (med(refs, "coherence"), pres["coherence"], N))
            if "sum" in opt.methods:
                parts.append("summed SNR %.1f vs local %.1f, gain %+.1f dB (peak %d/%d)" % (
                    med(refs, "snr_sum_db"), med(refs, "snr_local_db"), med(refs, "gain_db"), pres["sum"], N))
            o.append("  %-8s %s" % (gname, "; ".join(parts)))
    return "\n".join(o)


def write_csv(path, out, opt):
    with open(path, "w", encoding="utf-8", newline="") as f:
        w = csv.DictWriter(f, fieldnames=CSV_FIELDS, restval="")
        w.writeheader()
        for g, res in out["groups"].items():
            for name, r in res.items():
                for m in opt.methods:
                    for c in r[m]:
                        row = {k: ("%.3f" % v if isinstance(v, float) else v) for k, v in c.items() if k in CSV_FIELDS}
                        row.update(bundle=name, group=g, method=m, freq_hz="%.3f" % c["freq"],
                                   lag_samples=r["lag"], impact_ms="%.1f" % r["impact_ms"])
                        w.writerow(row)


def options(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dir", help="bundle directory (has index.txt)")
    ap.add_argument("--method", choices=METHODS + ("all",), default="all")
    ap.add_argument("--ref-f1", type=float, help="report what each method shows within +-5 Hz of this")
    ap.add_argument("--csv", help="per bundle, per method, per candidate line")
    ap.add_argument("--win-start-ms", type=float, default=WIN_START_MS, help="ring window start, from capture start")
    ap.add_argument("--win-len-ms", type=float, default=WIN_LEN_MS)
    ap.add_argument("--band", type=float, nargs=2, default=list(BAND), metavar=("LO", "HI"))
    ap.add_argument("--coh-min", type=float, default=COH_MIN)
    ap.add_argument("--segments", type=int, default=N_SEG, help="Welch segments inside the window")
    ap.add_argument("--max-lag-ms", type=float, default=MAX_LAG_MS)
    ap.add_argument("--min-share", type=float, default=0.5, help="cluster support, fraction of a group")
    ap.add_argument("--top", type=int, default=8, help="clusters printed per method")
    ap.add_argument("--selftest", action="store_true")
    opt = ap.parse_args(argv)
    opt.methods = METHODS if opt.method == "all" else (opt.method,)
    return ap, opt


# ------------------------------------------------------------------ selftest

def selftest():
    rng = np.random.default_rng(11)
    N, t0, LAG, NOISE = 57600, 9600, 17, 1e-4
    t = np.arange(N) / float(FS)

    def to_words(x):
        return (np.clip(np.round(x * wa.FULL_SCALE), -wa.FULL_SCALE, wa.FULL_SCALE - 1).astype("<i4") << 8)

    def capture(strike):
        """(local, far) floats. Far hears every source LAG samples later; the spoke 12 dB quieter there."""
        room = 0.0015 * np.sin(2 * np.pi * 353.0 * t + rng.uniform(0, 6.3))
        spoke = np.zeros(N)
        if strike:
            tt = np.maximum(t - t0 / float(FS), 0)
            a = 0.01 * 10 ** (rng.uniform(-1, 1) / 20)
            spoke = np.where(t >= t0 / float(FS), a * np.exp(-tt / 0.4) * np.sin(2 * np.pi * 406.0 * tt), 0)
            spoke[t0:t0 + 96] += 0.1 * rng.standard_normal(96)         # the impact click
        far_src = room + spoke / 10 ** (12 / 20.0)
        far = np.concatenate([np.zeros(LAG), far_src[:-LAG]])
        return (room + spoke + NOISE * rng.standard_normal(N), far + NOISE * rng.standard_normal(N))

    d = tempfile.mkdtemp(prefix="char_twomic_selftest_")
    try:
        names = []
        for kind, k in (("strike", 6), ("no_fire", 6)):
            for i in range(k):
                name = "%s_%02d" % (kind, i)
                xl, xf = capture(kind == "strike")
                to_words(xl).tofile(os.path.join(d, name + ".pcm"))
                to_words(xf).tofile(os.path.join(d, name + ".far.pcm"))
                with open(os.path.join(d, name + ".json"), "w") as f:
                    json.dump({"campaign_kind": kind, "char_pulse_ms": 6.0 if kind == "strike" else 0.0,
                               "pcm": name + ".pcm", "far_pcm": name + ".far.pcm"}, f)
                names.append(name)
        # a strike without the far channel is skipped and counted
        to_words(capture(True)[0]).tofile(os.path.join(d, "strike_nofar.pcm"))
        with open(os.path.join(d, "strike_nofar.json"), "w") as f:
            json.dump({"campaign_kind": "strike", "pcm": "strike_nofar.pcm"}, f)
        names.append("strike_nofar")
        with open(os.path.join(d, "index.txt"), "w") as f:
            f.write("\n".join(names) + "\n")

        _, opt = options(["--dir", d, "--ref-f1", "406.2", "--csv", os.path.join(d, "out.csv")])
        out = run(d, opt)
        summ = summarise(out, opt)
        report = render(out, summ, opt)
        write_csv(opt.csv, out, opt)
        S, C = out["groups"]["strike"], out["groups"]["no_fire"]
        assert len(S) == 6 and len(C) == 6 and out["skipped"]["no far_pcm"] == 1, out["skipped"]

        def line(m, f, tol=3.0):
            ls = [l for l in summ[m]["lines"] if abs(l["freq"] - f) <= tol]
            assert ls, "%s: no strike cluster near %.0f Hz\n%s" % (m, f, report)
            return ls[0]

        # 1. ratio: the spoke favours the local mic by 12 dB, the room line by nothing
        l406, l353 = line("ratio", 406), line("ratio", 353)
        assert l406["n"] == 6 and abs(med(l406["recs"], "ratio_db") - 12.04) < 1.0, med(l406["recs"], "ratio_db")
        assert abs(med(l353["recs"], "ratio_db")) < 1.0, med(l353["recs"], "ratio_db")
        assert len(l353["ctrl"]) == 6 and not l406["ctrl"], "controls must show 353, never 406"
        assert abs(out["delta_floor_db"]) < 1.0, out["delta_floor_db"]
        # 2. coherence: the same source at both mics, whatever its level ratio
        for f in (406, 353):
            assert med(line("coherence", f)["recs"], "coherence") > 0.9
        assert len(line("coherence", 353)["ctrl"]) == 6 and not line("coherence", 406)["ctrl"]
        # 3. delay-and-sum: lag recovered; the equal line gains ~+3 dB, the 12 dB-quieter-at-far line ~-1 dB
        assert all(r["lag"] == LAG for r in list(S.values()) + list(C.values())), [r["lag"] for r in S.values()]
        g353, g406 = med(line("sum", 353)["recs"], "gain_db"), med(line("sum", 406)["recs"], "gain_db")
        assert 1.5 < g353 < 4.5, g353
        assert -2.5 < g406 < 0.3, g406
        # controls: the room line is there, the spoke is not, in every method
        for m in METHODS:
            ctrl_lines = cluster(C, m, summ[m]["tol"], 0.5)
            assert any(abs(l["freq"] - 353) <= 3 for l in ctrl_lines), m
            assert not any(abs(l["freq"] - 406) <= 5 for l in ctrl_lines), m
        # ref f1 read-outs
        ref = [r["ref"] for r in S.values()]
        assert abs(med(ref, "ratio_db") - 12.04) < 1.0 and med(ref, "coherence") > 0.9
        assert all(r["ref_present"]["ratio"] for r in S.values())
        # a 6 dB-prominent noise maximum lands within +-5 Hz of any frequency now and then; never a majority
        assert sum(r["ref_present"]["ratio"] for r in C.values()) <= 2, report
        # window placement: the impact sits 50 ms before the ring window, so no late-strike warning
        assert all(abs(r["impact_ms"] - 200) < 10 for r in S.values()) and "0 strike(s)" in report, report
        with open(opt.csv) as f:
            assert sum(1 for _ in f) > 1
        print(report)
        print("")
        print("selftest ok: ratio +%.1f dB at 406 / %+.1f dB at 353; coherence high at both; lag %d recovered; "
              "sum gain %+.1f dB at the shared 353 line, %+.1f dB at the local-heavy 406 line; controls show 353 not 406"
              % (med(l406["recs"], "ratio_db"), med(l353["recs"], "ratio_db"), LAG, g353, g406))
    finally:
        shutil.rmtree(d, ignore_errors=True)


def main():
    ap, opt = options()
    if opt.selftest:
        selftest()
        return
    if not opt.dir:
        ap.error("--dir is required")
    out = run(opt.dir, opt)
    print(render(out, summarise(out, opt), opt))
    if opt.csv:
        write_csv(opt.csv, out, opt)


if __name__ == "__main__":
    main()
