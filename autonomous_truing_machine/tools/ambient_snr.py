"""Ambient-referenced SNR, measured on the bundles already on disk (plan item I2).

The firmware's SNR gate is a LOCAL spectral contrast: truing_peaks_snr_db (peaks.c) subtracts
the median log-magnitude of an annulus of bins 100-300 Hz either side of f1. It never reads the
pre-roll, never reads noise_floor_dbfs, and has no time-domain component. Two consequences
worth testing rather than assuming: a mains or motor line landing inside that annulus depresses
reported SNR, and flat broadband noise has low local contrast - which is plausibly why pass-C
ambient (nobody plucked) still scored up to 9.9 dB while genuine plucks scored 2-11 dB.

Every bundle carries ~200 ms of pre-excitation ambient, so the alternative needs no new data:

    ambient-referenced SNR = 20*log10( rms_ac(window) / rms_ac(pre-roll) )

with rms_ac the AC-coupled RMS on the same (word >> 8) scale the DSP multiplies by 2**-23 - the
INMP441's DC/sub-audio offset excluded, so numbers compare with the research chain. The
question is not raw pass rate but discrimination, judged the Phase-B way: do plucks clear it
and do the no-pluck controls (pass C) still fail?

    python tools/ambient_snr.py test/fixtures/acoustic/captures/_campaign
    python tools/ambient_snr.py nano_handpluck_provisional nano_ambient_ambiguous

CAVEAT, printed with the summary rather than buried: the pre-roll length is derived from
metadata (n_words minus capture_us), not measured - no field records how many pre-trigger
words the ring actually delivered (audio_i2s.c clamps it to ring_filled). On the orchestrator
path the mic has drained since boot so the ring is full in practice; this tool can only say
"assumed". That argues for recording the delivered pre-roll count in the capture diagnostics
(a tiny observability addition), which would make every future ambient analysis confirmed.

Output is a finding, not a constant change. If an ambient-referenced measure discriminates
better than the annulus SNR, it becomes a Phase B candidate alongside prominence_db = 12,
fitted against the final excitation - never accepted on hand-pluck data alone.

Requires nothing outside the standard library, and no hardware.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import capture_wav as cw  # noqa: E402  (same loading, same scale, same caveats)

CAMPAIGN = cw.CAMPAIGN


def group_of(name):
    """Which session a bundle belongs to, from its self-labelling name.

    Campaign bundles are <pass>_sp<N>_a<M>_seq<S>; the checked-in fixtures keep their own
    names. Anything unparseable is its own group - the summary never merges unknowns in.
    """
    head = name.split("_", 1)[0]
    if len(head) == 1 and head.isalpha():
        return head
    return name


def fmt(v, nd=1):
    if not isinstance(v, (int, float)) or v == float("-inf"):
        return "-"
    return "%.*f" % (nd, v)


def median(xs):
    xs = sorted(xs)
    n = len(xs)
    if n == 0:
        return None
    return xs[n // 2] if n % 2 else 0.5 * (xs[n // 2 - 1] + xs[n // 2])


def one_bundle(meta_path):
    meta, samples = cw.load(meta_path)
    name = meta.get("name") or os.path.splitext(os.path.basename(meta_path))[0]
    pre = cw.preroll_samples(meta)
    cut = cw.window_slice(meta, samples)
    row = {
        "name": name,
        "pass": group_of(name),
        "status": meta.get("status") or meta.get("expect_status") or "?",
        "reason": meta.get("reason") or meta.get("expect_reason") or "?",
        "f1_hz": meta.get("expect_f1_hz"),
        "board_snr_db": meta.get("expect_snr_db"),
        "onset_sample": meta.get("expect_onset_sample"),
    }
    if pre is None or pre <= 0 or pre >= len(samples):
        row["ambient_ref_snr_db"] = None
        row["preroll_rms_ac_dbfs"] = None
        row["preroll_n_assumed"] = None
        return row
    row["preroll_n_assumed"] = pre          # ASSUMED: see module docstring
    pre_stats = cw.pcm_stats(samples[:pre])
    row["preroll_rms_ac_dbfs"] = pre_stats["rms_ac_dbfs"]
    if cut is None:
        row["ambient_ref_snr_db"] = None
        row["window_rms_ac_dbfs"] = None
        return row
    start, count = cut
    win_stats = cw.pcm_stats(samples[start:start + count])
    row["window_rms_ac_dbfs"] = win_stats["rms_ac_dbfs"]
    row["ambient_ref_snr_db"] = (row["window_rms_ac_dbfs"] - row["preroll_rms_ac_dbfs"]
                                 if win_stats["rms_ac_dbfs"] > float("-inf")
                                 and pre_stats["rms_ac_dbfs"] > float("-inf") else None)
    return row


def summary(rows):
    """Per-group medians and ranges: pass C is the no-pluck control a candidate must keep
    out; the cleared captures are what a candidate must let through."""
    print("\n%-18s %5s  %11s  %13s  %11s  %13s"
          % ("group", "n", "med amb-SNR", "amb range", "med annulus", "annulus range"))
    groups = {}
    for r in rows:
        groups.setdefault("%s %s" % (r["pass"], r["status"]), []).append(r)
    for key in sorted(groups):
        g = [r for r in groups[key] if isinstance(r["ambient_ref_snr_db"], (int, float))]
        if not g:
            print("%-18s %5d  (no window/preroll computable)" % (key, len(groups[key])))
            continue
        ambs = [r["ambient_ref_snr_db"] for r in g]
        ann = [r["board_snr_db"] for r in g if isinstance(r["board_snr_db"], (int, float))]
        print("%-18s %5d  %11s  %5s..%-5s  %11s  %5s..%-5s"
              % (key, len(g), fmt(median(ambs)), fmt(min(ambs)), fmt(max(ambs)),
                 fmt(median(ann)), fmt(min(ann)), fmt(max(ann))))


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Ambient-referenced SNR over capture bundles (plan item I2).",
        epilog="A bundle argument may be a name, a .json path, a bundle stem, or a directory.")
    ap.add_argument("bundles", nargs="+", help="bundle names, paths, or directories")
    ap.add_argument("--csv", default=os.path.join(CAMPAIGN, "ambient_snr.csv"),
                    help="where to write the per-bundle CSV (default: %(default)s)")
    args = ap.parse_args(argv)

    paths = []
    for token in args.bundles:
        for path in cw.resolve(token):
            if path not in paths:
                paths.append(path)

    rows = []
    for path in paths:
        row = one_bundle(path)
        rows.append(row)
        pre_note = "" if row["preroll_n_assumed"] is None \
            else "  pre-roll %d samples ASSUMED" % row["preroll_n_assumed"]
        print("%-28s %-10s %-22s f1 %-8s annulus %-6s ambient-ref %-6s%s"
              % (row["name"], row["status"], row["reason"],
                 fmt(row["f1_hz"], 1), fmt(row["board_snr_db"]),
                 fmt(row["ambient_ref_snr_db"]), pre_note))

    summary(rows)

    # The finding, quantified from this run, for BOTH measures: the widest reading any no-pluck
    # control reaches, against the narrowest cleared capture - the gap a Phase-B gate sits in.
    ambient = [r for r in rows if r["pass"] == "C"]
    cleared = [r for r in rows if r["status"] == "suspect"]
    for label, key in (("ambient-ref", "ambient_ref_snr_db"), ("annulus (board)", "board_snr_db")):
        amb = [r[key] for r in ambient if isinstance(r.get(key), (int, float))]
        clr = [r[key] for r in cleared if isinstance(r.get(key), (int, float))]
        if not amb or not clr:
            continue
        amb_max, clr_min = max(amb), min(clr)
        print("\n%s:" % label)
        print("  ambient controls (pass C): max %s dB over %d bundles" % (fmt(amb_max), len(amb)))
        print("  cleared captures:          min %s dB over %d bundles" % (fmt(clr_min), len(clr)))
        if clr_min > amb_max:
            print("  SEPARATED: a gate between %s and %s dB lets every clear through and every "
                  "control out - on this data" % (fmt(amb_max), fmt(clr_min)))
        else:
            print("  NOT SEPARATED on this data: ranges overlap; see the CSV for where.")

    parent = os.path.dirname(os.path.abspath(args.csv))
    if parent:
        os.makedirs(parent, exist_ok=True)
    cols = ["name", "pass", "status", "reason", "f1_hz", "board_snr_db", "ambient_ref_snr_db",
            "preroll_rms_ac_dbfs", "window_rms_ac_dbfs", "preroll_n_assumed", "onset_sample"]
    with open(args.csv, "w", encoding="utf-8") as fh:
        fh.write(",".join(cols) + "\n")
        for r in rows:
            fh.write(",".join("" if r.get(c) is None else str(r.get(c)) for c in cols) + "\n")
    print("\n%d bundle(s) -> %s" % (len(rows), args.csv))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())