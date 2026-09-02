"""Golden fixtures for the firmware acoustic DSP port (SPEC 14.2, 14.4).

Runs the research repository's OWN pure DSP modules (lib/dsp) on short excerpts of the
recorded campaign WAVs and records what they produce, so the C port is verified
against the validated Python implementation on identical inputs. The research repo is
read-only: this script imports it, never modifies it, and the fixtures are COPIED into
this repository (SPEC 14.2: do not reference them across repos).

Usage:
    python acoustic_prep/make_golden.py --acoustic-repo "../Acoustic Repo" --out-dir autonomous_truing_machine

Outputs (all committed):
    <out-dir>/test/fixtures/acoustic/<case>.pcm     int32 little-endian words, 24-bit sample in the
                                                    upper bits (sample24 << 8), exactly what the ESP-IDF
                                                    I2S driver delivers for 24-bit data in 32-bit slots
    acoustic_prep/golden/acoustic_golden.json       the reference outputs and every parameter used
    <out-dir>/lib/truing_fixtures/include/truing_fixtures/acoustic_golden.h   C initialisers
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import subprocess
import sys
import wave
from pathlib import Path

import numpy as np
import yaml

# Excerpt geometry: enough pre-roll for the onset detector to see silence, and enough post-roll for
# gate + window (dsp.yaml: 40 ms gate, 500 ms window, so 540 ms is the minimum and 600 ms leaves
# margin). The excerpts are compiled into the firmware as C arrays, so they are kept short.
PRE_MS = 200.0
POST_MS = 600.0

# (case name, campaign, file, event index within the file) -- one pluck each. The noise-floor
# case has no pluck: the port must report NO_ONSET_DETECTED, never a number.
CASES = [
    ("ts00_e2", "tension_sweep", "TS00_S01_free_medium.wav", 2),
    ("ts03_e2", "tension_sweep", "TS03_S01_free_medium.wav", 2),
    ("d3_e1", "tension_sweep", "D3_partner_free.wav", 1),
]
NOISE_CASE = ("noise", "tension_sweep", "noise_floor.wav", 1.0, 0.25)   # name, campaign, file, start s, length s

# Firmware fixture tension-model profile (lib/truing_fixtures/src/fixtures.c profile_base, gauge 2.0 mm):
# the golden model outputs are computed with the SAME constants so the C layer-4 port is checked
# on the identical closed forms. SYNTHETIC fixture content, not a validated calibration.
GAUGE_MM = 2.0
L_EFF_M = 0.1935
NOMINAL_SPAN_M = 0.1935
DENSITY = 7850.0
YOUNGS = 200.0e9
L_EFF_BOUNDS = (0.5, 1.5)          # research config/models.yaml m2.l_eff_bounds

# Absolute onset floor of the FIRMWARE fixture chain profile (dsp.yaml leaves it null because a campaign
# file always contains plucks; a device capture may contain nothing). -55 dBFS rms sits 14 dB above the
# recorded noise floor of this chain (-69 dBFS rms envelope) and BELOW every pluck's own relative
# threshold (0.0023-0.0058), so pluck detection is unchanged; recorded here so the reference onsets are
# computed with the same rule the firmware applies.
ONSET_THRESHOLD_ABS = 10.0 ** (-55.0 / 20.0)   # -55 dBFS rms = 0.0017783


def read_wav_int24(path: Path) -> tuple[np.ndarray, int]:
    with wave.open(str(path), "rb") as w:
        assert w.getsampwidth() == 3 and w.getnchannels() == 1, "expected mono 24-bit PCM"
        fs = w.getframerate()
        raw = np.frombuffer(w.readframes(w.getnframes()), dtype=np.uint8).reshape(-1, 3)
    s = raw[:, 0].astype(np.int32) | (raw[:, 1].astype(np.int32) << 8) | (raw[:, 2].astype(np.int32) << 16)
    s = np.where(s >= 1 << 23, s - (1 << 24), s)
    return s.astype(np.int32), fs


def to_float(sample24: np.ndarray) -> np.ndarray:
    return sample24.astype(np.float64) / float(1 << 23)


def words_from_int24(sample24: np.ndarray) -> np.ndarray:
    return (sample24.astype(np.int64) << 8).astype("<i4")


def model_outputs(f1: float, f2: float | None) -> dict:
    d = GAUGE_MM * 1e-3
    mu = DENSITY * math.pi * d * d / 4.0
    ei = YOUNGS * math.pi * d**4 / 64.0
    out = {"mu_kg_per_m": mu, "ei_n_m2": ei, "L_eff_m": L_EFF_M}
    out["m0_tension_n"] = 4.0 * mu * L_EFF_M**2 * f1**2
    corr = math.pi**2 * ei / L_EFF_M**2
    out["m1_stiffness_correction_n"] = corr
    out["m1_tension_n"] = out["m0_tension_n"] - corr
    if f2 is not None:
        b = ((f2 / 2.0) ** 2 - f1**2) / 3.0
        if b > 0:
            k1 = 4.0 * mu * b / math.pi**2
            l_eff = (ei / k1) ** 0.25
            a = f1**2 - b
            if a > 0:
                ratio = l_eff / NOMINAL_SPAN_M
                if L_EFF_BOUNDS[0] <= ratio <= L_EFF_BOUNDS[1]:
                    out["m2_tension_n"] = 4.0 * mu * a * l_eff**2
                    out["m2_l_eff_m"] = l_eff
                    out["m2_l_eff_ratio"] = ratio
                else:   # research lib/models/m2.py: l_eff_out_of_bounds
                    out["m2_rejected"] = "l_eff_out_of_bounds"
                    out["m2_l_eff_ratio_unrejected"] = ratio
        if "m2_tension_n" not in out and "m2_rejected" not in out:
            out["m2_rejected"] = "inversion_non_physical"
    else:
        out["m2_rejected"] = "f2_null"
    return out


def main(argv=None) -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--acoustic-repo", required=True)
    p.add_argument("--out-dir", required=True, help="the autonomous_truing_machine project directory")
    a = p.parse_args(argv)
    repo = Path(a.acoustic_repo).resolve()
    out_dir = Path(a.out_dir).resolve()
    sys.path.insert(0, str(repo))
    from lib.dsp.features import EventParams, compute_event_features   # noqa: E402
    from lib.dsp.onset import detect_onsets                           # noqa: E402
    from lib.dsp.peaks import find_spectral_peaks                     # noqa: E402
    from lib.dsp.spectrum import compute_spectrum                     # noqa: E402

    cfg = yaml.safe_load((repo / "config" / "dsp.yaml").read_text(encoding="utf-8"))
    params = EventParams.from_config(cfg)
    onset_cfg = cfg["onset"]
    try:
        commit = subprocess.check_output(["git", "-C", str(repo), "rev-parse", "HEAD"], text=True).strip()
    except Exception:  # noqa: BLE001
        commit = "unknown"

    fixtures_dir = out_dir / "test" / "fixtures" / "acoustic"
    fixtures_dir.mkdir(parents=True, exist_ok=True)
    golden_dir = Path(__file__).resolve().parent / "golden"
    golden_dir.mkdir(exist_ok=True)

    def onset_params():
        abs_thr = onset_cfg.get("threshold_abs")
        if abs_thr is None:
            abs_thr = ONSET_THRESHOLD_ABS
        return dict(frame_ms=float(onset_cfg["frame_ms"]), hop_ms=float(onset_cfg["hop_ms"]),
                    threshold_rel=float(onset_cfg["threshold_rel"]), threshold_abs=float(abs_thr),
                    refractory_s=float(onset_cfg["refractory_s"]))

    cases = []
    pcm_words: dict[str, np.ndarray] = {}
    for name, campaign, fname, ev in CASES:
        s24, fs = read_wav_int24(repo / "data" / campaign / "raw" / fname)
        assert fs == 48000
        x = to_float(s24)
        full = detect_onsets(x, fs, **onset_params())
        onset = int(full.onset_samples[ev])
        lo = onset - int(round(PRE_MS * 1e-3 * fs))
        hi = onset + int(round(POST_MS * 1e-3 * fs))
        exc24 = s24[lo:hi]
        xe = to_float(exc24)
        # The port sees ONLY the excerpt: re-detect the onset within it (file-local threshold).
        res = detect_onsets(xe, fs, **onset_params())
        assert res.n_onsets >= 1, name
        onset_e = int(res.onset_samples[0])
        feats = compute_event_features(xe, fs, onset_e, None, params)
        assert feats is not None, name
        # Candidate list with magnitudes (features.py reports only freq/prominence).
        start = feats["window_start_sample"]
        seg = xe[start:start + int(round(feats["window_len_ms"] * 1e-3 * fs))]
        spec = compute_spectrum(seg, fs, params.zero_pad_factor)
        peaks = find_spectral_peaks(spec, params.search_band_hz, params.prominence_db, params.max_peak_depth_db)
        ranked = sorted(peaks, key=lambda pk: (-pk.prominence_db, pk.freq_hz))[: params.max_peaks]
        reported = sorted(ranked, key=lambda pk: pk.freq_hz)
        f1_peak = next((pk for pk in peaks if params.f1_band_hz[0] <= pk.freq_hz <= params.f1_band_hz[1]), None)
        words = words_from_int24(exc24)
        pcm_words[name] = words
        pcm_path = fixtures_dir / f"{name}.pcm"
        pcm_path.write_bytes(words.tobytes())
        cases.append({
            "name": name, "source_file": fname, "campaign": campaign, "event_index_in_file": ev,
            "excerpt_start_sample_in_file": lo, "n_samples": int(words.size),
            "pcm_sha256": hashlib.sha256(words.tobytes()).hexdigest(),
            "onset_sample": onset_e, "onset_threshold": float(res.threshold), "n_onsets_in_excerpt": int(res.n_onsets),
            "window_start_sample": int(feats["window_start_sample"]), "window_len_ms": float(feats["window_len_ms"]),
            "window_n_samples": int(seg.size), "window_truncated_by": feats["window_truncated_by"], "n_fft": int(spec.n_fft),
            "f1_hz": float(feats["f1_hz"]), "f1_magnitude_db": float(f1_peak.magnitude_db) if f1_peak else None,
            "f1_prominence_db": float(feats["prominence_f1_db"]), "f1_bin_index": int(f1_peak.bin_index) if f1_peak else None,
            "f2_hz": feats["f2_hz"], "snr_f1_db": float(feats["snr_f1_db"]), "n_peaks_in_band": int(feats["n_peaks_in_band"]),
            "n_strong_peaks": len(peaks),
            "candidates": [{"freq_hz": pk.freq_hz, "prominence_db": pk.prominence_db, "magnitude_db": pk.magnitude_db,
                            "bin_index": pk.bin_index} for pk in reported],
            "models": model_outputs(float(feats["f1_hz"]), feats["f2_hz"]),
        })
        print(f"{name}: onset {onset_e} window {feats['window_len_ms']:.1f} ms ({feats['window_truncated_by']}) "
              f"f1 {feats['f1_hz']:.3f} Hz f2 {feats['f2_hz']} snr {feats['snr_f1_db']:.1f} dB peaks {len(peaks)} "
              f"in-band {feats['n_peaks_in_band']} M0 {cases[-1]['models']['m0_tension_n']:.1f} N")

    name, campaign, fname, start_s, len_s = NOISE_CASE
    s24, fs = read_wav_int24(repo / "data" / campaign / "raw" / fname)
    lo, hi = int(start_s * fs), int((start_s + len_s) * fs)
    exc24 = s24[lo:hi]
    res = detect_onsets(to_float(exc24), fs, **onset_params())
    words = words_from_int24(exc24)
    pcm_words[name] = words
    (fixtures_dir / f"{name}.pcm").write_bytes(words.tobytes())
    noise = {"name": name, "source_file": fname, "campaign": campaign, "excerpt_start_sample_in_file": lo,
             "n_samples": int(words.size), "pcm_sha256": hashlib.sha256(words.tobytes()).hexdigest(),
             "n_onsets_in_excerpt_reference": int(res.n_onsets),
             "note": "Recorded noise floor. The relative onset threshold always fires on SOMETHING in a file-local "
                     "detector; the firmware applies the chain profile's absolute floor as well and must report "
                     "NO_ONSET_DETECTED or LOW_SNR, never a tension.",
             "rms_dbfs": float(20.0 * math.log10(max(np.sqrt(np.mean(to_float(exc24) ** 2)), 1e-20)))}
    print(f"noise: reference detector fired {res.n_onsets} time(s); rms {noise['rms_dbfs']:.1f} dBFS")

    doc = {
        "acoustic_repo_commit": commit, "sample_rate_hz": 48000, "pcm_format": "int32 LE, sample24 << 8 (I2S 24-in-32)",
        "float_scale": "sample24 / 2^23",
        "dsp_params": {k: (list(v) if isinstance(v, tuple) else v) for k, v in params.__dict__.items()},
        "onset_params": onset_params(), "pre_ms": PRE_MS, "post_ms": POST_MS,
        "model_constants": {"gauge_mm": GAUGE_MM, "L_eff_m": L_EFF_M, "nominal_span_m": NOMINAL_SPAN_M,
                            "density_kg_m3": DENSITY, "youngs_modulus_pa": YOUNGS},
        "tolerances": {"f1_hz_abs": 0.05, "f2_hz_abs": 0.1, "snr_db_abs": 0.2, "magnitude_db_abs": 0.2,
                       "prominence_db_abs": 0.2, "note": "float32 FFT of 2^18 points vs float64; judged in the C tests"},
        "cases": cases, "noise": noise,
    }
    (golden_dir / "acoustic_golden.json").write_text(json.dumps(doc, indent=2), encoding="utf-8")

    # C header ---------------------------------------------------------------------------
    def fl(v):
        if v is None or (isinstance(v, float) and not math.isfinite(v)):
            return "NAN"
        t = f"{float(v):.9g}"
        if "." not in t and "e" not in t:
            t += ".0"
        return t + "f"

    def sym(case_name: str) -> str:
        return f"fixture_acoustic_{case_name}_pcm"

    h = ["/* GENERATED by acoustic_prep/make_golden.py from the research repository's lib/dsp at commit",
         f" * {commit}. Reference outputs on the recorded excerpts, whose samples are compiled in by",
         " * fixture_acoustic_pcm.c. Do not edit. */",
         "#include <math.h>", "#include <stddef.h>", "#include <stdint.h>", "",
         f"#define ACOUSTIC_GOLDEN_N_CASES {len(cases)}u", "#define ACOUSTIC_GOLDEN_MAX_CANDIDATES 8u", ""]
    for c in cases:
        h.append(f"extern const int32_t {sym(c['name'])}[];")
        h.append(f"extern const size_t {sym(c['name'])}_len;")
    h.append(f"extern const int32_t {sym(noise['name'])}[];")
    h.append(f"extern const size_t {sym(noise['name'])}_len;")
    h += ["",
          "typedef struct { float freq_hz, prominence_db, magnitude_db; int bin_index; } acoustic_golden_peak_t;",
          "typedef struct {", "    const char *name;", "    const int32_t *pcm;", "    uint32_t n_samples;",
          "    int onset_sample;", "    int window_start_sample;", "    int window_n_samples;",
          "    const char *truncated_by;", "    int n_fft;", "    float f1_hz, f1_magnitude_db, f1_prominence_db;",
          "    int f1_bin_index;", "    float f2_hz;              /* NAN when the reference found none */",
          "    float snr_f1_db;", "    int n_peaks_in_band;", "    int n_strong_peaks;", "    int n_candidates;",
          "    acoustic_golden_peak_t candidates[ACOUSTIC_GOLDEN_MAX_CANDIDATES];",
          "    float m0_tension_n, m1_tension_n, m2_tension_n;   /* m2 NAN when the reference rejected it */",
          "    float m2_l_eff_m;", "} acoustic_golden_case_t;", "",
          f"static const acoustic_golden_case_t acoustic_golden_cases[{len(cases)}] = {{"]
    for c in cases:
        cands = ", ".join("{" + f"{fl(k['freq_hz'])}, {fl(k['prominence_db'])}, {fl(k['magnitude_db'])}, {k['bin_index']}" + "}"
                          for k in c["candidates"])
        m = c["models"]
        h.append("  {" + f"\"{c['name']}\", {sym(c['name'])}, {c['n_samples']}u, {c['onset_sample']}, "
                 f"{c['window_start_sample']}, {c['window_n_samples']}, \"{c['window_truncated_by']}\", {c['n_fft']}, "
                 f"{fl(c['f1_hz'])}, {fl(c['f1_magnitude_db'])}, {fl(c['f1_prominence_db'])}, {c['f1_bin_index']}, "
                 f"{fl(c['f2_hz'])}, {fl(c['snr_f1_db'])}, {c['n_peaks_in_band']}, {c['n_strong_peaks']}, "
                 f"{len(c['candidates'])}, {{{cands}}}, {fl(m['m0_tension_n'])}, {fl(m['m1_tension_n'])}, "
                 f"{fl(m.get('m2_tension_n'))}, {fl(m.get('m2_l_eff_m'))}" + "},")
    h.append("};")
    h.append(f"#define ACOUSTIC_GOLDEN_NOISE_PCM {sym(noise['name'])}")
    h.append(f"#define ACOUSTIC_GOLDEN_NOISE_N_SAMPLES {noise['n_samples']}u")
    h.append(f"#define ACOUSTIC_GOLDEN_NOISE_RMS_DBFS {fl(noise['rms_dbfs'])}")
    hdr = out_dir / "lib" / "truing_fixtures" / "include" / "truing_fixtures" / "acoustic_golden.h"
    hdr.write_text("\n".join(h) + "\n", encoding="utf-8")

    # The samples themselves, as one compiled translation unit. Both the host tests and the
    # on-target bring-up read them from here, so there is exactly one copy of the numbers in the
    # firmware sources and no file I/O in any test.
    src = ["/* GENERATED by acoustic_prep/make_golden.py: recorded campaign excerpts as the int32 I2S",
           " * word stream the front end delivers (24-bit sample in the upper bits). The identical bytes",
           " * are also written as .pcm files under test/fixtures/acoustic, whose hashes the JSON records.",
           f" * Research repository commit {commit}. Do not edit. */",
           "#include <stddef.h>", "#include <stdint.h>", ""]
    for name, words in pcm_words.items():
        src.append(f"const size_t {sym(name)}_len = {words.size}u;")
        src.append(f"const int32_t {sym(name)}[{words.size}] = {{")
        vals = words.tolist()
        for i in range(0, len(vals), 12):
            src.append("    " + ",".join(str(v) for v in vals[i:i + 12]) + ",")
        src.append("};")
    pcm_c = out_dir / "lib" / "truing_fixtures" / "src" / "fixture_acoustic_pcm.c"
    pcm_c.write_text("\n".join(src) + "\n", encoding="utf-8")
    print("wrote", golden_dir / "acoustic_golden.json", hdr, pcm_c, fixtures_dir)
    return 0


if __name__ == "__main__":
    sys.exit(main())
