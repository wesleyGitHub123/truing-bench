"""The committed fixtures and the committed golden values must agree: re-run the reference
pipeline on the .pcm excerpts and compare. Guards against editing one without the other."""
import hashlib
import json
import sys
from pathlib import Path

import numpy as np
import pytest

ROOT = Path(__file__).resolve().parents[2]
GOLDEN = ROOT / "acoustic_prep" / "golden" / "acoustic_golden.json"
FIXTURES = ROOT / "autonomous_truing_machine" / "test" / "fixtures" / "acoustic"
ACOUSTIC_REPO = ROOT.parent / "Acoustic Repo"

pytestmark = pytest.mark.skipif(not (ACOUSTIC_REPO / "lib" / "dsp").exists(), reason="research repository not present")


@pytest.fixture(scope="module")
def doc():
    return json.loads(GOLDEN.read_text(encoding="utf-8"))


def _load(name):
    words = np.fromfile(FIXTURES / f"{name}.pcm", dtype="<i4")
    return words, (words >> 8).astype(np.float64) / float(1 << 23)


def test_pcm_hashes_match_the_golden_record(doc):
    for c in doc["cases"] + [doc["noise"]]:
        words, _ = _load(c["name"])
        assert words.size == c["n_samples"]
        assert hashlib.sha256(words.tobytes()).hexdigest() == c["pcm_sha256"]


def test_reference_pipeline_reproduces_the_golden_values(doc):
    sys.path.insert(0, str(ACOUSTIC_REPO))
    from lib.dsp.features import EventParams, compute_event_features
    from lib.dsp.onset import detect_onsets

    p = dict(doc["dsp_params"])
    for k in ("search_band_hz", "f1_band_hz", "f2_ratio_range", "modulation_band_hz", "snr_noise_offset_hz"):
        p[k] = tuple(p[k])
    params = EventParams(**p)
    op = doc["onset_params"]
    for c in doc["cases"]:
        _, x = _load(c["name"])
        res = detect_onsets(x, 48000, **op)
        assert int(res.onset_samples[0]) == c["onset_sample"]
        feats = compute_event_features(x, 48000, int(res.onset_samples[0]), None, params)
        assert feats["window_start_sample"] == c["window_start_sample"]
        assert feats["window_truncated_by"] == c["window_truncated_by"]
        assert abs(feats["f1_hz"] - c["f1_hz"]) < 1e-9
        assert abs(feats["snr_f1_db"] - c["snr_f1_db"]) < 1e-9
        assert feats["n_peaks_in_band"] == c["n_peaks_in_band"]
    _, noise = _load(doc["noise"]["name"])
    assert detect_onsets(noise, 48000, **op).n_onsets == doc["noise"]["n_onsets_in_excerpt_reference"] == 0
