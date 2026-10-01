"""Frozen-rig characterization loop: one spoke clamped at one station, mechanics frozen, only pulse width,
recording choices and analysis vary. EXPLORATORY (docs/SOLENOID_CAMPAIGN.md): no held-out spoke set, not
campaign evidence. Plan: the "frozen-rig mic characterization" entry in that file.

    python tools/char_loop.py acquire --root R --tag E --pulses 30,40,50,60,70,85,100 --per-pulse 8 --nofire 20
    python tools/char_loop.py lines   --root R --tag E [--channel far]
    python tools/char_loop.py gonogo  --root R --tag E --ref-f1 406.2
    python tools/char_loop.py sweep   --root R --tag E --name it03 --variants v.txt \
                                      --hypothesis "..." --predict "..." [--channel far]
    python tools/char_loop.py score   --root R --tag E --csv R/iter/it03.local.csv
    python tools/char_loop.py wav     --root R --tag E
    python tools/char_loop.py log     --root R --text "decision: ..."
    python tools/char_loop.py --selftest

Discipline, enforced here rather than remembered:
  * `sweep` refuses to run without a hypothesis and a prediction, and writes both to LOG.md BEFORE the
    result exists. Every variant is an answer to a stated question, never one more row in a grid.
  * A variant may not lower measurement_min_snr_db below the fixture's (that buys clears, it does not
    find the spoke), and may not narrow the f1 band (that would aim the analysis at the answer).
  * `acquire` caps a batch at 100 shots and stops itself on a board reset, a DEAD mic, three failed shots
    in a row, or strikes that stop landing (level far below the batch's own strikes at that width).

Layout under --root:  <tag>/  bundles + index.txt + plan.json (one directory per acquisition)
                      ref/    the pluck-reference bundles       iter/  sweep CSVs + variant files
                      wav/    listening copies                   LOG.md the running log
"""
import argparse
import csv
import json
import os
import random
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "probe"))

import b30_verdict as bv                                    # noqa: E402
import campaign_runner as cr                                # noqa: E402
from capture_fetch import add_to_index, fetch_bundle, write_bundle   # noqa: E402

PIO = os.path.join(os.path.expanduser("~"), ".platformio", "penv", "Scripts", "pio.exe")
HOST = "192.168.4.1"
MAX_SHOTS = 100
BASE_MIN_SNR_DB = 12.0                 # fixtures.c: measurement_min_snr_db
BASE_F1_BAND = (350.0, 600.0)          # fixtures.c: f1_band_lo/hi_hz
SAME_LINE_HZ = 2.0                     # "the same f1": the tolerance B3.0/B3.2 use for a line
GONOGO_HZ = 5.0                        # Stage 0: a line this close to the reference f1
GONOGO_STRIKE_FRAC = 0.5               # ... in at least half the strikes at some width
GONOGO_NOFIRE_FRAC = 0.25              # ... and in at most a quarter of the no-fire controls
LANDING_DROP_DB = 15.0                 # a strike this far under its width's median peak did not land
SWEEP_ROW = re.compile(r"^[^,\s]+,(baseline|variant),")


def ts():
    return time.strftime("%Y-%m-%d %H:%M:%S")


def log_append(root, text):
    os.makedirs(root, exist_ok=True)
    with open(os.path.join(root, "LOG.md"), "a", encoding="utf-8") as f:
        f.write(text.rstrip() + "\n\n")


# ------------------------------------------------------------------ plan and acquisition

def build_plan(pulses, per_pulse, nofire, seed):
    """Every shot of one batch, shuffled with a stored seed: widths and controls interleaved, so drift over
    the batch lands on all of them alike."""
    shots = [{"kind": "strike", "pulse_ms": int(p)} for p in pulses for _ in range(per_pulse)]
    shots += [{"kind": "no_fire", "pulse_ms": 0} for _ in range(nofire)]
    if not shots:
        raise SystemExit("nothing to shoot")
    if len(shots) > MAX_SHOTS:
        raise SystemExit("%d shots in one batch; the cap is %d (split it, with a rest between)" % (len(shots), MAX_SHOTS))
    for p in pulses:
        if not 1 <= int(p) <= 1000:
            raise SystemExit("pulse %s ms is outside 1..1000" % p)
    random.Random(seed).shuffle(shots)
    for i, s in enumerate(shots):
        s["order"] = i
    return shots


def board_uptime(host, timeout=5.0):
    with urllib.request.urlopen("http://%s/id" % host, timeout=timeout) as r:
        return int(json.loads(r.read().decode("utf-8")).get("uptime_s", 0))


def landed(peaks_by_pulse, pulse, peak_dbfs):
    """False when this strike sits far under the earlier strikes at the same width (needs 3 to judge)."""
    prev = peaks_by_pulse.get(pulse, [])
    return len(prev) < 3 or peak_dbfs >= statistics.median(prev) - LANDING_DROP_DB


def acquire(args):
    import numpy as np
    import wb_analysis as wa
    out = os.path.join(args.root, args.tag)
    if os.path.exists(os.path.join(out, "plan.json")):
        raise SystemExit("%s already holds a batch: pick a new --tag (nothing is overwritten)" % out)
    pulses = [int(p) for p in args.pulses.split(",") if p.strip()] if args.pulses else []
    seed = args.seed if args.seed is not None else int(time.time())
    plan = build_plan(pulses, args.per_pulse, args.nofire, seed)
    os.makedirs(out, exist_ok=True)
    rev = subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=PROJECT, capture_output=True, text=True).stdout.strip()
    with open(os.path.join(out, "plan.json"), "w", encoding="utf-8") as f:
        json.dump({"tag": args.tag, "seed": seed, "spoke": args.spoke, "rig_id": args.rig_id, "tool_rev": rev,
                   "created": ts(), "shots": plan}, f, indent=1)
    minutes = len(plan) * 7.5 / 60
    print(">>> RECORDING NOW: %d shots, about %.0f min (keep the room quiet)" % (len(plan), minutes), flush=True)
    log_append(args.root, "## %s  acquire %s\n%d shots (%s ms x %d, %d no-fire), seed %d, spoke %d, %s, tool %s" % (
        ts(), args.tag, len(plan), args.pulses or "-", args.per_pulse, args.nofire, seed, args.spoke, args.rig_id, rev))
    ws = cr.WS(args.host, 80, "/ws")
    seq_counter = [int(time.time()) % 100000]
    up0 = board_uptime(args.host)
    fails, peaks_by_pulse, misses, kept = 0, {}, 0, 0
    stop = None
    t_last = time.time()
    for s in plan:
        no_fire = s["kind"] == "no_fire"
        try:
            seq_before = cr.current_seq(args.host, args.timeout)
            seq_counter[0] += 1
            ws.send_text({"cmd": "DEBUG", "code": cr.MEASURE_ONCE_CODE, "seq": seq_counter[0],
                          "arg": cr.measure_once_arg(args.spoke, no_fire, s["pulse_ms"])})
            gap = time.time() - t_last
            ack = cr.wait_for_ack(ws, seq_counter[0], args.timeout)
            t_last = time.time()
            if ack is None or ack.get("verdict") != "ACCEPT":
                raise RuntimeError("ack %s" % (ack and (ack.get("verdict"), ack.get("reason"))))
            meta, pcm, far = fetch_bundle(args.host, args.timeout, with_far=True)
            if int(meta.get("seq", -1)) == seq_before:
                raise RuntimeError("no new capture (is the station's PRESENT flag on?)")
        except (RuntimeError, OSError, urllib.error.URLError, SystemExit) as e:
            fails += 1
            print("!! shot %d (%s %s ms): %s" % (s["order"], s["kind"], s["pulse_ms"], e), flush=True)
            if fails >= 3:
                stop = "three failed shots in a row"
                break
            continue
        fails = 0
        if board_uptime(args.host) < up0:
            stop = "the board reset mid-batch"
            break
        health = {"local": wa.mic_health(np.frombuffer(pcm, dtype="<i4"))}
        if far:
            health["far"] = wa.mic_health(np.frombuffer(far, dtype="<i4"))
        dead = [ch for ch, h in health.items() if h["state"] == "DEAD"]
        name = "%s_o%02d_%s%s_seq%d" % (args.tag, s["order"], "nf" if no_fire else "p",
                                        "" if no_fire else s["pulse_ms"], int(meta.get("seq", 0)))
        meta = dict(meta, campaign_kind=s["kind"], campaign_station=cr.station_for_spoke(args.spoke),
                    campaign_rig_id=args.rig_id, char_tag=args.tag, char_order=s["order"],
                    char_pulse_ms=s["pulse_ms"], char_gap_s=round(gap, 2),
                    char_peak_dbfs=round(health["local"]["peak_dbfs"], 2))
        write_bundle(name, meta, pcm, None, out, far_pcm=far)
        add_to_index(name, out)
        kept += 1
        print("   %2d/%d %-8s %4s ms  %-9s %-22s peak %6.1f dBFS%s" % (
            kept, len(plan), s["kind"], s["pulse_ms"] or "-", meta.get("status"), meta.get("reason"),
            health["local"]["peak_dbfs"], "  DEAD: " + ",".join(dead) if dead else ""), flush=True)
        if dead:
            stop = "mic DEAD on %s" % ",".join(dead)
            break
        if not no_fire:
            pk = health["local"]["peak_dbfs"]
            misses = 0 if landed(peaks_by_pulse, s["pulse_ms"], pk) else misses + 1
            peaks_by_pulse.setdefault(s["pulse_ms"], []).append(pk)
            if misses >= 3:
                stop = "three strikes in a row far quieter than their width's median: the solenoid may not be landing"
                break
    ws.close()
    print(">>> DONE%s: %d of %d shots kept in %s" % ("  (STOPPED: %s)" % stop if stop else "", kept, len(plan), out),
          flush=True)
    log_append(args.root, "%s  acquire %s ended: %d/%d kept%s" % (ts(), args.tag, kept, len(plan),
                                                                 "; STOPPED: " + stop if stop else ""))
    return 1 if stop else 0


# ------------------------------------------------------------------ host replay (the firmware's own DSP)

def run_native_sweep(bundle_dir, variants=None, channel="local", mode=None):
    env = dict(os.environ, TRUING_SWEEP_DIR=os.path.abspath(bundle_dir))
    env.pop("TRUING_SWEEP_VARIANTS", None)
    env.pop("TRUING_SWEEP_MODE", None)
    env.pop("TRUING_SWEEP_CHANNEL", None)
    if variants:
        env["TRUING_SWEEP_VARIANTS"] = os.path.abspath(variants)
    if channel == "far":
        env["TRUING_SWEEP_CHANNEL"] = "far"
    if mode:
        env["TRUING_SWEEP_MODE"] = mode
    r = subprocess.run([PIO, "test", "-d", PROJECT, "-e", "native_sweep", "-v"], env=env, capture_output=True,
                       text=True, errors="replace")
    if "PASSED" not in r.stdout:
        sys.stderr.write(r.stdout[-3000:] + r.stderr[-2000:])
        raise SystemExit("native_sweep did not pass")
    return r.stdout


def check_variants(path):
    """The two rules a variant file may not break, checked before anything runs."""
    problems = []
    with open(path, encoding="utf-8") as f:
        for n, raw in enumerate(f, 1):
            line = raw.split("#", 1)[0].strip()
            if not line or ":" not in line:
                continue
            for part in line.split(":", 1)[1].split(","):
                if "=" not in part:
                    continue
                k, v = (x.strip() for x in part.split("=", 1))
                try:
                    v = float(v)
                except ValueError:
                    continue
                if k == "measurement_min_snr_db" and v < BASE_MIN_SNR_DB:
                    problems.append("line %d: lowers measurement_min_snr_db below %g (buys clears)" % (n, BASE_MIN_SNR_DB))
                if (k == "f1_band_lo_hz" and v > BASE_F1_BAND[0]) or (k == "f1_band_hi_hz" and v < BASE_F1_BAND[1]):
                    problems.append("line %d: narrows the f1 band (aims the analysis at the answer)" % n)
    return problems


def parse_sweep(text):
    """-> {label: {bundle: row}} from native_sweep stdout; label 'baseline' or the variant's name."""
    out = {}
    for line in text.splitlines():
        if not SWEEP_ROW.match(line):
            continue
        row = next(csv.reader([line]))
        if len(row) != 11:
            continue
        b, axis, value, status, reason, _ns, _ni, f1, _f2, snr, _c = row
        label = "baseline" if axis == "baseline" else value
        out.setdefault(label, {})[b] = {"status": status, "reason": reason,
                                        "f1": float(f1) if f1 else None, "snr": float(snr) if snr else None}
    return out


def is_clear(r):
    return r["status"] == "suspect" and r["f1"] is not None


def load_docs(bundle_dir):
    docs, _ = bv.load_bundles(bundle_dir)
    return docs


def score(docs, rows, ref_rows=None, nofire_lines=()):
    """One dict per label: the pre-stated metrics. Gates: no no-fire clears; the strike f1 is not a no-fire line."""
    res = {}
    for label, per in rows.items():
        nf = [per[b] for b, d in docs.items() if b in per and d.get("campaign_kind") == "no_fire"]
        by_pulse = {}
        for b, d in docs.items():
            if b in per and d.get("campaign_kind") == "strike":
                by_pulse.setdefault(int(d.get("char_pulse_ms") or d.get("pulse_ms") or 0), []).append(per[b])
        pulses = {}
        for p, rs in sorted(by_pulse.items()):
            f1s = [r["f1"] for r in rs if is_clear(r)]
            med = statistics.median(f1s) if f1s else None
            within = sum(1 for f in f1s if abs(f - med) <= SAME_LINE_HZ) if f1s else 0
            rig = med is not None and any(abs(med - l["freq_hz"]) <= SAME_LINE_HZ for l in nofire_lines)
            pulses[p] = {"n": len(rs), "clears": len(f1s), "within": within, "median_f1": med,
                         "spread": (max(f1s) - min(f1s)) if f1s else None, "rig_line": rig}
        ref = None
        if ref_rows and label in ref_rows:
            rf = [r["f1"] for r in ref_rows[label].values() if is_clear(r)]
            ref = {"n": len(ref_rows[label]), "clears": len(rf), "median_f1": statistics.median(rf) if rf else None}
        nf_clears = sum(1 for r in nf if is_clear(r))
        res[label] = {"nofire_n": len(nf), "nofire_clears": nf_clears, "pulses": pulses, "ref": ref,
                      "gate_ok": len(nf) > 0 and nf_clears == 0}   # no controls is not a clean result
    return res


def render_score(res):
    lines = []
    for label, r in res.items():
        ref = r["ref"]
        lines.append("%s   no-fire clears %d/%d%s   pluck-ref clears %s, median f1 %s" % (
            label, r["nofire_clears"], r["nofire_n"], "" if r["gate_ok"] else "  ** GATE FAILS **",
            "%d/%d" % (ref["clears"], ref["n"]) if ref else "-",
            "%.1f" % ref["median_f1"] if ref and ref["median_f1"] is not None else "-"))
        for p, s in r["pulses"].items():
            dist = ""
            if ref and ref["median_f1"] is not None and s["median_f1"] is not None:
                dist = "  (%+.1f Hz from pluck ref)" % (s["median_f1"] - ref["median_f1"])
            lines.append("    %4d ms  clears %2d/%-2d  within +-2 Hz %2d  median f1 %8s  spread %6s%s%s" % (
                p, s["clears"], s["n"], s["within"],
                "%.1f" % s["median_f1"] if s["median_f1"] is not None else "-",
                "%.1f" % s["spread"] if s["spread"] is not None else "-", dist,
                "  ** a no-fire line **" if s["rig_line"] else ""))
    return "\n".join(lines)


def nofire_coherent_lines(docs, lines_per, min_snr):
    caps = {b: [f for f, snr in lines_per[b]["peaks"] if snr >= min_snr]
            for b, d in docs.items() if d.get("campaign_kind") == "no_fire" and b in lines_per}
    return bv.coherent_lines(caps)


def gonogo(docs, lines_per, ref_f1, min_snr=None):
    """Stage 0: fraction of strikes (per width) and of no-fire controls whose strong in-band peaks include one
    within GONOGO_HZ of the reference f1. The board's clear/reject verdict is not used.

    min_snr (Amendment 1, 2026-10-01): count only peaks whose SNR reaches it, as B3.0's definition of a line
    does. Without it the test is degenerate: a noise control has ~128 strong peaks, so any +-5 Hz window
    catches one by chance (S0: 62% of controls "had" 406 Hz, at a median SNR of 0.1 dB)."""
    def hit(b):
        return any(abs(f - ref_f1) <= GONOGO_HZ and (min_snr is None or snr >= min_snr)
                   for f, snr in lines_per.get(b, {}).get("peaks", []))
    nf = [b for b, d in docs.items() if d.get("campaign_kind") == "no_fire" and b in lines_per]
    nf_frac = sum(hit(b) for b in nf) / len(nf) if nf else None
    per = {}
    for b, d in docs.items():
        if d.get("campaign_kind") == "strike" and b in lines_per:
            per.setdefault(int(d.get("char_pulse_ms") or 0), []).append(hit(b))
    widths = {p: sum(h) / len(h) for p, h in sorted(per.items())}
    go = nf_frac is not None and nf_frac <= GONOGO_NOFIRE_FRAC and any(
        f >= GONOGO_STRIKE_FRAC for f in widths.values())
    return {"go": go, "nofire_frac": nf_frac, "nofire_n": len(nf), "widths": widths}


# ------------------------------------------------------------------ subcommands

def cmd_lines(args):
    d = os.path.join(args.root, args.tag)
    text = run_native_sweep(d, channel=args.channel, mode="lines")
    os.makedirs(os.path.join(args.root, "iter"), exist_ok=True)
    path = os.path.join(args.root, "iter", "%s.lines.%s.csv" % (args.tag, args.channel))
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    print(path)


def read_lines(root, tag, channel):
    with open(os.path.join(root, "iter", "%s.lines.%s.csv" % (tag, channel)), encoding="utf-8") as f:
        return bv.parse_lines_csv(f.read())


def cmd_gonogo(args):
    docs = load_docs(os.path.join(args.root, args.tag))
    report = ["## %s  Stage 0 go/no-go on %s (reference f1 %.1f Hz, +-%.0f Hz)" % (ts(), args.tag, args.ref_f1, GONOGO_HZ)]
    verdicts = []
    for ch in ("local", "far"):
        try:
            settings, per = read_lines(args.root, args.tag, ch)
        except FileNotFoundError:
            report.append("%s: no lines CSV (run `lines --channel %s`)" % (ch, ch))
            continue
        floor = settings.get("min_snr", BASE_MIN_SNR_DB)
        g = gonogo(docs, per, args.ref_f1, min_snr=floor)
        verdicts.append(g["go"])
        report.append("%s mic (peaks >= %g dB SNR): no-fire with the line %s of %d; strikes by width: %s -> %s" % (
            ch, floor, "-" if g["nofire_frac"] is None else "%.0f%%" % (100 * g["nofire_frac"]), g["nofire_n"],
            ", ".join("%d ms %.0f%%" % (p, 100 * f) for p, f in g["widths"].items()), "GO" if g["go"] else "no-go"))
    report.append("**Stage 0: %s**" % ("GO" if any(verdicts) else "NO-GO"))
    text = "\n".join(report)
    print(text)
    log_append(args.root, text)


def cmd_sweep(args):
    if not (args.hypothesis or "").strip() or not (args.predict or "").strip():
        raise SystemExit("a sweep needs --hypothesis (why this variant) and --predict (what result would confirm "
                         "or refute it), written before it runs")
    problems = check_variants(args.variants)
    if problems:
        raise SystemExit("variant file refused:\n  " + "\n  ".join(problems))
    it = os.path.join(args.root, "iter")
    os.makedirs(it, exist_ok=True)
    kept_variants = os.path.join(it, "%s.variants.txt" % args.name)
    if os.path.exists(kept_variants):
        raise SystemExit("iteration %s already exists: pick a new --name" % args.name)
    shutil.copyfile(args.variants, kept_variants)
    with open(kept_variants, encoding="utf-8") as f:
        vtext = f.read().strip()
    log_append(args.root, "## %s  iteration %s  (%s mic, on %s)\n**Hypothesis:** %s\n**Prediction:** %s\n"
               "**Variants:**\n```\n%s\n```" % (ts(), args.name, args.channel, args.tag, args.hypothesis.strip(),
                                                args.predict.strip(), vtext))
    text = run_native_sweep(os.path.join(args.root, args.tag), args.variants, args.channel)
    csv_path = os.path.join(it, "%s.%s.csv" % (args.name, args.channel))
    with open(csv_path, "w", encoding="utf-8") as f:
        f.write(text)
    ref_rows = None
    ref_dir = os.path.join(args.root, "ref")
    if args.channel == "local" and os.path.exists(os.path.join(ref_dir, "index.txt")):
        ref_rows = parse_sweep(run_native_sweep(ref_dir, args.variants, "local"))
    rendered = render_and_log(args.root, args.tag, parse_sweep(text), ref_rows, args.channel)
    print(rendered)


def render_and_log(root, tag, rows, ref_rows, channel):
    docs = load_docs(os.path.join(root, tag))
    try:
        settings, per = read_lines(root, tag, channel)
        lines = nofire_coherent_lines(docs, per, settings.get("min_snr", BASE_MIN_SNR_DB))
    except FileNotFoundError:
        lines = []
    res = score(docs, rows, ref_rows, lines)
    text = render_score(res)
    log_append(root, "**Result** (no-fire coherent lines: %s):\n```\n%s\n```" % (
        ", ".join("%.1f Hz" % l["freq_hz"] for l in lines) or "none", text))
    return text


def cmd_score(args):
    with open(args.csv, encoding="utf-8") as f:
        rows = parse_sweep(f.read())
    print(render_score(score(load_docs(os.path.join(args.root, args.tag)), rows)))


def cmd_wav(args):
    import numpy as np
    import workbench as wb
    d = os.path.join(args.root, args.tag)
    out = os.path.join(args.root, "wav")
    os.makedirs(out, exist_ok=True)
    docs = load_docs(d)
    n = 0
    for b, doc in docs.items():
        kind = doc.get("campaign_kind")
        if kind == "no_fire":
            cat = "nofire"
        elif kind == "strike":
            cat = "strike_%s_%sms" % ("clear" if doc.get("status") == "suspect" else "fail", doc.get("char_pulse_ms"))
        else:
            cat = "ref"
        for ch, key in (("local", "pcm"), ("far", "far_pcm")):
            if not doc.get(key):
                continue
            words = np.fromfile(os.path.join(d, doc[key]), dtype="<i4")
            wb.write_listening_wav(os.path.join(out, "%s_%s_%s.wav" % (cat, b, ch)), words)
            n += 1
    print("%d listening copies in %s (peak-normalised: levels are not real)" % (n, out))


def cmd_log(args):
    log_append(args.root, "%s  %s" % (ts(), args.text))


# ------------------------------------------------------------------ selftest

def selftest():
    # plan: counts, determinism, cap
    p1 = build_plan([40, 60], 3, 2, seed=7)
    assert len(p1) == 8 and sum(s["kind"] == "no_fire" for s in p1) == 2
    assert [s["order"] for s in p1] == list(range(8))
    assert [(s["kind"], s["pulse_ms"]) for s in p1] == [(s["kind"], s["pulse_ms"]) for s in build_plan([40, 60], 3, 2, 7)]
    for bad in (([40], 101, 0), ([0], 1, 0), ([], 0, 0)):
        try:
            build_plan(bad[0], bad[1], bad[2], 1)
        except SystemExit:
            pass
        else:
            raise AssertionError("plan %r must be refused" % (bad,))
    # landing check
    assert landed({}, 40, -60.0) and landed({40: [-20.0, -21.0]}, 40, -60.0)
    assert not landed({40: [-20.0, -21.0, -19.0]}, 40, -40.0) and landed({40: [-20.0, -21.0, -19.0]}, 40, -30.0)
    # variant rules
    with tempfile.TemporaryDirectory() as t:
        v = os.path.join(t, "v.txt")
        with open(v, "w") as f:
            f.write("ok: gate_start_ms=200\nlow: measurement_min_snr_db=9\nnarrow: f1_band_lo_hz=390\n"
                    "wide: f1_band_hi_hz=800  # widening is fine\n")
        probs = check_variants(v)
        assert len(probs) == 2 and "line 2" in probs[0] and "line 3" in probs[1], probs
    # sweep parsing + scoring
    text = "\n".join([
        "sweep dir: x   bundles: 3",
        "bundle,axis,value,status,reason,n_strong_peaks,n_peaks_in_band,f1_hz,f2_hz,snr_db,clears_min_snr",
        "s1,baseline,0,suspect,PROVISIONAL_MODE_ID,5,1,406.1,812.0,15.0,1",
        "s2,baseline,0,suspect,PROVISIONAL_MODE_ID,5,1,406.3,812.0,16.0,1",
        "s3,baseline,0,suspect,PROVISIONAL_MODE_ID,5,1,431.0,,13.0,1",
        "n1,baseline,0,rejected,LOW_SNR,5,1,352.0,,5.0,0",
        "s1,variant,g200,rejected,LOW_SNR,5,1,406.1,,9.0,0",
        "n1,variant,g200,suspect,PROVISIONAL_MODE_ID,5,1,355.0,,13.0,1",
        "s1,gate_start_ms,100,suspect,x,1,1,1,,1,1",
        "sweep complete: 3 bundle(s)"])
    rows = parse_sweep(text)
    assert set(rows) == {"baseline", "g200"} and len(rows["baseline"]) == 4
    docs = {"s1": {"campaign_kind": "strike", "char_pulse_ms": 40}, "s2": {"campaign_kind": "strike", "char_pulse_ms": 40},
            "s3": {"campaign_kind": "strike", "char_pulse_ms": 40}, "n1": {"campaign_kind": "no_fire"}}
    res = score(docs, rows, nofire_lines=[{"freq_hz": 406.5}])
    b = res["baseline"]
    assert b["gate_ok"] and b["nofire_clears"] == 0
    assert b["pulses"][40]["clears"] == 3 and b["pulses"][40]["within"] == 2 and b["pulses"][40]["median_f1"] == 406.3
    assert b["pulses"][40]["rig_line"]                                 # 406.3 is within 2 Hz of a no-fire line
    assert not res["g200"]["gate_ok"] and res["g200"]["pulses"][40]["clears"] == 0
    assert "GATE FAILS" in render_score(res) and "a no-fire line" in render_score(res)
    assert not score({"s1": docs["s1"]}, {"baseline": {"s1": rows["baseline"]["s1"]}})["baseline"]["gate_ok"]
    # go/no-go
    per = {"s1": {"peaks": [(405.0, 9.0)]}, "s2": {"peaks": [(407.9, 3.0)]}, "s3": {"peaks": [(430.0, 20.0)]},
           "n1": {"peaks": [(352.0, 8.0)]}}
    g = gonogo(docs, per, 406.2)
    assert g["go"] and abs(g["widths"][40] - 2 / 3) < 1e-9 and g["nofire_frac"] == 0.0
    per["n1"] = {"peaks": [(406.0, 8.0)]}
    assert not gonogo(docs, per, 406.2)["go"]                          # the line is in the controls too
    assert gonogo(docs, per, 406.2, min_snr=8.5)["go"] is False         # s2's 3 dB peak no longer counts: 1/3 strikes
    per["n1"] = {"peaks": [(406.0, 0.5)]}
    assert gonogo(docs, per, 406.2, min_snr=2.0)["nofire_frac"] == 0.0  # a noise-level peak is not a line
    print("selftest ok")


def main():
    if "--selftest" in sys.argv:
        selftest()
        return 0
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    def common(sp, tag=True):
        sp.add_argument("--root", required=True, help="the characterization directory (under _explore/)")
        if tag:
            sp.add_argument("--tag", required=True, help="which acquisition (subdirectory of --root)")
        return sp

    a = common(sub.add_parser("acquire"))
    a.add_argument("--pulses", default="", help="comma list of strike widths in ms")
    a.add_argument("--per-pulse", type=int, default=0)
    a.add_argument("--nofire", type=int, default=0)
    a.add_argument("--spoke", type=int, default=0)
    a.add_argument("--rig-id", default="LEFT/rig-2")
    a.add_argument("--seed", type=int)
    a.add_argument("--host", default=HOST)
    a.add_argument("--timeout", type=float, default=20.0)
    ln = common(sub.add_parser("lines"))
    ln.add_argument("--channel", choices=("local", "far"), default="local")
    g = common(sub.add_parser("gonogo"))
    g.add_argument("--ref-f1", type=float, required=True)
    s = common(sub.add_parser("sweep"))
    s.add_argument("--name", required=True)
    s.add_argument("--variants", required=True)
    s.add_argument("--hypothesis")
    s.add_argument("--predict")
    s.add_argument("--channel", choices=("local", "far"), default="local")
    sc = common(sub.add_parser("score"))
    sc.add_argument("--csv", required=True)
    common(sub.add_parser("wav"))
    lg = common(sub.add_parser("log"), tag=False)
    lg.add_argument("--text", required=True)
    args = p.parse_args()
    return {"acquire": acquire, "lines": cmd_lines, "gonogo": cmd_gonogo, "sweep": cmd_sweep, "score": cmd_score,
            "wav": cmd_wav, "log": cmd_log}[args.cmd](args) or 0


if __name__ == "__main__":
    sys.exit(main())
