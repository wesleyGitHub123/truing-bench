"""Exploratory excitation probe: does a shorter pulse give the spoke back its ring?

B3.2 halted at G-analysis with 4 strike clears in 168. The diagnostics that followed put the blame on the
excitation rather than the analysis: ring sustain falls monotonically as the pulse gets longer (LEFT
-34.3 dB at 40 ms, -38.4 dB at 85 ms, n=24 each), which is what a plunger resting on the spoke and damping
its own ring looks like. B2-M3 set `p_reach` at 40 ms (LEFT) and 60 ms (RIGHT) as the shortest width that
strikes 10/10, so nothing below those has ever been fired. This fires them.

    sweep    interleaved strikes across pulse widths, round-robin so drift and heating hit every width alike
    pluck    cued hand pluck: the PC beeps as the capture command goes out, the operator plucks on the beep
             (20/20 captures caught a pluck this way against 4/29 uncued)
    report   the two measures below over any bundle directory

Everything is EXPLORATORY. Captures go to test/fixtures/acoustic/captures/_explore/ (gitignored), never
_campaign/. There is no plan, no trial number, no fixed n and no exclusion ledger; nothing here can be
read as campaign evidence, and anything promising has to be confirmed by a fresh registered run.

Measures, fixed before the data (see the plan):
    sustain  median over a width's shots of (RMS 250-550 ms after impact) - (impact peak), above 200 Hz.
             Higher is more ring. This is the number the table above is in.
    reach    share of a width's shots whose impact peak lands within 6 dB of the reference width's median.
             Below p_reach the plunger sometimes misses, and a width that only lands half the time is not
             usable however well it rings.
Reported but not decisive: the strongest narrow peak, its prominence over a smoothed floor, and whether
the shots at one width agree on it.

    python tools/explore_pulse_sweep.py sweep  --spoke 0 --widths 15,20,25,30,35,40,50,60,85 --count 6
    python tools/explore_pulse_sweep.py pluck  --spoke 0 --count 15
    python tools/explore_pulse_sweep.py report --dir test/fixtures/acoustic/captures/_explore
    python tools/explore_pulse_sweep.py report --dir test/fixtures/acoustic/captures/_campaign --strikes-only
"""
import argparse
import glob
import json
import os
import sys
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import campaign_runner as cr          # noqa: E402  (WS, take_shot, DEFAULT_*; one transport, one bundle writer)

FS = 48000
HP_HZ = 200.0                          # below this the chain's drift dominates and swamps any ring
ENV_N = 240                            # 5 ms envelope frames
LATE = (0.25, 0.55)                    # the ring window, relative to the impact
REACH_TOL_DB = 6.0
EXPLORE_DIR = os.path.join("test", "fixtures", "acoustic", "captures", "_explore")
NOTE = ("EXPLORATORY, not campaign data: excitation probe after the B3.2 G-analysis halt. No plan, no trial "
        "numbers, no fixed n, no exclusion ledger. Nothing here supports a claim without a fresh registered run.")


def db(v):
    return 20.0 * np.log10(max(float(v), 1e-12))


def load(meta_path):
    j = json.load(open(meta_path))
    w = np.fromfile(os.path.join(os.path.dirname(meta_path), j["pcm"]), dtype="<i4")
    x = (w >> 8).astype(np.float64) / 2 ** 23
    return j, x - x.mean()


def highpass(x, fc=HP_HZ):
    X = np.fft.rfft(x)
    f = np.fft.rfftfreq(len(x), 1.0 / FS)
    X[f < fc] = 0
    return np.fft.irfft(X, len(x))


def envelope(y):
    fr = y[: len(y) // ENV_N * ENV_N].reshape(-1, ENV_N)
    return np.sqrt((fr ** 2).mean(axis=1)), ENV_N / float(FS)


def sustain_of(x):
    """(sustain dB, impact peak dBFS, impact time s) or None when the ring window runs off the capture."""
    y = highpass(x)
    e, dt = envelope(y)
    i = int(e.argmax())
    a, b = i + int(LATE[0] / dt), i + int(LATE[1] / dt)
    if b > len(e) or e[i] <= 0:
        return None
    late = float(np.sqrt((e[a:b] ** 2).mean()))
    return db(late) - db(e[i]), db(e[i]), i * dt


def top_peak(x, t0, dur=0.30, lo=HP_HZ + 50.0, hi=2500.0):
    """(frequency, prominence dB) of the strongest narrow line just after the impact, or None.
    The search starts above the high-pass edge: the filter's own skirt otherwise wins every time and the
    'line' it reports is the 200 Hz cutoff, not anything in the capture."""
    y = highpass(x)
    a = int(t0 * FS)
    b = min(a + int(dur * FS), len(y))
    if b - a < 4096:
        return None
    seg = y[a:b] * np.hanning(b - a)
    P = 20.0 * np.log10(np.abs(np.fft.rfft(seg)) + 1e-12)
    f = np.fft.rfftfreq(b - a, 1.0 / FS)
    w = int(60.0 / (f[1] - f[0])) | 1
    prom = P - np.convolve(P, np.ones(w) / w, mode="same")
    m = np.where((f >= lo) & (f <= hi))[0]
    i = m[int(np.argmax(prom[m]))]
    return float(f[i]), float(prom[i])


# ---------------------------------------------------------------- capture

def _context(label, spoke):
    return {"seq_counter": [0],
            "rig_id_by_station": {"LEFT": cr.DEFAULT_RIG_ID_LEFT, "RIGHT": cr.DEFAULT_RIG_ID_RIGHT},
            "label": label, "session": time.strftime("%Y%m%d-%H%M"), "note": NOTE}


def _args(out, host, timeout):
    return argparse.Namespace(host=host, timeout=timeout, out=out)


def run_sweep(a):
    widths = [int(w) for w in a.widths.split(",") if w.strip()]
    for w in widths:
        cr.measure_once_arg(a.spoke, False, w)        # refuse an out-of-range width before touching the board
    os.makedirs(a.out, exist_ok=True)
    args, ctx = _args(a.out, a.host, a.timeout), _context(a.label, a.spoke)
    ws = cr.WS(a.host, 80, "/ws")
    kept = 0
    try:
        # round-robin: every width sees the same drift, heating and room over the run
        for r in range(a.count):
            for w in widths:
                cr.log("[round %d/%d] spoke %d  %d ms" % (r + 1, a.count, a.spoke, w))
                outcome, _ = cr.take_shot(ws, args, ctx, a.spoke, "strike", w,
                                          "%s_sp%d_p%d_r%d" % (a.label, a.spoke, w, r), {})
                kept += outcome == "kept"
                time.sleep(a.interval)
    except KeyboardInterrupt:
        cr.log("stopped by operator")
    finally:
        ws.close()
    cr.log("%s: %d captures in %s" % (a.label, kept, a.out))


def run_pluck(a):
    import winsound                                    # Windows-only, and only this mode needs it
    import threading

    def beep():
        threading.Thread(target=winsound.Beep, args=(2000, 90), daemon=True).start()

    os.makedirs(a.out, exist_ok=True)
    args, ctx = _args(a.out, a.host, a.timeout), _context(a.label, a.spoke)
    ws = cr.WS(a.host, 80, "/ws")
    print("test beep in 3 s ...", flush=True)
    time.sleep(3)
    beep()
    time.sleep(0.6)
    print("pluck ONCE on each beep, then hold still until the next one", flush=True)
    time.sleep(2.0)
    kept = 0
    try:
        for r in range(a.count):
            beep()
            outcome, _ = cr.take_shot(ws, args, ctx, a.spoke, "no_fire", 0,
                                      "%s_sp%d_r%d" % (a.label, a.spoke, r), {})
            kept += outcome == "kept"
            time.sleep(a.interval)
    except KeyboardInterrupt:
        cr.log("stopped by operator")
    finally:
        ws.close()
    cr.log("%s: %d captures in %s" % (a.label, kept, a.out))


# ---------------------------------------------------------------- report

def run_report(a):
    groups = {}
    for p in sorted(glob.glob(os.path.join(a.dir, "*.json"))):
        if p.endswith("_plan.json"):
            continue
        j, x = load(p)
        if a.strikes_only and j.get("campaign_kind") != "strike":
            continue
        if a.block and j.get("campaign_block") != a.block:
            continue
        s = sustain_of(x)
        if s is None:
            continue
        station = j.get("campaign_station") or "?"
        key = (station, float(j.get("pulse_ms") or 0.0))
        groups.setdefault(key, []).append((s, top_peak(x, s[2]), j))

    if not groups:
        raise SystemExit("no usable captures in %s" % a.dir)

    # reach is measured against one width, so the misses at short widths are visible as misses
    ref = None
    for key in sorted(groups):
        if a.reference and abs(key[1] - a.reference) < 0.01:
            ref = float(np.median([g[0][1] for g in groups[key]]))
    print("sustain = (RMS %.0f-%.0f ms after impact) - (impact peak), above %.0f Hz. higher = more ring."
          % (LATE[0] * 1000, LATE[1] * 1000, HP_HZ))
    if ref is not None:
        print("reach   = share of shots whose impact peak is within %.0f dB of the %.0f ms median (%.1f dBFS)."
              % (REACH_TOL_DB, a.reference, ref))
    print()
    print("%-6s %8s %4s %10s %10s %8s   %s" % ("st", "pulse", "n", "sustain", "peak dBFS", "reach", "strongest line"))
    for key in sorted(groups):
        rows = groups[key]
        sus = np.array([g[0][0] for g in rows])
        pk = np.array([g[0][1] for g in rows])
        reach = "-" if ref is None else "%d/%d" % (int((pk >= ref - REACH_TOL_DB).sum()), len(pk))
        peaks = [g[1] for g in rows if g[1]]
        if peaks:
            fr = np.array([p[0] for p in peaks])
            med = float(np.median(fr))
            agree = int((np.abs(fr - med) / med < 0.02).sum())
            line = "%.1f Hz +%.0f dB, %d/%d agree" % (med, np.median([p[1] for p in peaks]), agree, len(peaks))
        else:
            line = "-"
        print("%-6s %6.0f ms %4d %9.1f %10.1f %8s   %s" % (
            key[0], key[1], len(rows), np.median(sus), np.median(pk), reach, line))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="mode", required=True)

    def common(q):
        q.add_argument("--spoke", type=int, default=0, help="physical spoke under the plunger (default 0)")
        q.add_argument("--count", type=int, default=6, help="rounds (sweep) or plucks (pluck)")
        q.add_argument("--interval", type=float, default=0.5, help="seconds between shots (default 0.5)")
        q.add_argument("--host", default=cr.DEFAULT_HOST)
        q.add_argument("--timeout", type=float, default=20.0)
        q.add_argument("--out", default=EXPLORE_DIR, help="bundle directory (default %s)" % EXPLORE_DIR)

    s = sub.add_parser("sweep", help="interleaved strikes across pulse widths")
    common(s)
    s.add_argument("--widths", default="15,20,25,30,35,40,50,60,85", help="comma list of pulse widths in ms")
    s.add_argument("--label", default="EXPLORE-pulsesweep")

    k = sub.add_parser("pluck", help="cued hand pluck, the PC beeps as each capture starts")
    common(k)
    k.add_argument("--label", default="EXPLORE-pluck-beep")

    r = sub.add_parser("report", help="sustain, reach and the strongest line, per station and width")
    r.add_argument("--dir", default=EXPLORE_DIR)
    r.add_argument("--block", default=None, help="only this campaign_block")
    r.add_argument("--strikes-only", action="store_true", help="ignore no-fire and air-shot bundles")
    r.add_argument("--reference", type=float, default=40.0,
                   help="the width whose median impact peak defines reach (default 40)")

    a = p.parse_args()
    if a.mode == "sweep":
        run_sweep(a)
    elif a.mode == "pluck":
        run_pluck(a)
    else:
        run_report(a)


if __name__ == "__main__":
    main()
