"""Checks the dual-mic workbench's numbers on synthetic captures with known answers, no board.

    python tools/wb_analysis_selftest.py

A planted spoke tone that is 20 dB louder at the local mic than at the far one, beside a planted rig
tone that is only 6 dB louder, must come out as a +14 dB localization index; a line common to two
spokes must be called rig and a line on one spoke unique; the far-mic fetch must refuse a capture whose
seq moved; and a listening WAV must resolve back to the bundle it was exported from.
"""
import json
import os
import sys
import tempfile

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import capture_fetch as cf      # noqa: E402
import wb_analysis as wa        # noqa: E402
import workbench as wb          # noqa: E402

FS = wa.FS
N = 57600
rng = np.random.default_rng(20260927)


def words(x):
    return (np.round(np.clip(x, -1, 1 - 2 ** -23) * 2 ** 23).astype(np.int64) << 8).astype("<i4")


def tone(f, amp, t0=0.2, tau=0.4):
    t = np.arange(N) / FS
    env = np.where(t >= t0, np.exp(-(t - t0) / tau), 0.0)
    return amp * env * np.sin(2 * np.pi * f * t)


def noise(level=1e-4):
    return rng.normal(0.0, level, N)


META = {"fired": True, "expect_onsets": 1, "expect_onset_sample": int(0.2 * FS)}


def check_line_level():
    x = wa.words_to_float(words(0.1 * np.sin(2 * np.pi * 406.0 * np.arange(N) / FS)))
    f, P = wa.spectrum(x, 12000, 36000)
    lvl = wa.line_level_db(f, P, 406.0)
    assert abs(lvl - (-20.0)) < 1.0, lvl
    assert abs(wa.top_lines(f, P, k=1)[0][0] - 406.0) < 1.0


def check_localization():
    local = words(tone(406.0, 0.10) + tone(1490.0, 0.05) + noise())
    far = words(tone(406.0, 0.01) + tone(1490.0, 0.025) + noise())
    m = wa.shot_metrics(META, local, far, focus_hz=406.0)
    L = wa.localization(m["_spectra"], 406.0, rig_freqs=[1490.0])
    assert abs(L["delta_db"] - 20.0) < 1.0, L
    assert abs(L["delta_rig_db"] - 6.0) < 1.0, L
    assert abs(L["index_db"] - 14.0) < 1.5, L
    assert L["reference"] == "rig"
    # no rig lines known: falls back to the controls' offset
    L2 = wa.localization(m["_spectra"], 406.0, rig_freqs=[], delta_floor_db=0.0)
    assert L2["reference"] == "floor" and abs(L2["index_db"] - 20.0) < 1.0
    assert wa.localization({"local": m["_spectra"]["local"]}, 406.0) is None   # single-mic capture


def check_shared_lines():
    ctrl = [wa.shot_metrics({}, words(noise()), None)["_spectra"]["local"] for _ in range(3)]
    floor = wa.floor_spectrum(ctrl)
    sp_a = [wa.shot_metrics(META, words(tone(700.0, 0.05) + tone(406.0, 0.05) + noise()), None)["_spectra"]["local"]
            for _ in range(4)]
    sp_b = [wa.shot_metrics(META, words(tone(700.0, 0.05) + tone(452.0, 0.05) + noise()), None)["_spectra"]["local"]
            for _ in range(4)]
    lines = {0: wa.consistent_lines(sp_a, floor), 2: wa.consistent_lines(sp_b, floor)}
    rig, unique = wa.shared_lines(lines)
    assert any(abs(f - 700.0) < 3 for f, _ in rig), rig
    assert any(abs(f - 406.0) < 3 for f in unique[0]), unique
    assert any(abs(f - 452.0) < 3 for f in unique[2]), unique
    assert not any(abs(f - 406.0) < 3 for f, _ in rig)


def check_line_presence():
    # a weak 406 Hz line among six much stronger lines: out of any top-5 list, but present
    loud = sum(tone(f, 0.05) for f in (700.0, 910.0, 1108.0, 1490.0, 1890.0, 2365.0))
    x = words(loud + tone(406.0, 0.002) + noise(1e-5))
    f, P = wa.shot_metrics(META, x, None)["_spectra"]["local"]
    assert not any(abs(ln[0] - 406.0) < 2 for ln in wa.top_lines(f, P, k=5))
    ok, prom, fq = wa.line_presence(f, P, 406.0)
    assert ok and abs(fq - 406.0) < 1.0, (ok, prom, fq)
    ok2, _, _ = wa.line_presence(f, P, 520.0)
    assert not ok2


def check_repeatability():
    r = wa.repeatability([406.1, 406.3, 406.2, 406.2, 397.6])
    assert r["n"] == 5 and r["within_tol"] == 4 and abs(r["median_hz"] - 406.2) < 1e-9


def check_far_fetch():
    meta = {"schema": "truing.acoustic.capture/2", "n_words": 4, "far_n_words": 4, "seq": 7}
    pcm, far = b"\x00" * 16, b"\x01" * 16
    responses = {"/debug/capture.json": (json.dumps(meta).encode(), "7"), "/debug/capture.pcm": (pcm, "7"),
                 "/debug/capture_far.pcm": (far, "7")}
    real = cf.get
    cf.get = lambda url, timeout: responses[url[url.index("/debug"):]]
    try:
        m, p, fa = cf.fetch_bundle("x", 1, with_far=True)
        assert p == pcm and fa == far
        m2, p2 = cf.fetch_bundle("x", 1)               # old callers: unchanged shape, no far fetch
        assert p2 == pcm
        responses["/debug/capture_far.pcm"] = (far, "8")
        try:
            cf.fetch_bundle("x", 1, with_far=True)
        except SystemExit:
            pass
        else:
            raise AssertionError("a far channel from another capture must be refused")
    finally:
        cf.get = real
    with tempfile.TemporaryDirectory() as d:
        doc = cf.write_bundle("b", dict(meta), pcm, None, d, far_pcm=far)
        assert os.path.exists(os.path.join(d, "b.far.pcm")) and doc["far_pcm"] == "b.far.pcm"
        _, lw, fw = wa.load_bundle(os.path.join(d, "b.json"))
        assert fw is not None and len(fw) == 4 and len(lw) == 4


def check_reference_resolution():
    name = "EXPLORE-pluck-lead3_sp0_r2_seq66"
    bundle = os.path.join(wb.CAPTURES, "_explore", name + ".json")
    if not os.path.exists(bundle):
        print("  (skipped reference resolution: %s not on this machine)" % name)
        return
    wav = os.path.join(wb.CAPTURES, "_wav", "pluck_lead3_spoke0_0921", name + ".norm.wav")
    assert os.path.normcase(wb.resolve_reference(wav)) == os.path.normcase(os.path.abspath(bundle))
    try:
        wb.resolve_reference(r"C:\nowhere\orphan.norm.wav")
    except ValueError:
        pass
    else:
        raise AssertionError("a WAV with no source bundle must be refused, not analysed")


def main():
    for fn in (check_line_level, check_localization, check_shared_lines, check_line_presence, check_repeatability,
               check_far_fetch, check_reference_resolution):
        fn()
        print("  ok  %s" % fn.__name__)
    print("selftest ok")


if __name__ == "__main__":
    main()
