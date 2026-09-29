"""Numbers for the dual-mic workbench (tools/workbench.py). Pure numpy, no board, no plotting.

Everything here is exploratory description of captures, never a verdict: no function decides that a
configuration is good, and no threshold below is an acceptance criterion. The two numbers that come
from earlier evidence are marked where they are defined:

    LINE_TOL_HZ = 2.0      the plan's consistency band (SOLENOID_CAMPAIGN_PLAN.md vocabulary)
    SHARED_* (6 Hz, 10 dB, 80 %)   the 2026-09-21 wideband-look method that found the rig lines

Host analysis uses ONE fixed ring window per capture, placed after the excitation and the same for
both microphones, so spectra of different shots sit on the same frequency grid and local vs far
compare the same instants. The board's own analysis window is carried alongside for display only.
"""
import json
import os

import numpy as np

FS = 48000
FULL_SCALE = 2 ** 23
RING_START_S = 0.05          # after the impact / onset
RING_LEN_S = 0.50            # the board's window_ms, so host and board look at comparable spans
FALLBACK_START_S = 0.25      # a capture with no onset (a quiet control): somewhere in the middle
VIEW_LO_HZ, VIEW_HI_HZ = 100.0, 4000.0
LINE_LO_HZ, LINE_HI_HZ = 150.0, 2500.0
F1_BAND = (350.0, 600.0)     # the firmware chain profile's search band, for reference only
LINE_TOL_HZ = 2.0            # consistency band, pre-registered in the campaign plan
SHARED_TOL_HZ = 6.0          # 09-21 wideband look
SHARED_OVER_FLOOR_DB = 10.0  # 09-21 wideband look
SHARED_MIN_SHARE = 0.8       # 09-21 wideband look
HP_HZ = 200.0                # below this the INMP441 chain's drift dominates (explore_pulse_sweep.py)
ENV_N = 240                  # 5 ms envelope frames


def db(v):
    return 20.0 * np.log10(np.maximum(np.asarray(v, dtype=np.float64), 1e-12))


# ------------------------------------------------------------------ loading

def words_to_float(raw_words):
    """int32 capture words (sample24 << 8) -> float full scale 1.0, DC removed, as the DSP scales them."""
    w = np.asarray(raw_words, dtype="<i4")
    x = (w >> 8).astype(np.float64) / FULL_SCALE
    return x - x.mean()


def rail_hits(raw_words):
    s = np.asarray(raw_words, dtype="<i4") >> 8
    return int(np.count_nonzero((s >= FULL_SCALE - 1) | (s <= -FULL_SCALE)))


STUCK_FRACTION = 0.5         # a live INMP441's self-noise (~-87 dBFS, hundreds of LSB) never repeats a value this often


def mic_health(raw_words):
    """What one channel of a quiet capture says about the mic behind it.

    "Nonzero" is not "alive": a data line nobody drives reads one constant (0 when it sits low, -1 when
    it sits high), and pickup on the floating wire adds isolated glitches that can reach full scale and
    fake spectral lines. So the test is whether the samples scatter, not whether they are nonzero."""
    s = np.asarray(raw_words, dtype="<i4") >> 8
    if s.size == 0:
        return {"state": "NO DATA", "why": "empty capture", "rms_dbfs": None, "peak_dbfs": None, "rails": 0}
    vals, counts = np.unique(s, return_counts=True)
    i = int(counts.argmax())
    mode, mode_frac = int(vals[i]), float(counts[i]) / s.size
    x = words_to_float(raw_words)
    h = {"rms_dbfs": float(db(np.sqrt(np.mean(x * x)))), "peak_dbfs": float(db(np.abs(x).max())),
         "rails": rail_hits(raw_words), "mode": mode, "mode_frac": mode_frac}
    if mode_frac >= STUCK_FRACTION:
        level = {0: "0 (line low)", -1: "-1 (line high)"}.get(mode, str(mode))
        glitches = s.size - int(counts[i])
        h["state"] = "DEAD"
        h["why"] = "stuck at %s in %.0f%% of samples: nothing is driving the data line%s" % (
            level, 100 * mode_frac, ", %d glitch samples (pickup on a floating wire)" % glitches if glitches else "")
    elif h["rails"]:
        h["state"], h["why"] = "CLIPPING", "live, but hitting full scale %d times" % h["rails"]
    else:
        h["state"], h["why"] = "ALIVE", "live signal"
    return h


def load_bundle(json_path):
    """(meta, local_words, far_words_or_None) as raw int32 arrays."""
    with open(json_path, "r", encoding="utf-8") as f:
        meta = json.load(f)
    d = os.path.dirname(os.path.abspath(json_path))
    local = np.fromfile(os.path.join(d, meta["pcm"]), dtype="<i4")
    far = None
    if meta.get("far_pcm") and os.path.exists(os.path.join(d, meta["far_pcm"])):
        far = np.fromfile(os.path.join(d, meta["far_pcm"]), dtype="<i4")
    return meta, local, far


# ------------------------------------------------------------------ time domain

def highpass(x, fc=HP_HZ):
    X = np.fft.rfft(x)
    f = np.fft.rfftfreq(len(x), 1.0 / FS)
    X[f < fc] = 0
    return np.fft.irfft(X, len(x))


def envelope(x):
    y = highpass(x)
    n = len(y) // ENV_N * ENV_N
    fr = y[:n].reshape(-1, ENV_N)
    return np.sqrt((fr ** 2).mean(axis=1))


def impact(x):
    """(peak dBFS of the 5 ms envelope above 200 Hz, its time in s)."""
    e = envelope(x)
    i = int(e.argmax())
    return float(db(e[i])), i * ENV_N / float(FS)


def ring_start_sample(meta, x, excited=None):
    """Where the fixed ring window starts: after the board's onset if it found a real one, else after the
    loudest event when something was excited, else mid-capture (a quiet control has nothing to anchor on).
    An onset at sample 0 is the start of the capture, not an excitation, so it is not trusted. `excited`
    None infers it from the bundle (fired, or a workbench excitation tag)."""
    onset = meta.get("expect_onset_sample")
    if isinstance(onset, int) and meta.get("expect_onsets", 0) and 0 < onset < len(x):
        return onset + int(RING_START_S * FS)
    if excited is None:
        excited = meta.get("fired") is True or meta.get("wb_excitation") in ("pluck", "strike", "air")
    if excited:
        return int((impact(x)[1] + RING_START_S) * FS)
    return int(FALLBACK_START_S * FS)


def ring_window(meta, x, excited=None):
    n = int(RING_LEN_S * FS)
    a = ring_start_sample(meta, x, excited)
    a = max(0, min(a, len(x) - n))
    return a, a + n


# ------------------------------------------------------------------ spectra

def spectrum(x, a, b, pad=2):
    """(freqs, magnitude dBFS) of x[a:b], Hann windowed and zero padded; amplitude-corrected so a full
    scale sine reads about 0 dBFS."""
    seg = x[a:b]
    w = np.hanning(len(seg))
    n = 1
    while n < len(seg) * pad:
        n *= 2
    X = np.fft.rfft(seg * w, n)
    mag = np.abs(X) * 2.0 / w.sum()
    return np.fft.rfftfreq(n, 1.0 / FS), db(mag)


def band_level_db(freqs, P_db, lo, hi):
    """Power-summed level of a band, dB."""
    m = (freqs >= lo) & (freqs <= hi)
    if not m.any():
        return float("nan")
    return float(10.0 * np.log10(np.sum(10.0 ** (P_db[m] / 10.0)) + 1e-24))


def line_level_db(freqs, P_db, f, tol=LINE_TOL_HZ):
    """Strongest bin within +-tol of f, dB."""
    m = (freqs >= f - tol) & (freqs <= f + tol)
    return float(P_db[m].max()) if m.any() else float("nan")


def smooth_db(P_db, freqs, width_hz=60.0):
    w = max(3, int(width_hz / (freqs[1] - freqs[0])) | 1)
    return np.convolve(P_db, np.ones(w) / w, mode="same")


def top_lines(freqs, P_db, k=5, lo=LINE_LO_HZ, hi=LINE_HI_HZ, min_sep_hz=8.0, floor_db=None):
    """Up to k strongest narrow lines as [(freq, level dB, prominence dB, over-floor dB or None)].
    Prominence is over a 60 Hz running mean of the same spectrum; a floor spectrum on the same grid,
    when given, adds the level over it."""
    base = smooth_db(P_db, freqs)
    prom = P_db - base
    m = np.where((freqs >= lo) & (freqs <= hi))[0]
    if len(m) < 3:
        return []
    # local maxima only
    cand = [i for i in m[1:-1] if P_db[i] >= P_db[i - 1] and P_db[i] >= P_db[i + 1]]
    cand.sort(key=lambda i: prom[i], reverse=True)
    out = []
    for i in cand:
        if all(abs(freqs[i] - o[0]) >= min_sep_hz for o in out):
            over = float(P_db[i] - floor_db[i]) if floor_db is not None else None
            out.append((float(freqs[i]), float(P_db[i]), float(prom[i]), over))
            if len(out) == k:
                break
    return out


# ------------------------------------------------------------------ one shot

def shot_metrics(meta, local_words, far_words=None, floor_local=None, floor_far=None, focus_hz=None, excited=None):
    """Everything the workbench prints after a shot. Floors are (freqs, P_db) on the same grid, or None."""
    out = {}
    chans = {"local": local_words}
    if far_words is not None:
        chans["far"] = far_words
    x_local = words_to_float(local_words)
    a, b = ring_window(meta, x_local, excited)
    out["ring_window"] = (a, b)
    spectra = {}
    for name, words in chans.items():
        x = words_to_float(words)
        f, P = spectrum(x, a, b)
        spectra[name] = (f, P)
        fl = floor_local if name == "local" else floor_far
        fl_P = fl[1] if fl is not None and len(fl[1]) == len(P) else None
        peak_db, t_imp = impact(x)
        c = {
            "peak_dbfs": float(db(np.abs(x).max())),
            "rail_hits": rail_hits(words),
            "impact_dbfs": peak_db,
            "impact_s": t_imp,
            "in_band_db": band_level_db(f, P, *F1_BAND),
            "wide_db": band_level_db(f, P, LINE_LO_HZ, VIEW_HI_HZ),
            "lines": top_lines(f, P, floor_db=fl_P),
        }
        if fl_P is not None:
            c["in_band_over_floor_db"] = c["in_band_db"] - band_level_db(f, fl_P, *F1_BAND)
            c["wide_over_floor_db"] = c["wide_db"] - band_level_db(f, fl_P, LINE_LO_HZ, VIEW_HI_HZ)
        out[name] = c
    # focus: explicit, else the board's f1, else the strongest local line
    f_focus = focus_hz
    source = "set"
    if f_focus is None and isinstance(meta.get("expect_f1_hz"), (int, float)) and meta.get("expect_f1_hz") == meta.get("expect_f1_hz"):
        f_focus, source = float(meta["expect_f1_hz"]), "board_f1"
    if f_focus is None and out["local"]["lines"]:
        f_focus, source = out["local"]["lines"][0][0], "top_local_line"
    out["focus_hz"], out["focus_source"] = f_focus, source
    if "far" in spectra and f_focus is not None:
        fl, Pl = spectra["local"]
        ff, Pf = spectra["far"]
        out["delta_focus_db"] = line_level_db(fl, Pl, f_focus) - line_level_db(ff, Pf, f_focus)
    out["_spectra"] = spectra
    return out


# ------------------------------------------------------------------ localization

def delta_at(spectra, f):
    fl, Pl = spectra["local"]
    ff, Pf = spectra["far"]
    return line_level_db(fl, Pl, f) - line_level_db(ff, Pf, f)


def localization(spectra, focus_hz, rig_freqs=(), delta_floor_db=None):
    """Delta(f) = local - far at the focus line, and the same difference at this shot's rig lines.
    Index = Delta(f) - median Delta(rig) when rig lines are known, else Delta(f) - Delta_floor.
    Positive suggests energy excited near the local mic. No threshold is implied."""
    if "far" not in spectra or focus_hz is None:
        return None
    d = delta_at(spectra, focus_hz)
    rig = [delta_at(spectra, f) for f in rig_freqs if abs(f - focus_hz) > SHARED_TOL_HZ]
    rig = [v for v in rig if np.isfinite(v)]
    res = {"focus_hz": focus_hz, "delta_db": d, "delta_rig_db": None, "delta_floor_db": delta_floor_db,
           "n_rig": len(rig), "index_db": None, "reference": None}
    if rig:
        res["delta_rig_db"] = float(np.median(rig))
        res["index_db"], res["reference"] = d - res["delta_rig_db"], "rig"
    elif delta_floor_db is not None:
        res["index_db"], res["reference"] = d - delta_floor_db, "floor"
    return res


def delta_floor(control_spectra):
    """Median over controls of the median local-far bin difference in 150-2500 Hz: the mics' baseline
    offset (sensitivity + placement) with nothing excited."""
    vals = []
    for sp in control_spectra:
        if "far" not in sp:
            continue
        fl, Pl = sp["local"]
        _, Pf = sp["far"]
        m = (fl >= LINE_LO_HZ) & (fl <= LINE_HI_HZ)
        vals.append(float(np.median(Pl[m] - Pf[m])))
    return float(np.median(vals)) if vals else None


def floor_spectrum(spectra_list):
    """Per-bin median of a set of spectra on one grid: the session's room floor for one mic."""
    if not spectra_list:
        return None
    f = spectra_list[0][0]
    P = np.median(np.vstack([s[1] for s in spectra_list if len(s[1]) == len(f)]), axis=0)
    return f, P


# ------------------------------------------------------------------ across shots

def consistent_lines(spectra_list, floor, lo=VIEW_LO_HZ, hi=VIEW_HI_HZ,
                     over_db=SHARED_OVER_FLOOR_DB, share=SHARED_MIN_SHARE):
    """Lines of ONE spoke: bins that are local maxima of the median excess spectrum and stand >= over_db
    over the floor in >= share of the shots. Returns [(freq, median excess dB, fraction)]."""
    if not spectra_list or floor is None:
        return []
    f, Pf = floor
    ex = np.vstack([s[1] - Pf for s in spectra_list if len(s[1]) == len(Pf)])
    if ex.size == 0:
        return []
    med = np.median(ex, axis=0)
    # allow +-1 bin of jitter shot to shot
    hit = np.maximum.reduce([np.roll(ex >= over_db, k, axis=1) for k in (-1, 0, 1)])
    frac = hit.mean(axis=0)
    m = np.where((f >= lo) & (f <= hi))[0]
    out = []
    for i in m[1:-1]:
        if med[i] >= med[i - 1] and med[i] >= med[i + 1] and frac[i] >= share and med[i] >= over_db:
            if not out or f[i] - out[-1][0] >= SHARED_TOL_HZ:
                out.append((float(f[i]), float(med[i]), float(frac[i])))
            elif med[i] > out[-1][1]:
                out[-1] = (float(f[i]), float(med[i]), float(frac[i]))
    return out


def shared_lines(lines_by_spoke, min_spokes=2, tol=SHARED_TOL_HZ):
    """Cluster per-spoke consistent lines within tol; a cluster present on >= min_spokes different spokes is
    'rig' (the same line under different spokes). Returns (rig [(freq, spokes)], unique {spoke: [freq]})."""
    items = sorted((f, sp) for sp, ls in lines_by_spoke.items() for f, _, _ in ls)
    clusters = []
    for f, sp in items:
        if clusters and f - clusters[-1]["f"][-1] <= tol:
            clusters[-1]["f"].append(f)
            clusters[-1]["sp"].add(sp)
        else:
            clusters.append({"f": [f], "sp": {sp}})
    rig, unique = [], {sp: [] for sp in lines_by_spoke}
    for c in clusters:
        fc = float(np.median(c["f"]))
        if len(c["sp"]) >= min_spokes:
            rig.append((fc, sorted(c["sp"])))
        else:
            for sp in c["sp"]:
                unique[sp].append(fc)
    return rig, unique


def repeatability(freqs, tol=LINE_TOL_HZ):
    """Median and full spread of a set of line frequencies, and how many sit within +-tol of the median."""
    v = np.array([x for x in freqs if x is not None and np.isfinite(x)], dtype=float)
    if v.size == 0:
        return None
    med = float(np.median(v))
    return {"n": int(v.size), "median_hz": med, "spread_hz": float(v.max() - v.min()),
            "within_tol": int(np.count_nonzero(np.abs(v - med) <= tol)), "tol_hz": tol}


def nearest_line(lines, f, tol):
    best = None
    for ln in lines:
        if abs(ln[0] - f) <= tol and (best is None or abs(ln[0] - f) < abs(best[0] - f)):
            best = ln
    return best


PRESENCE_PROM_DB = 6.0       # the firmware chain profile's prominence_db: its own peak criterion


def line_presence(freqs, P_db, f0, tol=LINE_TOL_HZ, min_prom_db=PRESENCE_PROM_DB):
    """Is there a peak within +-tol of f0 standing min_prom_db over the 60 Hz running mean? Asked directly at
    f0, so a line is found even when stronger rig lines crowd it out of any top-k list.
    Returns (present, prominence dB of the best bin there, its frequency)."""
    m = np.where((freqs >= f0 - tol) & (freqs <= f0 + tol))[0]
    if len(m) == 0:
        return False, float("nan"), None
    prom = P_db - smooth_db(P_db, freqs)
    i = m[int(np.argmax(prom[m]))]
    is_peak = 0 < i < len(P_db) - 1 and P_db[i] >= P_db[i - 1] and P_db[i] >= P_db[i + 1]
    return bool(is_peak and prom[i] >= min_prom_db), float(prom[i]), float(freqs[i])


def match_lines(a_lines, b_lines, tol=LINE_TOL_HZ):
    """Pairs of lines from two captures that agree within tol: [(fa, fb)]."""
    out = []
    for la in a_lines:
        lb = nearest_line(b_lines, la[0], tol)
        if lb is not None:
            out.append((la[0], lb[0]))
    return out
