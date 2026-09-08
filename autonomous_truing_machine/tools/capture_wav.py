"""Turn acoustic capture bundles into WAV files you can listen to.

Every claim this project makes about a pluck so far has been a number read out of a JSON
file. The bundles already contain the audio, so nothing new has to be recorded to check
those numbers by ear:

    python tools/capture_wav.py nano_handpluck_provisional
    python tools/capture_wav.py test/fixtures/acoustic/captures/_campaign

Per bundle it writes two files into test/fixtures/acoustic/captures/_wav/ (gitignored):

    <name>.wav          the whole capture - the 1.2 s the front end delivered
    <name>.window.wav   ONLY the slice the verdict came from, cut at
                        expect_window_start_sample for expect_window_n_samples

The window file is the one that settles arguments. The board's status/reason came from
those samples and nothing else, so "the machine did not hear my pluck" and "the machine
heard it and rejected it" stop being the same sentence.

FIDELITY. The DSP multiplies each captured word by 2**-23 after an arithmetic shift
(truing_audio_word_to_float: (word >> 8) * (1.0f / 8388608.0f)). So 24-bit PCM of
`word >> 8` is byte-for-byte the sample the DSP analysed, and that is what is written -
unnormalised, so the levels printed here are the real ones and are comparable with the
research campaign's noise floor. --normalize writes SEPARATE .norm.wav copies for
listening on laptop speakers; it never overwrites the faithful file, because a normalised
capture cannot be used to reason about SNR.

Requires nothing outside the standard library, and no hardware.
"""
import argparse
import array
import json
import math
import os
import sys
import wave

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CAPTURES = os.path.join(REPO_ROOT, "test", "fixtures", "acoustic", "captures")
CAMPAIGN = os.path.join(CAPTURES, "_campaign")
DEFAULT_OUT = os.path.join(CAPTURES, "_wav")
SCHEMA = "truing.acoustic.capture/1"

# The capture words are I2S 24-in-32: the sample sits in the upper 24 bits, so `word >> 8`
# is the signed 24-bit value and 2**23 is its full scale. Matches audio_source_if.h.
SHIFT = 8
SAMPLE_WIDTH = 3
FULL_SCALE = 8388608.0
DISTINCT_CAP = 4096          # only exists to catch a dead data line


# ------------------------------------------------------------- bundle input --

def resolve(token):
    """A bundle name, a .json path, a bundle stem, or a directory of bundles."""
    if os.path.isdir(token):
        found = sorted(os.path.join(token, f) for f in os.listdir(token)
                       if f.endswith(".json") and not f.endswith(".observed.json"))
        if not found:
            raise SystemExit("%s contains no bundle .json files" % token)
        return found
    for candidate in (token, token + ".json",
                      os.path.join(CAPTURES, token + ".json"),
                      os.path.join(CAMPAIGN, token + ".json")):
        if os.path.isfile(candidate):
            return [candidate]
    raise SystemExit("no bundle called %r (looked in %s and %s)" % (token, CAPTURES, CAMPAIGN))


def load(meta_path):
    with open(meta_path, "r", encoding="utf-8") as fh:
        meta = json.load(fh)
    if meta.get("schema") != SCHEMA:
        raise SystemExit("%s speaks schema %r, this tool speaks %r"
                         % (meta_path, meta.get("schema"), SCHEMA))
    pcm_path = os.path.join(os.path.dirname(os.path.abspath(meta_path)),
                            meta.get("pcm") or (os.path.splitext(os.path.basename(meta_path))[0] + ".pcm"))
    if not os.path.isfile(pcm_path):
        raise SystemExit("%s names samples %s, which is not there" % (meta_path, pcm_path))
    with open(pcm_path, "rb") as fh:
        raw = fh.read()
    expected = int(meta["n_words"]) * 4
    if len(raw) != expected:
        raise SystemExit("%s: metadata says %d words (%d bytes), file holds %d bytes"
                         % (pcm_path, int(meta["n_words"]), expected, len(raw)))
    return meta, decode_words(raw)


def decode_words(raw):
    """int32 LE capture words -> signed 24-bit samples, exactly as the DSP shifts them."""
    words = array.array("i")
    if words.itemsize != 4:
        # Every platform this runs on has a 4-byte 'i'; refuse rather than write garbage.
        raise SystemExit("this platform's array('i') is %d bytes, not 4" % words.itemsize)
    words.frombytes(raw)
    if sys.byteorder == "big":
        words.byteswap()
    # Python's >> floors toward -inf on negatives, which is the arithmetic shift C performs.
    return [w >> SHIFT for w in words]


# ------------------------------------------------------------- WAV and levels --

def write_mono_pcm24_wav(path, samples, sample_rate):
    """Write signed 24-bit little-endian mono PCM. Returns the path."""
    parent = os.path.dirname(os.path.abspath(path))
    if parent:
        os.makedirs(parent, exist_ok=True)
    buf = bytearray()
    for value in samples:
        if value > 8388607:
            value = 8388607
        elif value < -8388608:
            value = -8388608
        buf += (value & 0xFFFFFF).to_bytes(3, "little")
    with wave.open(path, "wb") as out:
        out.setnchannels(1)
        out.setsampwidth(SAMPLE_WIDTH)
        out.setframerate(sample_rate)
        out.writeframes(bytes(buf))
    return path


def read_back(path):
    """Decode a written WAV to signed ints, so a claim of fidelity can be checked."""
    with wave.open(path, "rb") as src:
        if (src.getnchannels(), src.getsampwidth()) != (1, SAMPLE_WIDTH):
            raise SystemExit("%s is not mono 24-bit" % path)
        raw = src.readframes(src.getnframes())
    return [int.from_bytes(raw[i:i + 3], "little", signed=True)
            for i in range(0, len(raw), 3)]


def pcm_stats(samples):
    """Peak/RMS/DC and health flags, on the same scale the DSP sees."""
    count = len(samples)
    if count == 0:
        return {"n": 0, "peak": 0, "peak_dbfs": float("-inf"), "rms": 0.0,
                "rms_dbfs": float("-inf"), "dc": 0.0, "distinct": 0, "clipped": 0,
                "silent": True, "stuck": True}
    peak = 0
    total = 0
    total_sq = 0
    clipped = 0
    seen = set()
    for value in samples:
        magnitude = -value if value < 0 else value
        if magnitude > peak:
            peak = magnitude
        total += value
        total_sq += value * value
        if value >= 8388607 or value <= -8388608:
            clipped += 1
        if len(seen) < DISTINCT_CAP:
            seen.add(value)
    dc = total / float(count)
    rms = math.sqrt(total_sq / float(count))
    # The INMP441 carries a substantial DC/sub-audio offset (the golden recordings do not).
    # Plain RMS therefore overstates how much ACOUSTIC noise is present, while the FFT puts
    # DC in its own bin well below the 350-600 Hz f1 band. Report both and never conflate:
    # rms_ac is the number to compare against another chain's noise floor.
    var_ac = (total_sq / float(count)) - (dc * dc)
    rms_ac = math.sqrt(var_ac) if var_ac > 0.0 else 0.0
    return {"n": count, "peak": peak, "peak_dbfs": dbfs(peak), "rms": rms,
            "rms_dbfs": dbfs(rms), "rms_ac": rms_ac, "rms_ac_dbfs": dbfs(rms_ac),
            "dc": dc, "dc_dbfs": dbfs(abs(dc)), "distinct": len(seen),
            "clipped": clipped, "silent": peak == 0, "stuck": len(seen) <= 1}


def dbfs(magnitude):
    return float("-inf") if magnitude <= 0 else 20.0 * math.log10(magnitude / FULL_SCALE)


def format_levels(stats):
    def fmt(v):
        return "-inf" if v == float("-inf") else "%.1f" % v
    distinct = "%d+" % DISTINCT_CAP if stats["distinct"] >= DISTINCT_CAP else str(stats["distinct"])
    line = ("peak %s  rms %s  rms_ac %s  dc %s dBFS  distinct %s"
            % (fmt(stats["peak_dbfs"]), fmt(stats["rms_dbfs"]),
               fmt(stats["rms_ac_dbfs"]), fmt(stats["dc_dbfs"]), distinct))
    if stats["clipped"]:
        line += "  CLIPPED %d" % stats["clipped"]
    if stats["stuck"]:
        line += "  STUCK (dead data line?)"
    elif stats["silent"]:
        line += "  SILENT"
    return line


def normalized(samples, headroom_db=1.0):
    """Scaled so the peak sits headroom_db below full scale. Returns (samples, gain_db)."""
    peak = max((abs(v) for v in samples), default=0)
    if peak == 0:
        return list(samples), 0.0
    target = FULL_SCALE * (10.0 ** (-abs(headroom_db) / 20.0))
    gain = target / float(peak)
    return [int(round(v * gain)) for v in samples], 20.0 * math.log10(gain)


# ------------------------------------------------------------------ reporting --

def verdict_line(meta):
    """What the board concluded from these exact samples."""
    def num(key, fmt="%.1f"):
        v = meta.get(key)
        return (fmt % v) if isinstance(v, (int, float)) else "-"
    # Board bundles carry status/reason; the golden fixture carries only the expect_* pair.
    status = meta.get("status") or meta.get("expect_status") or "?"
    reason = meta.get("reason") or meta.get("expect_reason") or "?"
    return ("%s / %s   f1 %s Hz   snr %s dB   onset sample %s   peaks %s strong / %s in band"
            % (status, reason,
               num("expect_f1_hz"), num("expect_snr_db"),
               meta.get("expect_onset_sample", "-"),
               meta.get("expect_n_strong_peaks", "-"),
               meta.get("expect_n_peaks_in_band", "-")))


def preroll_samples(meta):
    """How much of the buffer precedes the excitation window, derived from metadata.

    No field reports how many pre-trigger words the ring actually delivered, so this is the
    configured length, not a measurement -- see the note printed with it.
    """
    fs = float(meta.get("sample_rate_hz") or 48000)
    capture_us = meta.get("capture_us")
    if not isinstance(capture_us, (int, float)):
        return None
    return int(meta["n_words"]) - int(round(capture_us / 1e6 * fs))


def window_slice(meta, samples):
    """(start, count) of the analysed slice, clamped to what the buffer holds."""
    start = meta.get("expect_window_start_sample")
    count = meta.get("expect_window_n_samples")
    if not isinstance(start, int) or not isinstance(count, int) or count <= 0:
        return None
    start = max(0, start)
    count = min(count, len(samples) - start)
    return (start, count) if count > 0 else None


# ----------------------------------------------------------------------- main --

def export(meta_path, out_dir, do_normalize, window_only, full_only, verify=False):
    meta, samples = load(meta_path)
    name = meta.get("name") or os.path.splitext(os.path.basename(meta_path))[0]
    fs = int(meta.get("sample_rate_hz") or 48000)

    print("%s" % name)
    print("  %s" % verdict_line(meta))

    pre = preroll_samples(meta)
    if pre is not None and pre > 0:
        print("  pre-roll: samples 0..%d are ASSUMED ambient (pre_trigger_ms). No metadata "
              "field reports how many pre-trigger words the ring really delivered." % (pre - 1))
        print("    ambient  %s" % format_levels(pcm_stats(samples[:pre])))

    written = []
    if not window_only:
        path = os.path.join(out_dir, name + ".wav")
        write_mono_pcm24_wav(path, samples, fs)
        print("    full     %s" % format_levels(pcm_stats(samples)))
        written.append((path, samples))

    if not full_only:
        cut = window_slice(meta, samples)
        if cut is None:
            print("    window   no expect_window_* in this bundle - nothing analysed, or an "
                  "older bundle; the full capture is still written")
        else:
            start, count = cut
            slice_ = samples[start:start + count]
            path = os.path.join(out_dir, name + ".window.wav")
            write_mono_pcm24_wav(path, slice_, fs)
            print("    window   %s" % format_levels(pcm_stats(slice_)))
            print("             samples %d..%d (%.0f ms), truncated by %s"
                  % (start, start + count - 1, 1000.0 * count / fs,
                     meta.get("expect_window_truncated_by", "?")))
            written.append((path, slice_))

    if verify:
        for path, data in written:
            if read_back(path) != data:
                raise SystemExit("%s does not decode back to the samples it was written from"
                                 % path)
        print("    verified %d file(s) decode back to (word >> 8) exactly" % len(written))

    if do_normalize:
        for path, data in written:
            norm, gain_db = normalized(data)
            npath = path[:-len(".wav")] + ".norm.wav"
            write_mono_pcm24_wav(npath, norm, fs)
            print("    +%s  (%+.1f dB gain - LISTENING COPY, levels are not real)"
                  % (os.path.basename(npath), gain_db))

    return len(written)


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Export acoustic capture bundles as listenable WAV files.",
        epilog="A bundle argument may be a name, a .json path, a bundle stem, or a directory.")
    ap.add_argument("bundles", nargs="+", help="bundle names, paths, or directories")
    ap.add_argument("--out", default=DEFAULT_OUT, help="output directory (default: %(default)s)")
    ap.add_argument("--normalize", action="store_true",
                    help="also write .norm.wav listening copies (levels are not real)")
    ap.add_argument("--verify", action="store_true",
                    help="read each WAV back and prove it decodes to (word >> 8) exactly")
    group = ap.add_mutually_exclusive_group()
    group.add_argument("--window-only", action="store_true", help="write only the analysed slice")
    group.add_argument("--full-only", action="store_true", help="write only the whole capture")
    args = ap.parse_args(argv)

    paths = []
    for token in args.bundles:
        for path in resolve(token):
            if path not in paths:
                paths.append(path)

    os.makedirs(args.out, exist_ok=True)
    total = 0
    for path in paths:
        total += export(path, args.out, args.normalize, args.window_only, args.full_only,
                        args.verify)
    print("\n%d file(s) from %d bundle(s) -> %s" % (total, len(paths), args.out))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
