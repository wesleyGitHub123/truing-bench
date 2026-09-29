"""Dual-mic acoustic characterization workbench: experiment -> observe -> analyze -> adjust.

EXPLORATORY. Not a campaign: no plan, no trial numbers, no fixed n, no exclusion ledger. Captures go to
test/fixtures/acoustic/captures/_explore/wb_<date>_<session>/ (gitignored). Nothing here supports a claim
without a later registered run. Operator manual: tools/WORKBENCH.md.

    python tools/workbench.py --session micpos1            # against the board (join its AP first)
    python tools/workbench.py --session micpos1 --offline  # analysis, references and report only
    python tools/workbench.py --heartbeat                  # live check of both mics, nothing saved

Image: nano_esp32_fastdemo_mic_dualmic (both mics). The single-mic campaign image also works; shots then
have no far channel and localization is unavailable.

Vocabulary
    shot    one MEASURE_ONCE: strike (fire), no-fire control (ctrl), air shot (air) or cued hand pluck (pluck)
    epoch   one physical set-up plus one firmware identity (build + chain digest). `set` of any physical
            key opens a new epoch; so does a reflash. Spoke, pulse and excitation are per-shot variables.
    local   the struck station's own mic -- the only channel the board's DSP analysed
    far     the other station's mic over the same frames -- never analysed on the board
"""
import argparse
import cmd
import csv
import glob
import json
import os
import shlex
import sys
import threading
import time
import urllib.error
import warnings
import wave

import numpy as np

# a dead channel is all zeros, and matplotlib's spectrogram takes log10 of it
warnings.filterwarnings("ignore", message="divide by zero encountered in log10", category=RuntimeWarning)

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import campaign_runner as cr                     # noqa: E402  WS, arg packing, ack wait, station convention
import capture_fetch as cf                       # noqa: E402  seq-guarded fetch, flat bundle writer
import capture_wav                               # noqa: E402  faithful 24-bit WAV export
import wb_analysis as wa                         # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CAPTURES = os.path.join(REPO, "test", "fixtures", "acoustic", "captures")
EXPLORE = os.path.join(CAPTURES, "_explore")
NOTE = ("EXPLORATORY workbench capture (tools/workbench.py), not campaign data: no plan, no trial numbers, "
        "no exclusion ledger. Nothing here supports a claim without a fresh registered run.")
# Mirror of the board profile's BOARD_I2S_MIC_INPUT_*_STATION: which slot each station's own mic answers in.
EXPECTED_MIC_INPUT = {"LEFT": "left_slot", "RIGHT": "right_slot"}
# B2-M3 station brackets (SOLENOID_CAMPAIGN.md), measured on the ORIGINAL (Nano) rig's mount and
# standoff. Not re-verified on the new DevKit rig -- a different mount/standoff/spring makes a
# different reach and dwell. Kept only as a starting point for a fresh sweep; widths outside them
# are allowed, but said, and the note below flags them as unverified for this rig.
BRACKET_MS = {"LEFT": (40, 85), "RIGHT": (60, 95)}
REF_PRESETS = {
    # the 09-21 hand plucks on spoke 0 that cleared on the unchanged DSP at 406.2 Hz
    "spoke0-pluck": ["EXPLORE-pluck-lead3_sp0_r1_seq65", "EXPLORE-pluck-lead3_sp0_r2_seq66",
                     "EXPLORE-pluck-lead3_sp0_r5_seq69", "EXPLORE-pluck-lead3_sp0_r9_seq73"],
}
SHOT_KEYS = {"spoke": 0, "pulse": 0, "reps": 1, "interval": 1.0, "lead": 0.5, "focus": None,
             "autoview": True, "host": cr.DEFAULT_HOST, "timeout": 20.0}
KINDS = ("strike", "ctrl", "air", "pluck")
GUIDE = """
THE LOOP
  1 experiment  set spoke / pulse / reps ; set <physical key> <value>  (mic.dist, mic.coupling, mic.mount,
                strike.point, standoff, hold, damping ... any key not listed by `knobs` is physical and opens
                a new epoch) ; then fire | ctrl | air | pluck
  2 observe     printed per shot: both mics' peak / rail hits / impact / in-band and wide level over this
                epoch's floor / top lines ; the board verdict on the local mic ; delta = local - far at the
                focus line. A PNG opens (autoview). play local | far | ref
  3 analyze     compare, repeat, lines, loc, board
  4 adjust      decide keep|reject|ambiguous <why> ; note <text> ; change ONE thing ; repeat ; report

SETUP EACH EPOCH   ctrl 3 (room floor for both mics; the floor is per epoch because moving a mic moves it)
TAP TEST           ctrl while someone taps the LEFT mic: the LEFT station's channel must spike; then RIGHT
ROLLER (lab image) roll on | roll <+/-steps> | roll stop | roll off. Energising the coils opens a new epoch (holding
                   current is a noise source beside the mics), so `ctrl 3` with the roller on and off gives two
                   floors. A capture is refused while the roller moves. Every saved shot records the roller state.

READING GUIDES (not thresholds)
  promising  a local line repeating within +-2 Hz across a spoke's strikes; NOT in the rig list; moves when
             the spoke changes; localization index clearly above the rig lines'; stays put across pulse width;
             agrees with the reference (spoke 0 hand pluck: ~406 Hz) ; board clears on it ; controls clean
  bad        strongest lines identical across spokes (rig) ; delta no better than at rig lines ; energy only
             in the impact click ; a line that moves when ONLY the mic mount moves (mount ringing) ; a line
             that moves with pulse width ; rail hits on the local mic ; any clear on ctrl / air
  benchmarks old single mic: hand pluck SNR 16-24 dB at 406.2 Hz (spoke 0) ; B3.2 strikes median SNR 4.0 dB

WHAT MAY CHANGE HERE   physical set-up, spoke, pulse, excitation: freely (recorded). DSP constants: NOT on the
  board -- replay stored .pcm offline (a later phase). Capture length / pre-trigger: reflash, new epoch.
"""


def now():
    return time.strftime("%Y-%m-%dT%H:%M:%S")


def split_args(arg):
    """Tokens with quotes honoured and backslashes kept: POSIX shlex would eat Windows paths."""
    return [t[1:-1] if len(t) >= 2 and t[0] == t[-1] and t[0] in "\"'" else t for t in shlex.split(arg, posix=False)]


def fmt(v, f="%.1f", none="-"):
    if v is None:
        return none
    try:
        if isinstance(v, float) and not np.isfinite(v):
            return none
        return f % v
    except (TypeError, ValueError):
        return str(v)


# ------------------------------------------------------------------ listening copies

def write_listening_wav(path, raw_words, headroom_db=1.0):
    """16-bit mono, peak-normalised: for ears only (levels are NOT real). The .pcm stays the record."""
    x = wa.words_to_float(raw_words)
    peak = float(np.abs(x).max()) or 1.0
    y = x / peak * (10 ** (-headroom_db / 20.0))
    pcm16 = np.clip(np.round(y * 32767.0), -32768, 32767).astype("<i2")
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(wa.FS)
        w.writeframes(pcm16.tobytes())
    return path


def play(path):
    try:
        import winsound
        winsound.PlaySound(path, winsound.SND_FILENAME | winsound.SND_ASYNC)
    except ImportError:
        os.startfile(path)  # noqa  (non-Windows would need another player; this tool is run on Windows)


def beep():
    try:
        import winsound
        threading.Thread(target=winsound.Beep, args=(2000, 90), daemon=True).start()
    except ImportError:
        print("\a", end="", flush=True)


# ------------------------------------------------------------------ talking to the board

SLOT_NAMES = {"left_slot": "LEFT mic  (L/R->GND)", "right_slot": "RIGHT mic (L/R->3V3)"}
TAP_DB = 12.0                   # a beat this far over the mic's recent peaks is flagged as a tap
UNREACHABLE = ("can't reach the board at %s -- is this PC on the board's Wi-Fi (truing-xxxxxx)? "
               "The board may also have rebooted from a bumped wire; give it ~30 s.")


def describe_reject(ack):
    """Plain words for a refused debug command (a capture or a roller command)."""
    v, why = (ack or {}).get("verdict"), (ack or {}).get("reason")
    if v == "REJECT_STATE":
        return "board is busy (booting, or running its start-up self-play): wait ~30 s and try again"
    if v == "REJECT_DEBUG_DISABLED":
        return "this firmware has the debug channel off: flash s3_devkit_lab or s3_devkit_fastdemo_mic_dualmic"
    if v == "REJECT_SESSION_ADMISSION":
        if why == "NOT_IMPLEMENTED":
            return "this firmware has no bench drive for the roller: flash s3_devkit_lab"
        if why == "SENSOR_TIMEOUT":
            return "the roller is off or faulted: `roll on` first (that also clears a latched fault)"
        if why == "VALUE_OUT_OF_RANGE":
            return "refused: the roller is moving (`roll stop` or wait), or an argument is out of range"
        if why == "CALIBRATION_MISSING":
            return "the roller has no usable speed/acceleration: `roll speed <steps/s> <accel>`"
    return "board refused the command: %s" % (ack or "no answer")


def board_drive_state(host, timeout):
    """The lab image's bench drive as a dict (GET /debug/drive.json), or None when this image has none."""
    try:
        raw, _ = cf.get("http://%s/debug/drive.json" % host, timeout)
    except urllib.error.HTTPError as e:
        if e.code == 404:
            return None
        raise
    return json.loads(raw.decode("utf-8"))


def drive_line(d):
    if d is None:
        return "  roller                 (no bench drive in this image)"
    flags = []
    if d.get("faulted"):
        flags.append("FAULT 0x%x" % d.get("fault_detail", 0))
    if d.get("diag_high"):
        flags.append("DIAG high")
    if d.get("drv_ok") and d.get("drv_flags", "ok") != "ok":
        flags.append("driver: " + d["drv_flags"])
    if d.get("drv_ok") and d.get("gstat", 0) & 0x6:
        flags.append("GSTAT 0x%x" % d["gstat"])
    drv = ("  driver I %2d/31 %s" % (d.get("cs_actual", 0), d.get("drv_flags", "ok"))) if d.get("drv_ok") else \
          ("  driver UART silent" if "drv_ok" in d else "")
    return "  roller                 %-8s %-6s pos %7d steps  %5d steps/s  %6d steps/s2%s%s" % (
        "ENERGISED" if d.get("enabled") else "off", "MOVING" if d.get("moving") else "idle", d.get("position_steps", 0),
        d.get("max_step_hz", 0), d.get("accel_steps_s2", 0), drv, "  !! " + ", ".join(flags) if flags else "")


ROLL_USAGE = ("roll [on | off | stop | zero | <+/-steps> | speed <steps/s> [accel]]  -- `roll` alone shows the state")


def parse_roll(arg):
    """('status',) | ('on',) | ('off',) | ('stop',) | ('zero',) | ('move', steps) | ('speed', hz, accel|None)."""
    t = arg.split()
    if not t:
        return ("status",)
    head = t[0].lower()
    if len(t) == 1 and head in ("on", "off", "stop", "zero"):
        return (head,)
    if head == "speed":
        if len(t) not in (2, 3):
            raise ValueError("roll speed <steps/s> [accel steps/s2]")
        try:
            return ("speed", int(t[1]), int(t[2]) if len(t) == 3 else None)
        except ValueError:
            raise ValueError("roll speed <steps/s> [accel steps/s2] -- whole numbers") from None
    if len(t) == 1:
        try:
            steps = int(head)
        except ValueError:
            raise ValueError(ROLL_USAGE) from None
        return ("move", cr.drive_move_arg(steps))
    raise ValueError(ROLL_USAGE)


def board_seq(host, timeout):
    """The board's current capture sequence number (0 before its first capture)."""
    try:
        raw, _ = cf.get("http://%s/debug/capture.json" % host, timeout)
        return int(json.loads(raw.decode("utf-8")).get("seq", 0))
    except urllib.error.HTTPError as e:
        if e.code == 404:
            return 0
        raise


def level_bar(level_db, lo=-100.0, hi=0.0, width=24):
    if level_db is None or not np.isfinite(level_db):
        return "." * width
    n = int(round((min(max(level_db, lo), hi) - lo) / (hi - lo) * width))
    return "#" * n + "." * (width - n)


def mic_line(name, h, tap=False):
    if h["state"] in ("DEAD", "NO DATA"):
        return "  %-21s %-8s %s" % (name, h["state"], h["why"])
    return "  %-21s %-8s |%s| %6.1f dBFS rms  peak %6.1f%s%s" % (
        name, h["state"], level_bar(h["rms_dbfs"]), h["rms_dbfs"], h["peak_dbfs"],
        "  rails %d" % h["rails"] if h["rails"] else "", "   << TAP" if tap else "")


def slot_channels(meta, local_raw, far_raw):
    """[(slot name, raw words)] LEFT first, whichever of them the board called local."""
    ch = [(meta.get("mic_input") or "mic", local_raw)]
    if far_raw is not None:
        ch.append((meta.get("far_input") or "far", far_raw))
    return sorted(ch, key=lambda c: c[0])


HEARTBEAT_HELP = """
MIC HEARTBEAT -- half a second of both mics from the live stream every ~1 s, nothing saved.
  ALIVE     the samples scatter the way a powered mic's self-noise does (quiet room: about -90..-60 dBFS)
  DEAD      the data line sits on one value: that mic is unpowered, unclocked, or not on GPIO6
  CLIPPING  live but hitting full scale: too loud / too close
Tap a mic: its bar should jump and show << TAP. Ctrl+C stops.
"""


def board_answers(host, timeout=2.5):
    try:
        import urllib.request
        with urllib.request.urlopen("http://%s/id" % host, timeout=timeout):
            return True
    except OSError:
        return False


def truing_networks():
    """The board's AP name(s) this PC can join (Windows): visible networks first, then saved profiles."""
    import re
    import subprocess
    names = []
    for args in (["netsh", "wlan", "show", "networks"], ["netsh", "wlan", "show", "profiles"]):
        try:
            out = subprocess.run(args, capture_output=True, text=True, timeout=15).stdout
        except (OSError, subprocess.SubprocessError):
            continue
        for n in re.findall(r"truing-[0-9a-f]{6}", out):
            if n not in names:
                names.append(n)
    return names


def ensure_board(host, wait_s=90.0, say=print):
    """Get this PC talking to the board with no manual steps: if it does not answer, join its Wi-Fi
    (truing-xxxxxx, a saved Windows profile) and wait for it to boot. True once /id answers."""
    if board_answers(host):
        return True
    import subprocess
    t_end = time.time() + wait_s
    say("   the board is not answering: joining its Wi-Fi and waiting for it (a fresh boot takes ~30 s)")
    last_join = 0.0
    while time.time() < t_end:
        if time.time() - last_join > 15.0:
            last_join = time.time()
            for name in truing_networks()[:1]:
                try:
                    subprocess.run(["netsh", "wlan", "connect", "name=" + name], capture_output=True, timeout=20)
                except (OSError, subprocess.SubprocessError):
                    pass
        if board_answers(host):
            say("   the board is up")
            return True
        time.sleep(2.0)
    say("   still no answer after %d s: is the board powered (USB), and is its Wi-Fi profile saved on this PC?" % wait_s)
    return False


def until_settled(fn, limit=25.0):
    """Call fn(), and while the board answers 409 (a capture it took has not finished settling) keep asking the
    SAME question every 0.15 s instead of starting over: the result is ready as soon as the board is."""
    t_end = time.time() + limit
    while True:
        try:
            return fn()
        except urllib.error.HTTPError as e:
            if e.code != 409 or time.time() > t_end:
                raise
            time.sleep(0.15)


def quick_channels(host, timeout, ms=500):
    """Both mics from the board's live stream: [(slot, raw int32 words)] for `ms` of audio, with none of the
    acoustic analysis a capture drags along (~1 s a beat instead of ~7). The stream interleaves the LEFT
    station's mic (left slot) and the RIGHT station's (right slot) per frame."""
    import urllib.request
    with urllib.request.urlopen("http://%s/debug/listen.pcm?ms=%d&station=2" % (host, ms), timeout=timeout) as r:
        data = r.read()
    w = np.frombuffer(data[:len(data) // 8 * 8], dtype="<i4")
    if w.size < 2 * 48 * 100:
        raise urllib.error.HTTPError("listen", 502, "the stream ended early", None, None)
    return [("left_slot", w[0::2].tobytes()), ("right_slot", w[1::2].tobytes())]


def heartbeat(host, timeout=20.0, beats=None):
    """Repeated no-fire captures on spoke 0 (LEFT local, RIGHT far), one status line per mic per beat."""
    def say(text):
        print(text, flush=True)                           # a live monitor: never sit in a pipe's buffer

    say(HEARTBEAT_HELP)
    ensure_board(host, say=say)
    ws, seq, beat, peaks, tap_run = None, 0, 0, {}, {}
    quick = True                                          # the live stream; falls back to captures on an old image
    try:
        while beats is None or beat < beats:
            try:
                if quick:
                    chans = quick_channels(host, timeout)
                else:
                    if ws is None:
                        ws = cr.WS(host, 80, "/ws")
                    before = until_settled(lambda: board_seq(host, timeout))
                    seq += 1
                    ws.send_text({"cmd": "DEBUG", "code": cr.MEASURE_ONCE_CODE,
                                  "arg": cr.measure_once_arg(0, True, 0), "seq": seq})
                    ack = cr.wait_for_ack(ws, seq, timeout)
                    if ack is None:
                        raise TimeoutError("no answer to the capture request")
                    if ack.get("verdict") != "ACCEPT":
                        say("%s  %s" % (time.strftime("%H:%M:%S"), describe_reject(ack)))
                        time.sleep(3)
                        continue
                    meta, pcm, far = until_settled(lambda: cf.fetch_bundle(host, timeout, with_far=True))
                    if int(meta.get("seq", -1)) == before:
                        say("%s  no new capture came back; retrying" % time.strftime("%H:%M:%S"))
                        continue
                    chans = slot_channels(meta, pcm, far)
            except OSError as e:                          # timeouts, refused, URLError: Wi-Fi or a reboot
                if ws is not None:
                    ws.close()
                    ws = None
                if isinstance(e, urllib.error.HTTPError):      # it answered, it just is not ready yet
                    if quick and e.code in (400, 404):
                        quick = False
                        say("   (this image has no live stream: checking with full captures, ~7 s a beat)")
                        continue
                    time.sleep(0.5 if quick else 2)       # 409: another listener, or a capture still settling
                    continue
                say("%s  lost the board (%s)" % (time.strftime("%H:%M:%S"), e.__class__.__name__))
                if not ensure_board(host, say=say):
                    time.sleep(3)
                continue
            except SystemExit as e:                       # fetch_bundle's refusals, e.g. seq moved mid-download
                say("%s  %s; retrying" % (time.strftime("%H:%M:%S"), e))
                continue
            beat += 1
            say("%s  beat %d" % (time.strftime("%H:%M:%S"), beat))
            for slot, raw in chans:
                h = wa.mic_health(np.frombuffer(raw, dtype="<i4"))
                recent = peaks.setdefault(slot, [])
                tap = h["state"] != "DEAD" and len(recent) >= 3 and h["peak_dbfs"] > np.median(recent[-8:]) + TAP_DB
                # The baseline is the quiet level, so taps stay out of it: otherwise sustained tapping raises
                # the median until the tap stops registering. If a mic stays "tapped" for 20 beats in a row
                # the room really got louder, so let it in and the baseline follows.
                run = tap_run[slot] = (tap_run.get(slot, 0) + 1) if tap else 0
                if h["state"] != "DEAD" and (not tap or run > 20):
                    recent.append(h["peak_dbfs"])
                say(mic_line(SLOT_NAMES.get(slot, slot), h, tap))
            try:
                drv = board_drive_state(host, timeout)
            except (OSError, ValueError):
                drv = None
            if drv is not None:                           # the lab image: the roller's state beside the mics
                say(drive_line(drv))
    except KeyboardInterrupt:
        say("\nheartbeat stopped")
    finally:
        if ws is not None:
            ws.close()


# ------------------------------------------------------------------ figures

def shot_figure(path, title, meta, local, far, m, floor_local, floor_far, focus_hz, ref_spec=None):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    rows = [("local (analysed)", local, floor_local)] + ([("far (reference, not analysed)", far, floor_far)]
                                                         if far is not None else [])
    fig, ax = plt.subplots(len(rows), 3, figsize=(16, 4.2 * len(rows)), squeeze=False)
    fig.suptitle(title, fontsize=10)
    a, b = m["ring_window"]
    for r, (label, words, floor) in enumerate(rows):
        x = wa.words_to_float(words)
        t = np.arange(len(x)) / wa.FS
        ax[r][0].plot(t, x, lw=0.4)
        ax[r][0].axvspan(a / wa.FS, b / wa.FS, color="tab:orange", alpha=0.15, label="host ring window")
        ws, wn = meta.get("expect_window_start_sample"), meta.get("expect_window_n_samples")
        if r == 0 and isinstance(ws, int) and isinstance(wn, int) and wn > 0:
            ax[r][0].axvspan(ws / wa.FS, (ws + wn) / wa.FS, color="tab:green", alpha=0.12, label="board window")
        ax[r][0].set_title("%s waveform" % label, fontsize=9)
        ax[r][0].set_xlabel("s")
        ax[r][0].legend(fontsize=7, loc="upper right")
        f, P = m["_spectra"]["local" if r == 0 else "far"]
        sel = (f >= wa.VIEW_LO_HZ) & (f <= wa.VIEW_HI_HZ)
        ax[r][1].plot(f[sel], P[sel], lw=0.6, label="ring")
        if floor is not None and len(floor[1]) == len(P):
            ax[r][1].plot(f[sel], floor[1][sel], lw=0.6, color="grey", label="epoch floor")
        if ref_spec is not None and r == 0:
            fr, Pr = ref_spec
            sr = (fr >= wa.VIEW_LO_HZ) & (fr <= wa.VIEW_HI_HZ)
            ax[r][1].plot(fr[sr], Pr[sr] - Pr[sr].max() + P[sel].max(), lw=0.5, color="tab:purple", alpha=0.7,
                          label="reference (shape, normalised)")
        ax[r][1].axvspan(*wa.F1_BAND, color="tab:green", alpha=0.08)
        if focus_hz:
            ax[r][1].axvline(focus_hz, color="tab:red", lw=0.6, ls="--")
        ax[r][1].set_title("%s ring spectrum (350-600 shaded = firmware f1 band)" % label, fontsize=9)
        ax[r][1].set_xlabel("Hz")
        ax[r][1].set_ylabel("dBFS")
        ax[r][1].legend(fontsize=7, loc="upper right")
        ax[r][2].specgram(x, NFFT=2048, Fs=wa.FS, noverlap=1536, cmap="magma")
        ax[r][2].set_ylim(0, wa.VIEW_HI_HZ)
        ax[r][2].set_title("%s spectrogram" % label, fontsize=9)
    fig.tight_layout()
    fig.savefig(path, dpi=90)
    plt.close(fig)
    return path


def compare_figure(path, title, items):
    """items: [(label, words, spectra (f, P), onset_sample)] -> onset-aligned waveforms + normalised spectra."""
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    fig, ax = plt.subplots(1, 2, figsize=(16, 4.5))
    fig.suptitle(title, fontsize=10)
    for label, words, (f, P), onset in items:
        x = wa.words_to_float(words)
        a = max(0, onset - int(0.05 * wa.FS))
        seg = x[a:a + int(0.9 * wa.FS)]
        ax[0].plot(np.arange(len(seg)) / wa.FS, seg / (np.abs(seg).max() or 1.0), lw=0.4, label=label)
        sel = (f >= wa.VIEW_LO_HZ) & (f <= wa.VIEW_HI_HZ)
        ax[1].plot(f[sel], P[sel] - P[sel].max(), lw=0.6, label=label)
    ax[0].set_title("onset-aligned, each normalised to its own peak", fontsize=9)
    ax[1].axvspan(*wa.F1_BAND, color="tab:green", alpha=0.08)
    ax[1].set_title("ring spectra, each normalised to its own max (levels NOT comparable across mic positions)",
                    fontsize=9)
    for a_ in ax:
        a_.legend(fontsize=7)
    fig.tight_layout()
    fig.savefig(path, dpi=90)
    plt.close(fig)
    return path


# ------------------------------------------------------------------ the session

class Session:
    def __init__(self, tag, root=EXPLORE):
        self.tag = tag
        self.dir = os.path.join(root, "wb_%s_%s" % (time.strftime("%Y%m%d"), tag))
        existing = sorted(glob.glob(os.path.join(root, "wb_*_%s" % tag)))
        if existing:
            self.dir = existing[-1]                      # resume the latest session with this tag
        os.makedirs(self.dir, exist_ok=True)
        self.state_path = os.path.join(self.dir, "session.json")
        self.shots_path = os.path.join(self.dir, "session.jsonl")
        self.knobs = dict(SHOT_KEYS)
        self.physical = {}
        self.epochs = []
        self.refs = []                                   # json paths
        self.notes = []
        self.shots = []
        self.rig = {}                                    # station -> [(freq, spokes)]
        self._cache = {}                                 # shot id -> (meta, local, far, metrics)
        if os.path.exists(self.state_path):
            with open(self.state_path, "r", encoding="utf-8") as f:
                st = json.load(f)
            self.knobs.update(st.get("knobs", {}))
            self.physical = st.get("physical", {})
            self.epochs = st.get("epochs", [])
            self.refs = st.get("refs", [])
            self.notes = st.get("notes", [])
            self.rig = {k: [tuple(x) for x in v] for k, v in st.get("rig", {}).items()}
        if os.path.exists(self.shots_path):
            with open(self.shots_path, "r", encoding="utf-8") as f:
                self.shots = [json.loads(line) for line in f if line.strip()]
        if not self.epochs:
            self.new_epoch("session start")

    # ---- persistence
    def save(self):
        st = {"tag": self.tag, "knobs": self.knobs, "physical": self.physical, "epochs": self.epochs,
              "refs": self.refs, "notes": self.notes, "rig": self.rig}
        with open(self.state_path, "w", encoding="utf-8") as f:
            json.dump(st, f, indent=2)

    def append_shot(self, rec):
        self.shots.append(rec)
        with open(self.shots_path, "a", encoding="utf-8") as f:
            f.write(json.dumps(rec) + "\n")

    # ---- epochs
    @property
    def epoch(self):
        return self.epochs[-1]

    def new_epoch(self, why, firmware=None):
        e = {"id": "E%d" % (len(self.epochs) + 1), "started": now(), "why": why,
             "physical": dict(self.physical), "firmware": firmware, "decision": None, "decision_why": None}
        self.epochs.append(e)
        self.save()
        return e

    def firmware_check(self, meta):
        fw = {"build": meta.get("build"), "chain_digest": (meta.get("chain_digest") or "")[:12],
              "mode": meta.get("mode")}
        if self.epoch["firmware"] is None:
            self.epoch["firmware"] = fw
            self.save()
            return None
        if fw != self.epoch["firmware"]:
            old = self.epoch["firmware"]
            self.new_epoch("firmware changed: %s -> %s" % (old, fw), fw)
            return "firmware identity changed (%s -> %s): opened %s" % (old, fw, self.epoch["id"])
        return None

    # ---- shots
    def shot(self, sid):
        for s in self.shots:
            if s["id"] == sid:
                return s
        raise KeyError(sid)

    def load(self, rec):
        sid = rec["id"]
        if sid not in self._cache:
            meta, local, far = wa.load_bundle(rec["json"])
            self._cache[sid] = (meta, local, far)
        return self._cache[sid]

    def epoch_shots(self, eid=None, kinds=None):
        eid = eid or self.epoch["id"]
        return [s for s in self.shots if s["epoch"] == eid and (kinds is None or s["kind"] in kinds)]

    def floors(self, eid=None):
        """(floor_local, floor_far, delta_floor) from this epoch's controls, per station of the control."""
        ctrl = self.epoch_shots(eid, ("ctrl",))
        spectra = []
        for s in ctrl:
            meta, local, far = self.load(s)
            m = wa.shot_metrics(meta, local, far, excited=False)
            spectra.append(m["_spectra"])
        if not spectra:
            return None, None, None
        fl = wa.floor_spectrum([sp["local"] for sp in spectra])
        ff = wa.floor_spectrum([sp["far"] for sp in spectra if "far" in sp])
        return fl, ff, wa.delta_floor(spectra)

    def metrics(self, rec, floors=None, focus=None):
        meta, local, far = self.load(rec)
        fl, ff, dfl = floors if floors is not None else self.floors(rec["epoch"])
        m = wa.shot_metrics(meta, local, far, fl, ff, focus, excited=rec["kind"] != "ctrl")
        m["loc"] = wa.localization(m["_spectra"], m["focus_hz"], [f for f, _ in self.rig.get(rec["station"], [])], dfl)
        return meta, local, far, m, (fl, ff, dfl)


def ref_excited(meta):
    """A reference was excited if it fired, or it is a hand pluck (a no-fire capture labelled as one)."""
    label = (meta.get("campaign_label") or meta.get("wb_excitation") or "").lower()
    return meta.get("fired") is True or "pluck" in label


def resolve_reference(token):
    """A bundle json from a name, a .json/.pcm path, or any of its WAV exports. A .norm.wav is only a
    listening copy: it is traced back to the bundle it came from, never analysed itself."""
    t = token.strip().strip('"')
    base = os.path.basename(t)
    for suffix in (".window.norm.wav", ".norm.wav", ".window.wav", ".wav", ".json", ".pcm"):
        if base.endswith(suffix):
            base = base[: -len(suffix)]
            break
    if t.endswith(".json") and os.path.exists(t):
        return os.path.abspath(t)
    candidates = []
    for d in [os.path.dirname(t)] if os.path.dirname(t) else []:
        candidates.append(os.path.join(d, base + ".json"))
    for sub in ("_explore", "_campaign", ""):
        candidates.append(os.path.join(CAPTURES, sub, base + ".json"))
    candidates += glob.glob(os.path.join(EXPLORE, "wb_*", base + ".json"))
    for c in candidates:
        if c and os.path.exists(c):
            return os.path.abspath(c)
    if t.lower().endswith(".wav"):
        raise ValueError("%s is a WAV with no source bundle found: a listening copy's levels are rescaled, so "
                         "it is not analysed. Point at the bundle (.json/.pcm) instead." % t)
    raise ValueError("no bundle found for %r" % token)


# ------------------------------------------------------------------ the REPL

MENU = """
 dual-mic workbench -- EXPLORATORY, not campaign data                        session: %s
 ---------------------------------------------------------------------------------------------
  mic [n]          live heartbeat of both mics (Ctrl+C stops). Start here after any wiring change.
  left | right     which station's mic the board analyses (LEFT = spoke 0, RIGHT = spoke 1)
  ctrl [n]         n silent captures, saved        pluck [n]   cued hand pluck, saved
  fire [n]         solenoid strikes (needs solenoids wired and enabled in the firmware)
  roll [...]       the roller, lab image only: `roll on`, `roll 1600`, `roll -1600`, `roll stop`, `roll off`
  listen [s]       record the mic live for s seconds (up to 30) and play it: the ear / wiring check
  burst [n] [ms] [gap]  rapid solenoid burst while both mics are watched live (needs PRESENT=1 in the image)
  play local|far   listen to the last shot         view        reopen its figure
  ls               shots so far                    report      write report.md for the session
  knobs            current set-up                  guide       the full experiment loop
  menu             this list                       help <cmd>  details on one command      quit
"""


class Workbench(cmd.Cmd):
    def __init__(self, session, offline):
        super().__init__()
        self.s = session
        self.offline = offline
        self.ws = None
        self.seq_counter = [0]
        self.intro = MENU % session.tag
        self._update_prompt()

    # ---- plumbing
    def _update_prompt(self):
        k = self.s.knobs
        st = cr.station_for_spoke(int(k["spoke"]))
        pulse = ", pulse %s ms" % k["pulse"] if k["pulse"] else ""
        self.prompt = "[%s | %s mic, spoke %s%s%s] wb> " % (self.s.epoch["id"], st, k["spoke"], pulse,
                                                           " | OFFLINE" if self.offline else "")

    def emptyline(self):
        pass

    def default(self, line):
        print("unknown command %r -- `menu` lists the common ones, `help` all of them" % line.split()[0])

    def postcmd(self, stop, line):
        self._update_prompt()
        return stop

    def onecmd(self, line):
        try:
            return super().onecmd(line)
        except (ValueError, KeyError, IndexError) as e:
            print("!! %s" % e)
        except SystemExit as e:
            print("!! %s" % e)
        except KeyboardInterrupt:
            print("\n   stopped")
        except OSError as e:                              # timeouts, refused, URLError: Wi-Fi or a reboot
            self._drop_ws()
            print("!! " + UNREACHABLE % self.s.knobs["host"] + " (%s)" % e.__class__.__name__)
        return False

    def _ws(self):
        if self.ws is None:
            self.ws = cr.WS(self.s.knobs["host"], 80, "/ws")
        return self.ws

    def _drop_ws(self):
        if self.ws is not None:
            self.ws.close()
            self.ws = None

    def _seq_now(self):
        return board_seq(self.s.knobs["host"], float(self.s.knobs["timeout"]))

    # ---- the roller (lab image)
    def _drive_cmd(self, code, arg=0):
        """One DRIVE_* debug code; refusals come back as plain words."""
        self.seq_counter[0] += 1
        ws = self._ws()
        ws.send_text({"cmd": "DEBUG", "code": code, "arg": int(arg), "seq": self.seq_counter[0]})
        ack = cr.wait_for_ack(ws, self.seq_counter[0], float(self.s.knobs["timeout"]))
        if ack is None:
            raise TimeoutError("no answer to the roller command")
        if ack.get("verdict") != "ACCEPT":
            raise ValueError(describe_reject(ack))

    def _drive_state(self):
        d = board_drive_state(self.s.knobs["host"], float(self.s.knobs["timeout"]))
        if d is None:
            raise ValueError(describe_reject({"verdict": "REJECT_SESSION_ADMISSION", "reason": "NOT_IMPLEMENTED"}))
        return d

    def _sync_roller(self, drv):
        """Energised coils are part of the physical set-up (holding current is a noise source next to the
        mics), so a change opens a new epoch and the floor is taken again for it."""
        want = "energised" if drv["enabled"] else "off"
        if self.s.physical.get("roller", "off") != want:
            self.s.physical["roller"] = want
            e = self.s.new_epoch("roller %s" % want, self.s.epoch["firmware"])
            print("   roller is %s: new epoch %s. Take `ctrl 3` for this epoch's floor." % (want, e["id"]))

    def _await_roller(self, steps):
        """Poll until the move ends. Ctrl+C sends STOP (a controlled deceleration) and keeps waiting."""
        d = self._drive_state()
        hz = max(1, int(d.get("max_step_hz") or 1))
        deadline = time.time() + abs(steps) / hz * 2.0 + 5.0
        stopping = False
        while time.time() < deadline:
            try:
                time.sleep(0.25)
                d = self._drive_state()
                if not d["moving"]:
                    break
            except KeyboardInterrupt:
                if not stopping:
                    self._drive_cmd(cr.DRIVE_STOP_CODE)
                    print("   stop requested: decelerating")
                    stopping = True
                    deadline = time.time() + 5.0
        else:
            print("   !! still moving after the expected time: `roll stop`")
        print(drive_line(self._drive_state()))

    def do_roll(self, arg):
        """roll [on | off | stop | zero | <+/-steps> | speed <steps/s> [accel]]: the roller on the lab image (s3_devkit_lab).
        `roll` alone shows its state. `roll on` energises the coils (and opens a new epoch: holding current is a
        noise source); a move is raw steps at the ramp shown, and Ctrl+C during one decelerates it to a stop.
        `roll off` releases the coils. Position counts pulses sent, not proof the wheel followed."""
        if self.offline:
            raise ValueError("offline session: no board")
        op = parse_roll(arg)
        if op[0] == "status":
            print(drive_line(self._drive_state()))
        elif op[0] in ("on", "off"):
            self._drive_cmd(cr.DRIVE_ENABLE_CODE, 1 if op[0] == "on" else 0)
            d = self._drive_state()
            self._sync_roller(d)
            print(drive_line(d))
        elif op[0] == "stop":
            self._drive_cmd(cr.DRIVE_STOP_CODE)
            time.sleep(0.3)
            print(drive_line(self._drive_state()))
        elif op[0] == "zero":
            self._drive_cmd(cr.DRIVE_ZERO_CODE)
            print(drive_line(self._drive_state()))
        elif op[0] == "speed":
            _, hz, accel = op
            accel = accel if accel is not None else int(self._drive_state().get("accel_steps_s2") or 1600)
            self._drive_cmd(cr.DRIVE_MOTION_CODE, cr.drive_motion_arg(hz, accel))
            print(drive_line(self._drive_state()))
        else:
            self._drive_cmd(cr.DRIVE_MOVE_CODE, op[1])
            self._await_roller(op[1])

    def _roller_before_shot(self):
        """The roller's state at the moment of a shot, or None on an image with no bench drive. The firmware
        refuses a capture while it moves, so this cannot change during one."""
        drv = board_drive_state(self.s.knobs["host"], float(self.s.knobs["timeout"]))
        if drv is None:
            return None
        if drv["moving"]:
            raise ValueError("the roller is moving: `roll stop`, or wait for it to finish")
        self._sync_roller(drv)
        return drv

    # ---- 0 is anything listening
    def do_mic(self, arg):
        """mic [n]: heartbeat -- a silent capture every few seconds, printing whether each mic is ALIVE, DEAD or
        CLIPPING, with a level bar. Nothing is saved. Runs until Ctrl+C, or for n beats."""
        if self.offline:
            raise ValueError("offline session: no board")
        self._drop_ws()                                   # the heartbeat opens its own connection
        heartbeat(self.s.knobs["host"], float(self.s.knobs["timeout"]), int(arg) if arg.strip() else None)

    do_heartbeat = do_mic
    do_hb = do_mic

    def do_burst(self, arg):
        """burst [shots] [pulse_ms] [gap_ms] [left|right|both]: fire the solenoids in a rapid burst (default 20 shots
        of 20 ms every 250 ms, alternating) while BOTH mics are watched live, ten times a second: does a mic
        stay ALIVE, does the rail sag, how loud is each hit. The gap is held to >= 4 x the pulse (25 % duty).
        Saves both channels as listening wavs. Needs the solenoids enabled in the image (PRESENT=1)."""
        if self.offline:
            raise ValueError("offline session: no board")
        nums, which = [], "both"
        for tok in arg.split():
            if tok.lower() in ("left", "right", "both"):
                which = tok.lower()
            else:
                nums.append(int(tok))
        shots, pulse, gap = (nums + [20, 20, 250][len(nums):])[:3]
        gap = max(gap, 4 * pulse, 100)
        fs = {"left": 0, "right": 1, "both": 2}[which]
        host = self.s.knobs["host"]
        ms = 400 + shots * gap + 800
        url = "http://%s/debug/listen.pcm?ms=%d&station=2&fn=%d&fp=%d&fg=%d&fs=%d" % (host, ms, shots, pulse, gap, fs)
        n = len(glob.glob(os.path.join(self.s.dir, "burst_*_LEFT.pcm"))) + 1
        base = os.path.join(self.s.dir, "burst_%02d" % n)
        self._drop_ws()

        def beats(label, count):
            for _ in range(count):
                try:
                    lines = []
                    for slot, raw in quick_channels(host, 20.0):
                        lines.append(mic_line(SLOT_NAMES.get(slot, slot), wa.mic_health(np.frombuffer(raw, dtype="<i4")), False))
                    print("   heartbeat %s  %s" % (label, time.strftime("%H:%M:%S")))
                    print("\n".join(lines))
                except OSError as e:
                    print("   heartbeat %s: no answer (%s)" % (label, e.__class__.__name__))

        beats("BEFORE", 3)
        print("   ==== BURST STARTING NOW: %d solenoid shots (%s), %d ms each, one every %d ms, over ~%.1f s ===="
              % (shots, which.upper(), pulse, gap, ms / 1000.0))
        print("   watch or listen: the first shot fires in about 0.4 s. Each one is announced below as it happens.")
        # When each shot is due (the firmware fires the first 0.4 s into the stream, then every `gap`), so the
        # printout can say out loud that a solenoid is firing right now and you can check it by ear or eye.
        sched = [(0.4 + k * gap / 1000.0, "LEFT" if fs == 0 or (fs == 2 and k % 2 == 0) else "RIGHT", k + 1)
                 for k in range(shots)]
        blk = 9600                                            # 0.2 s of frames; two words each
        need = blk * 2 * 4
        chans = {"left_slot": [], "right_slot": []}
        peaks = {"left_slot": [], "right_slot": []}
        dead = {"left_slot": 0, "right_slot": 0}
        hits = 0
        t = 0.0
        try:
            import urllib.request
            with urllib.request.urlopen(url, timeout=ms / 1000.0 + 20) as r:
                buf = b""
                while True:
                    part = r.read(need - len(buf))
                    if not part:
                        break
                    buf += part
                    if len(buf) < need:
                        continue
                    w = np.frombuffer(buf, dtype="<i4")
                    buf = b""
                    row, hit = [], False
                    for slot, arr in (("left_slot", w[0::2]), ("right_slot", w[1::2])):
                        chans[slot].append(arr.copy())
                        h = wa.mic_health(arr)
                        rms = h["rms_dbfs"] if h["rms_dbfs"] is not None else -999.0
                        pk = h["peak_dbfs"] if h["peak_dbfs"] is not None else -999.0
                        base_pk = np.median(peaks[slot][-8:]) if len(peaks[slot]) >= 3 else None
                        this_hit = h["state"] != "DEAD" and base_pk is not None and pk > base_pk + TAP_DB
                        hit = hit or this_hit
                        if h["state"] == "DEAD":
                            dead[slot] += 1
                        else:
                            peaks[slot].append(pk)
                        row.append("%s %-8s %6.1f rms %6.1f pk" % ("L" if slot == "left_slot" else "R", h["state"], rms, pk))
                    hits += 1 if hit else 0
                    t += blk / 48000.0
                    win = int(round(t * 5.0)) - 1                     # 0-based 0.2 s window number, no float drift
                    due = [x for x in sched if int(x[0] * 5.0 + 1e-6) == win]
                    tag = "".join("   >>> FIRING %s SOLENOID: shot %d of %d <<<" % (x[1], x[2], shots) for x in due)
                    print("  %4.1fs  %s  |  %s%s%s" % (t, row[0], row[1], "   << HIT" if hit else "", tag))
        except urllib.error.HTTPError as e:
            raise ValueError("the board refused: %s %s" % (e.code, e.read().decode(errors="replace")[:160]))
        except OSError as e:
            raise ValueError(UNREACHABLE % (host,) + " (%s)" % e)
        if not chans["left_slot"]:
            raise ValueError("no samples came back")
        for slot, tag in (("left_slot", "LEFT"), ("right_slot", "RIGHT")):
            write_listening_wav(base + "_%s.norm.wav" % tag, np.concatenate(chans[slot]))
        total = len(chans["left_slot"])
        print("   ==== BURST FINISHED: %d shots were commanded (the board's own count is in its serial log) ====" % shots)
        print("   summary: %d windows, %d with a hit  |  LEFT dead in %d, RIGHT dead in %d" % (
            total, hits, dead["left_slot"], dead["right_slot"]))
        if dead["left_slot"] == 0 and dead["right_slot"] == 0:
            print("   both mics stayed ALIVE through the whole burst")
        else:
            print("   !! a mic went DEAD during the burst: suspect the supply rail or the solenoid wiring")
        print("   files:", base + "_LEFT.norm.wav", "and", base + "_RIGHT.norm.wav")
        beats("AFTER", 3)

    def do_listen(self, arg):
        """listen [seconds] [left|right]: record the mic live for as long as you like (1..30 s, default 10) and
        play it back. A capture is only 1.2 s; this streams the board's microphone ring instead. It is a
        wiring/ear check: nothing is analysed and nothing is added to the shots. The .pcm and a
        peak-normalised .wav (levels NOT real) land in the session folder."""
        if self.offline:
            raise ValueError("offline session: no board")
        seconds, station = 10, cr.station_for_spoke(int(self.s.knobs["spoke"]))
        for tok in arg.split():
            if tok.lower() in ("left", "right"):
                station = tok.upper()
            else:
                seconds = int(tok)
        if not 1 <= seconds <= 30:
            raise ValueError("seconds must be 1..30")
        host = self.s.knobs["host"]
        n = len(glob.glob(os.path.join(self.s.dir, "listen_*.pcm"))) + 1
        base = os.path.join(self.s.dir, "listen_%02d_%s" % (n, station))
        url = "http://%s/debug/listen.pcm?s=%d&station=%d" % (host, seconds, 0 if station == "LEFT" else 1)
        self._drop_ws()
        print("   recording the %s mic for %d s -- make your noise now (the board serves nothing else meanwhile)"
              % (station, seconds))
        try:
            import urllib.request
            with urllib.request.urlopen(url, timeout=seconds + 15) as r:
                data = r.read()
        except urllib.error.HTTPError as e:
            raise ValueError("the board refused: %s %s" % (e.code, e.read().decode(errors="replace")[:120]))
        except OSError as e:
            raise ValueError(UNREACHABLE % (host,) + " (%s)" % e)
        words = np.frombuffer(data[:len(data) // 4 * 4], dtype="<i4")
        if words.size == 0:
            raise ValueError("no samples came back -- is this the lab/dual-mic image? (`roll` should work too)")
        with open(base + ".pcm", "wb") as f:
            f.write(words.tobytes())
        write_listening_wav(base + ".norm.wav", words)
        h = wa.mic_health(words)
        got = words.size / float(wa.FS)
        print("   %s mic: %.1f of %d s recorded, %s%s" % (
            station, got, seconds, h["state"], "" if h["rms_dbfs"] is None else "  %.1f dBFS rms" % h["rms_dbfs"]))
        if got < seconds - 0.5:
            print("   !! it ended early: the board could not keep the stream going (Wi-Fi too slow?); try again")
        print("   file:", base + ".norm.wav")
        play(base + ".norm.wav")

    def do_menu(self, arg):
        """The common commands."""
        print(MENU % self.s.tag)

    def do_left(self, arg):
        """left: analyse the LEFT station's mic (spoke 0)."""
        self.do_set("spoke 0")
        print("   the board now analyses the LEFT mic; RIGHT is recorded alongside as `far`")

    def do_right(self, arg):
        """right: analyse the RIGHT station's mic (spoke 1)."""
        self.do_set("spoke 1")
        print("   the board now analyses the RIGHT mic; LEFT is recorded alongside as `far`")

    # ---- 1 experiment
    def do_guide(self, arg):
        """The loop, what to look for, and what may change."""
        print(GUIDE)

    def do_knobs(self, arg):
        """Show shot knobs, the physical set-up of this epoch, and the active references."""
        k = self.s.knobs
        st = cr.station_for_spoke(int(k["spoke"]))
        print("shot knobs   spoke %s (%s station)  pulse %s  reps %s  interval %ss  lead %ss  focus %s  autoview %s"
              % (k["spoke"], st, (str(k["pulse"]) + " ms") if k["pulse"] else "profile", k["reps"], k["interval"],
                 k["lead"], k["focus"] or "auto", k["autoview"]))
        print("epoch %s     %s  firmware %s  decision %s" % (self.s.epoch["id"], self.s.epoch["why"],
                                                          self.s.epoch["firmware"], self.s.epoch["decision"]))
        print("physical     %s" % (", ".join("%s=%s" % kv for kv in sorted(self.s.physical.items())) or "(none recorded)"))
        print("references   %s" % (", ".join(os.path.basename(r)[:-5] for r in self.s.refs) or "(none)"))

    def do_set(self, arg):
        """set <key> <value>. Shot knobs: spoke pulse reps interval lead focus autoview host timeout.
        Any other key is PHYSICAL set-up (mic.dist, mic.coupling, mic.mount, strike.point, standoff, hold,
        damping ...) and opens a new epoch. `set pulse 0` = the excitation profile's width."""
        parts = split_args(arg)
        if len(parts) < 2:
            raise ValueError("set <key> <value>")
        key, val = parts[0], " ".join(parts[1:])
        if key in SHOT_KEYS:
            if key in ("spoke", "pulse", "reps"):
                v = int(val)
            elif key in ("interval", "lead", "timeout"):
                v = float(val)
            elif key == "focus":
                v = None if val in ("auto", "none", "-") else float(val)
            elif key == "autoview":
                v = val.lower() in ("1", "on", "true", "yes")
            else:
                v = val
            if key == "pulse" and v:
                cr.measure_once_arg(0, False, v)
                lo, hi = BRACKET_MS[cr.station_for_spoke(int(self.s.knobs["spoke"]))]
                if not lo <= v <= hi:
                    print("   note: %d ms is outside the OLD rig's B2-M3 bracket [%d, %d] -- unverified on this "
                          "rig; not yet re-measured" % (v, lo, hi))
            self.s.knobs[key] = v
            self.s.save()
            return
        if self.s.physical.get(key) == val:
            return
        self.s.physical[key] = val
        e = self.s.new_epoch("set %s=%s" % (key, val), self.s.epoch["firmware"])
        print("   physical set-up changed: new epoch %s. Take `ctrl 3` for this epoch's floor." % e["id"])

    def do_unset(self, arg):
        """unset <physical key>: drop it from the set-up (opens a new epoch)."""
        key = arg.strip()
        if key in self.s.physical:
            del self.s.physical[key]
            e = self.s.new_epoch("unset %s" % key, self.s.epoch["firmware"])
            print("   new epoch %s" % e["id"])

    def do_fire(self, arg):
        """fire [n]: n strikes at the current spoke and pulse (default: reps)."""
        self._shots("strike", arg)

    def do_ctrl(self, arg):
        """ctrl [n]: no-fire controls on the current spoke's station -- the room floor for both mics."""
        self._shots("ctrl", arg)

    def do_air(self, arg):
        """air [n]: strikes with the wheel turned so the plunger lands in a gap (nothing to hit)."""
        print("   air shot: the plunger must be at a gap between spokes.")
        self._shots("air", arg)

    def do_pluck(self, arg):
        """pluck [n]: cued hand pluck. Beep, `lead` seconds, then a no-fire capture: pluck on the beep."""
        self._shots("pluck", arg)

    def _shots(self, kind, arg):
        if self.offline:
            raise ValueError("offline session: no board")
        n = int(arg) if arg.strip() else int(self.s.knobs["reps"])
        for i in range(n):
            if kind == "pluck":
                beep()
                time.sleep(float(self.s.knobs["lead"]))
            self._one(kind, i, n, show=i + 1 == n)
            if i + 1 < n:
                time.sleep(float(self.s.knobs["interval"]))

    def _one(self, kind, i, n, show=True):
        k = self.s.knobs
        spoke = int(k["spoke"])
        station = cr.station_for_spoke(spoke)
        no_fire = kind in ("ctrl", "pluck")
        pulse = 0 if no_fire else int(k["pulse"] or 0)
        drv = self._roller_before_shot()
        seq_before = self._seq_now()
        self.seq_counter[0] += 1
        ws = self._ws()
        ws.send_text({"cmd": "DEBUG", "code": cr.MEASURE_ONCE_CODE,
                      "arg": cr.measure_once_arg(spoke, no_fire, pulse), "seq": self.seq_counter[0]})
        ack = cr.wait_for_ack(ws, self.seq_counter[0], float(k["timeout"]))
        if ack is None:
            raise TimeoutError("no answer to the shot")
        if ack.get("verdict") != "ACCEPT":
            raise ValueError(describe_reject(ack))
        meta, pcm, far = cf.fetch_bundle(k["host"], float(k["timeout"]), with_far=True)
        if int(meta.get("seq", -1)) == seq_before:
            raise ValueError("no new capture came back%s" % (
                " -- `fire`/`air` need a solenoid wired and enabled in the firmware" if not no_fire else ""))
        fw_note = self.s.firmware_check(meta)
        if fw_note:
            print("   " + fw_note)
        sid = len(self.s.shots) + 1
        name = "wb%03d_%s_%s_sp%d_%s_seq%d" % (sid, self.s.epoch["id"], kind, spoke,
                                               ("p%d" % pulse) if pulse else "prof", int(meta.get("seq", 0)))
        extra = {"campaign_label": "WORKBENCH", "campaign_session": self.s.tag, "campaign_station": station,
                 "campaign_rig_id": {"LEFT": cr.DEFAULT_RIG_ID_LEFT, "RIGHT": cr.DEFAULT_RIG_ID_RIGHT}[station],
                 "campaign_kind": "no_fire" if no_fire else ("air_shot" if kind == "air" else "strike"),
                 "wb_session": self.s.tag, "wb_epoch": self.s.epoch["id"], "wb_excitation": kind}
        for key, val in self.s.physical.items():
            extra["wb_phys_" + key.replace(".", "_")] = str(val)
        roller = None if drv is None else ("energised" if drv["enabled"] else "off")
        if roller is not None:
            extra["wb_roller"] = roller
        doc = cf.write_bundle(name, dict(meta, **extra), pcm, NOTE, self.s.dir, far_pcm=far)
        cf.add_to_index(name, self.s.dir)
        flags = []
        if no_fire and doc.get("fired") is not False:
            flags.append("asked for no-fire but fired=%s" % doc.get("fired"))
        if not no_fire and doc.get("fired") is False:
            flags.append("asked to fire but fired=false")
        if pulse and abs(float(doc.get("pulse_ms") or 0) - pulse) > 0.01:
            flags.append("asked %d ms, board applied %s" % (pulse, doc.get("pulse_ms")))
        if doc.get("mic_input") and doc["mic_input"] != EXPECTED_MIC_INPUT[station]:
            flags.append("mic_input %s but the board profile maps %s to %s" % (doc["mic_input"], station,
                                                                             EXPECTED_MIC_INPUT[station]))
        if doc.get("capture_result") not in (None, "OK"):
            flags.append("capture_result=%s" % doc.get("capture_result"))
        rec = {"id": sid, "t": now(), "epoch": self.s.epoch["id"], "kind": kind, "spoke": spoke, "station": station,
               "pulse_req": pulse, "name": name, "json": os.path.join(self.s.dir, name + ".json"),
               "physical": dict(self.s.physical), "flags": flags, "roller": roller}
        self._observe(rec, header="[%d/%d] #%d %s sp%d %s" % (i + 1, n, sid, kind, spoke, station), show=show)
        self.s.append_shot(rec)
        if kind == "strike":
            self._refresh_rig(quiet=True)

    # ---- 2 observe
    def _observe(self, rec, header=None, show=True):
        meta, local, far, m, floors = self.s.metrics(rec, focus=self.s.knobs["focus"])
        fl, ff, dfl = floors
        d = self.s.dir
        rec["png"] = os.path.join(d, rec["name"] + ".png")
        rec["wav_local"] = write_listening_wav(os.path.join(d, rec["name"] + ".local.norm.wav"), local)
        rec["wav_far"] = write_listening_wav(os.path.join(d, rec["name"] + ".far.norm.wav"), far) if far is not None else None
        ref_spec = None
        if self.s.refs:
            rm = self._ref_metrics(self.s.refs[0])
            ref_spec = rm[3]["_spectra"]["local"]
        title = "#%d %s  %s sp%d %s  pulse %s  epoch %s  %s" % (
            rec["id"], rec["name"], rec["kind"], rec["spoke"], rec["station"], meta.get("pulse_ms"), rec["epoch"],
            " ".join("%s=%s" % kv for kv in sorted(rec["physical"].items())))
        shot_figure(rec["png"], title, meta, local, far, m, fl, ff, m["focus_hz"], ref_spec)
        rec["board"] = {k: meta.get(k) for k in ("status", "reason", "expect_f1_hz", "expect_snr_db", "fired", "pulse_ms",
                                                "capture_result", "build", "mode", "mic_input", "far_input", "seq",
                                                "expect_window_truncated_by")}
        rec["m"] = {ch: {k: v for k, v in m[ch].items() if k != "lines"} for ch in ("local", "far") if ch in m}
        rec["lines_local"] = [ln[:3] for ln in m["local"]["lines"]]
        rec["lines_far"] = [ln[:3] for ln in m["far"]["lines"]] if "far" in m else []
        rec["focus_hz"], rec["focus_source"] = m["focus_hz"], m["focus_source"]
        rec["loc"] = m["loc"]
        if header:
            print(header + "  -> " + rec["name"] + ("   [roller %s]" % rec["roller"] if rec.get("roller") else ""))
        dead = False
        for slot, raw in slot_channels(meta, local, far):
            role = "local" if slot == meta.get("mic_input") else "far"
            h = wa.mic_health(raw)
            dead = dead or h["state"] == "DEAD"
            print(mic_line("%s %s" % (SLOT_NAMES.get(slot, slot).split("(")[0].strip(), role), h))
        if dead:
            print("   !! a DEAD channel's numbers below describe a floating wire, not sound -- fix the wiring first "
                  "(`mic` watches it live)")
        for ch in ("local", "far"):
            if ch not in m:
                print("   %-5s  (no far channel on this image)" % ch)
                continue
            c = m[ch]
            print("   %-5s  peak %s dBFS  rail %d  impact %s dBFS @%.2fs  in-band %s (over floor %s)  wide over floor %s"
                  % (ch, fmt(c["peak_dbfs"]), c["rail_hits"], fmt(c["impact_dbfs"]), c["impact_s"], fmt(c["in_band_db"]),
                     fmt(c.get("in_band_over_floor_db")), fmt(c.get("wide_over_floor_db"))))
            print("          lines  " + "  ".join("%.1f(%s)" % (ln[0], fmt(ln[3] if ln[3] is not None else ln[2], "%+.0f"))
                                         for ln in c["lines"]))
        b = rec["board"]
        print("   board  %s/%s  f1 %s  snr %s  fired %s pulse %s  mic %s  (DSP saw the local mic only)"
              % (b["status"], b["reason"], fmt(b["expect_f1_hz"]), fmt(b["expect_snr_db"]), b["fired"], b["pulse_ms"],
                 b["mic_input"] or "single"))
        L = m["loc"]
        if L:
            print("   loc    focus %.1f Hz (%s)  delta %s dB  rig delta %s (n=%d)  floor delta %s  INDEX %s (vs %s)"
                  % (L["focus_hz"], m["focus_source"], fmt(L["delta_db"]), fmt(L["delta_rig_db"]), L["n_rig"],
                     fmt(L["delta_floor_db"]), fmt(L["index_db"], "%+.1f"), L["reference"]))
        if fl is None and rec["kind"] != "ctrl":
            print("   (no floor yet in this epoch: `ctrl 3`)")
        for fl_ in rec.get("flags", []):
            print("   !! " + fl_)
        if show and self.s.knobs["autoview"]:
            os.startfile(rec["png"])  # noqa

    def do_view(self, arg):
        """view [id]: reopen a shot's figure (default: last)."""
        rec = self._pick(arg)
        os.startfile(rec["png"])  # noqa

    def do_play(self, arg):
        """play local|far|ref [id]: listening copy (peak-normalised, NOT real levels). Default: last shot."""
        parts = arg.split()
        which = parts[0] if parts else "local"
        if which == "ref":
            if not self.s.refs:
                raise ValueError("no reference: `ref ...`")
            idx = int(parts[1]) - 1 if len(parts) > 1 else 0
            meta, local, _ = wa.load_bundle(self.s.refs[idx])
            p = os.path.join(self.s.dir, "ref_%s.norm.wav" % meta.get("name", "ref"))
            if not os.path.exists(p):
                write_listening_wav(p, local)
            play(p)
            return
        rec = self._pick(parts[1] if len(parts) > 1 else "")
        p = rec.get("wav_" + which)
        if not p:
            raise ValueError("shot #%d has no %s channel" % (rec["id"], which))
        play(p)

    def do_export(self, arg):
        """export [id]: faithful 24-bit WAVs (real levels) of a shot's local and far channels."""
        rec = self._pick(arg)
        meta, local, far = self.s.load(rec)
        for ch, w in (("local", local), ("far", far)):
            if w is None:
                continue
            p = os.path.join(self.s.dir, "%s.%s.faithful.wav" % (rec["name"], ch))
            capture_wav.write_mono_pcm24_wav(p, [int(v) >> 8 for v in w], wa.FS)
            print("   " + p)

    def do_ls(self, arg):
        """ls [all]: shots of this epoch (all: every epoch)."""
        rows = self.s.shots if arg.strip() == "all" else self.s.epoch_shots()
        for r in rows:
            b = r.get("board", {})
            L = r.get("loc") or {}
            print("#%-3d %s %-6s sp%-2d %-5s %5s  %-9s %-24s f1 %6s snr %5s  d %5s idx %5s %s"
                  % (r["id"], r["epoch"], r["kind"], r["spoke"], r["station"], b.get("pulse_ms"), b.get("status"),
                     b.get("reason"), fmt(b.get("expect_f1_hz")), fmt(b.get("expect_snr_db")), fmt(L.get("delta_db")),
                     fmt(L.get("index_db"), "%+.1f"), "!!" if r.get("flags") else ""))

    def _pick(self, arg):
        if not self.s.shots:
            raise ValueError("no shots yet")
        a = arg.strip().lstrip("#")
        return self.s.shot(int(a)) if a else self.s.shots[-1]

    # ---- references
    def do_ref(self, arg):
        """ref <bundle name | .json | .pcm | .wav export>  add a reference (a WAV is traced to its bundle)
        ref preset spoke0-pluck                          the 09-21 hand plucks that cleared at 406.2 Hz
        ref list | ref clear"""
        parts = split_args(arg)
        if not parts or parts[0] == "list":
            for i, r in enumerate(self.s.refs, 1):
                meta, _, _ = wa.load_bundle(r)
                print("   %d  %s  sp%s %s fired=%s  %s/%s f1 %s snr %s  chain %s" % (
                    i, meta.get("name"), meta.get("spoke_id"), meta.get("station"), meta.get("fired"), meta.get("status"),
                    meta.get("reason"), fmt(meta.get("expect_f1_hz")), fmt(meta.get("expect_snr_db")),
                    (meta.get("chain_digest") or "")[:12]))
            return
        if parts[0] == "clear":
            self.s.refs = []
            self.s.save()
            return
        names = REF_PRESETS[parts[1]] if parts[0] == "preset" else [" ".join(parts)]
        for n in names:
            p = resolve_reference(n)
            if p not in self.s.refs:
                self.s.refs.append(p)
            print("   + %s" % p)
        self.s.save()
        print("   note: references taken with the old single mic at its old position -- compare frequencies and "
              "shape, not absolute level.")

    def _ref_metrics(self, path):
        meta, local, far = wa.load_bundle(path)
        return meta, local, far, wa.shot_metrics(meta, local, None, excited=ref_excited(meta))

    # ---- 3 analyze
    def do_compare(self, arg):
        """compare [id|ref[N]] [id|ref[N]]: overlay two captures (default: last shot vs first reference)."""
        parts = arg.split()
        items = []
        for tok in (parts + ["", "ref"])[:2] if len(parts) < 2 else parts[:2]:
            if tok.startswith("ref"):
                idx = int(tok[3:] or 1) - 1
                meta, local, _, m = self._ref_metrics(self.s.refs[idx])
                label = "ref %s" % meta.get("name")
            else:
                rec = self._pick(tok)
                meta, local, _, m, _ = self.s.metrics(rec)
                label = "#%d %s" % (rec["id"], rec["name"])
            onset = meta.get("expect_onset_sample") if meta.get("expect_onsets") else None
            if not isinstance(onset, int) or onset <= 0:
                onset = int(wa.impact(wa.words_to_float(local))[1] * wa.FS)
            items.append((label, local, m["_spectra"]["local"], onset, meta, m))
        print("   %-44s %-10s %-22s %8s %6s %9s %s" % ("capture", "status", "reason", "f1", "snr", "trunc", "top lines"))
        for label, _, _, _, meta, m in items:
            print("   %-44s %-10s %-22s %8s %6s %9s %s" % (
                label[:44], meta.get("status"), meta.get("reason"), fmt(meta.get("expect_f1_hz")),
                fmt(meta.get("expect_snr_db")), meta.get("expect_window_truncated_by"),
                " ".join("%.1f" % ln[0] for ln in m["local"]["lines"])))
        pairs = wa.match_lines(items[0][5]["local"]["lines"], items[1][5]["local"]["lines"])
        print("   top lines agreeing within +-%.0f Hz: %s" % (wa.LINE_TOL_HZ, ", ".join("%.1f~%.1f" % p for p in pairs) or "none"))
        for (la, _, _, _, ma, _), (lb, _, spb, _, _, _) in ((items[0], items[1]), (items[1], items[0])):
            f0 = ma.get("expect_f1_hz")
            if isinstance(f0, (int, float)) and f0 == f0:
                ok, prom, fq = wa.line_presence(*spb, f0)
                print("   %s's board f1 %.1f Hz in %s: %s (prominence %s dB at %s Hz)" % (
                    la[:30], f0, lb[:30], "PRESENT" if ok else "absent", fmt(prom), fmt(fq)))
        p = os.path.join(self.s.dir, "compare_%s.png" % time.strftime("%H%M%S"))
        compare_figure(p, " vs ".join(i[0] for i in items), [(i[0], i[1], i[2], i[3]) for i in items])
        if self.s.knobs["autoview"]:
            os.startfile(p)  # noqa

    def _refresh_rig(self, quiet=False):
        fl_all = self.s.floors()
        if fl_all[0] is None:
            if not quiet:
                print("   no controls in this epoch: `ctrl 3` first (lines are measured over the floor)")
            return
        for station in ("LEFT", "RIGHT"):
            by_spoke = {}
            for s in self.s.epoch_shots(kinds=("strike",)):
                if s["station"] != station:
                    continue
                meta, local, far = self.s.load(s)
                m = wa.shot_metrics(meta, local, far, excited=True)
                by_spoke.setdefault(s["spoke"], []).append(m["_spectra"]["local"])
            if len(by_spoke) < 2:
                if not quiet:
                    print("   %s: %d spoke(s) struck in this epoch -- shared lines need >= 2" % (station, len(by_spoke)))
                continue
            lines = {sp: wa.consistent_lines(v, fl_all[0]) for sp, v in by_spoke.items()}
            rig, unique = wa.shared_lines(lines)
            self.s.rig[station] = rig
            if not quiet:
                print("   %s  spokes %s" % (station, sorted(by_spoke)))
                print("     rig (same line under >= 2 spokes, %d%s): %s" % (
                    len(rig), ", first 40" if len(rig) > 40 else "", ", ".join("%.0f" % f for f, _ in rig[:40])))
                for sp, fs in sorted(unique.items()):
                    inb = [f for f in fs if wa.F1_BAND[0] <= f <= wa.F1_BAND[1]]
                    print("     sp%-2d unique: %s   (in 350-600: %s)" % (sp, ", ".join("%.0f" % f for f in fs) or "-",
                                                                         ", ".join("%.1f" % f for f in inb) or "-"))
        self.s.save()

    def do_lines(self, arg):
        """lines: the 09-21 shared-line check over this epoch's strikes, per station (local mic, >=10 dB over the
        epoch floor in >=80% of a spoke's strikes; lines within 6 Hz on >=2 spokes = rig)."""
        self._refresh_rig(quiet=False)

    def do_repeat(self, arg):
        """repeat: per (spoke, excitation, pulse) in this epoch -- board f1 over its clears, and the focus-line and
        top-line frequency spread over all shots."""
        groups = {}
        for s in self.s.epoch_shots(kinds=("strike", "pluck", "air")):
            groups.setdefault((s["spoke"], s["kind"], s.get("board", {}).get("pulse_ms")), []).append(s)
        for (sp, kind, pulse), shots in sorted(groups.items(), key=lambda kv: str(kv[0])):
            clears = [s["board"]["expect_f1_hz"] for s in shots if s["board"].get("status") == "suspect"]
            r_board = wa.repeatability(clears)
            tops = [s["lines_local"][0][0] if s.get("lines_local") else None for s in shots]
            r_top = wa.repeatability(tops)
            print("   sp%-2d %-6s pulse %-6s n=%-3d clears %d/%d  board f1 %s  top line %s" % (
                sp, kind, pulse, len(shots), len(clears), len(shots), self._rep(r_board), self._rep(r_top)))

    @staticmethod
    def _rep(r):
        if not r:
            return "-"
        return "%.1f Hz (spread %.1f, %d/%d within +-%.0f)" % (r["median_hz"], r["spread_hz"], r["within_tol"], r["n"], r["tol_hz"])

    def do_loc(self, arg):
        """loc [f]: localization table for this epoch's shots (at f if given, else each shot's focus)."""
        f = float(arg) if arg.strip() else None
        floors = self.s.floors()
        for s in self.s.epoch_shots():
            meta, _, far, m, _ = self.s.metrics(s, floors, f)
            L = m["loc"]
            if L is None:
                continue
            print("   #%-3d %-6s sp%-2d %-5s focus %7.1f  delta %6s  rig %6s  INDEX %6s (%s)" % (
                s["id"], s["kind"], s["spoke"], s["station"], L["focus_hz"], fmt(L["delta_db"]), fmt(L["delta_rig_db"]),
                fmt(L["index_db"], "%+.1f"), L["reference"]))

    def do_board(self, arg):
        """board: the board's verdicts in this epoch by kind; clears on ctrl/air are FALSE clears."""
        by = {}
        for s in self.s.epoch_shots():
            by.setdefault(s["kind"], []).append(s)
        for kind, shots in sorted(by.items()):
            clears = [s for s in shots if s.get("board", {}).get("status") == "suspect"]
            reasons = {}
            for s in shots:
                reasons[s["board"].get("reason")] = reasons.get(s["board"].get("reason"), 0) + 1
            tag = "  <-- FALSE CLEARS" if clears and kind in ("ctrl", "air") else ""
            print("   %-6s n=%-3d clears %d  %s%s" % (kind, len(shots), len(clears),
                                                   ", ".join("%s %d" % kv for kv in sorted(reasons.items(), key=str)), tag))

    # ---- 4 adjust
    def do_decide(self, arg):
        """decide keep|reject|ambiguous <why>: stamp this epoch's configuration."""
        parts = arg.split(None, 1)
        if not parts or parts[0] not in ("keep", "reject", "ambiguous"):
            raise ValueError("decide keep|reject|ambiguous <why>")
        self.s.epoch["decision"] = parts[0]
        self.s.epoch["decision_why"] = parts[1] if len(parts) > 1 else ""
        self.s.epoch["decided_at"] = now()
        self.s.save()

    def do_note(self, arg):
        """note <text>: timestamped, attached to this epoch."""
        self.s.notes.append({"t": now(), "epoch": self.s.epoch["id"], "text": arg})
        self.s.save()

    def do_import(self, arg):
        """import <bundle dir or glob> [kind]: register existing bundles as shots of THIS epoch, for re-analysis.
        kind defaults from the bundle (fired -> strike, label with 'pluck' -> pluck, else ctrl)."""
        parts = split_args(arg)
        pattern = parts[0]
        if os.path.isdir(pattern):
            pattern = os.path.join(pattern, "*.json")
        paths = sorted(p for p in glob.glob(pattern) if not p.endswith(".observed.json"))
        for p in paths:
            meta, _, _ = wa.load_bundle(p)
            kind = parts[1] if len(parts) > 1 else (
                "strike" if meta.get("fired") else ("pluck" if "pluck" in (meta.get("campaign_label") or "").lower() else "ctrl"))
            spoke = int(meta.get("spoke_id", 0))
            rec = {"id": len(self.s.shots) + 1, "t": now(), "epoch": self.s.epoch["id"], "kind": kind, "spoke": spoke,
                   "station": cr.station_for_spoke(spoke), "pulse_req": meta.get("pulse_ms"),
                   "name": os.path.basename(p)[:-5], "json": os.path.abspath(p), "physical": dict(self.s.physical),
                   "flags": [], "imported": True}
            self.s.firmware_check(meta)
            self._observe(rec, header="imported #%d" % rec["id"], show=False)
            self.s.append_shot(rec)
        print("   %d bundle(s) imported into %s" % (len(paths), self.s.epoch["id"]))

    def do_report(self, arg):
        """report: session report.md + shots.csv + epochs.csv in the session directory."""
        p = write_report(self)
        print("   " + p)

    def do_quit(self, arg):
        """quit"""
        if not self.offline:
            try:                                          # leaving the coils energised is a choice, not an accident
                d = board_drive_state(self.s.knobs["host"], 3.0)
                if d is not None and d["enabled"]:
                    print("   note: the roller is still ENERGISED (`roll off` releases it; the board keeps holding).")
            except (OSError, ValueError):
                pass
        if self.ws is not None:
            self.ws.close()
        self.s.save()
        return True

    do_exit = do_quit
    do_EOF = do_quit


# ------------------------------------------------------------------ report

def write_report(wb):
    s = wb.s
    d = s.dir
    shots_csv = os.path.join(d, "shots.csv")
    cols = ["id", "t", "epoch", "kind", "spoke", "station", "roller", "pulse_req", "status", "reason", "f1_hz", "snr_db",
            "pulse_ms", "mic_input", "local_peak_dbfs", "local_rail", "local_in_band_over_floor_db",
            "local_wide_over_floor_db", "far_in_band_over_floor_db", "focus_hz", "focus_source", "delta_db",
            "delta_rig_db", "loc_index_db", "loc_reference", "top_local_lines", "flags", "name"]
    with open(shots_csv, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(cols)
        for r in s.shots:
            b, m, L = r.get("board", {}), r.get("m", {}), r.get("loc") or {}
            w.writerow([r["id"], r["t"], r["epoch"], r["kind"], r["spoke"], r["station"], r.get("roller"), r.get("pulse_req"),
                        b.get("status"), b.get("reason"), b.get("expect_f1_hz"), b.get("expect_snr_db"), b.get("pulse_ms"),
                        b.get("mic_input"), m.get("local", {}).get("peak_dbfs"), m.get("local", {}).get("rail_hits"),
                        m.get("local", {}).get("in_band_over_floor_db"), m.get("local", {}).get("wide_over_floor_db"),
                        m.get("far", {}).get("in_band_over_floor_db"), r.get("focus_hz"), r.get("focus_source"),
                        L.get("delta_db"), L.get("delta_rig_db"), L.get("index_db"), L.get("reference"),
                        " ".join("%.1f" % ln[0] for ln in r.get("lines_local", [])), "; ".join(r.get("flags", [])), r["name"]])
    epochs_csv = os.path.join(d, "epochs.csv")
    with open(epochs_csv, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["epoch", "started", "why", "physical", "firmware", "n_shots", "decision", "decision_why"])
        for e in s.epochs:
            w.writerow([e["id"], e["started"], e["why"], json.dumps(e["physical"]), json.dumps(e["firmware"]),
                        len(s.epoch_shots(e["id"])), e["decision"], e["decision_why"]])

    refs = []
    for rp in s.refs:
        meta, local, _ = wa.load_bundle(rp)
        refs.append((meta, wa.shot_metrics(meta, local, None, excited=ref_excited(meta))))

    L = []
    L.append("# Workbench session `%s`\n" % s.tag)
    L.append("> **EXPLORATORY, not campaign data.** No plan, no trial numbers, no fixed n, no exclusion ledger. "
             "Readings here are hypotheses for a later registered run, and no number below is an acceptance "
             "threshold. `local` is the struck station's mic (the only channel the board's DSP analysed); `far` is "
             "the other station's mic over the same frames.\n")
    L.append("Directory: `%s`  \nGenerated: %s  \nShots: %d in %d epoch(s)\n" % (os.path.relpath(d, REPO), now(),
                                                                              len(s.shots), len(s.epochs)))
    L.append("## Configurations tried\n")
    L.append("| epoch | started | why | physical set-up | firmware | shots | decision |")
    L.append("|---|---|---|---|---|---|---|")
    for e in s.epochs:
        fw = e["firmware"] or {}
        L.append("| %s | %s | %s | %s | %s %s | %d | %s |" % (
            e["id"], e["started"], e["why"], ", ".join("%s=%s" % kv for kv in sorted(e["physical"].items())) or "-",
            fw.get("build", "-"), fw.get("chain_digest", ""), len(s.epoch_shots(e["id"])),
            ("**%s** — %s" % (e["decision"], e["decision_why"])) if e["decision"] else "undecided"))
    L.append("")
    for e in s.epochs:
        shots = s.epoch_shots(e["id"])
        if not shots:
            continue
        L.append("## %s — %s\n" % (e["id"], ", ".join("%s=%s" % kv for kv in sorted(e["physical"].items())) or "no physical keys recorded"))
        by = {}
        for r in shots:
            by.setdefault(r["kind"], []).append(r)
        L.append("**Board verdicts** (DSP unchanged, local mic only):\n")
        L.append("| kind | n | clears | reasons |")
        L.append("|---|---|---|---|")
        for kind, rs in sorted(by.items()):
            cl = sum(1 for r in rs if r.get("board", {}).get("status") == "suspect")
            reasons = {}
            for r in rs:
                reasons[r["board"].get("reason")] = reasons.get(r["board"].get("reason"), 0) + 1
            L.append("| %s | %d | %d%s | %s |" % (kind, len(rs), cl, " **false clears**" if cl and kind in ("ctrl", "air") else "",
                                               ", ".join("%s %d" % kv for kv in sorted(reasons.items(), key=str))))
        L.append("")
        groups = {}
        for r in shots:
            if r["kind"] in ("strike", "pluck", "air"):
                groups.setdefault((r["spoke"], r["kind"], r.get("board", {}).get("pulse_ms")), []).append(r)
        if groups:
            L.append("**Per spoke / excitation / pulse** (medians; spreads are max-min):\n")
            L.append("| spoke | kind | pulse | n | clears | board f1 | top local line | in-band over floor (local / far) | delta at focus | loc. index |")
            L.append("|---|---|---|---|---|---|---|---|---|---|")
            for (sp, kind, pulse), rs in sorted(groups.items(), key=lambda kv: str(kv[0])):
                cl = [r["board"]["expect_f1_hz"] for r in rs if r["board"].get("status") == "suspect"]
                rb, rt = wa.repeatability(cl), wa.repeatability([r["lines_local"][0][0] if r.get("lines_local") else None for r in rs])

                def med(key, ch):
                    v = [r.get("m", {}).get(ch, {}).get(key) for r in rs]
                    v = [x for x in v if isinstance(x, (int, float)) and np.isfinite(x)]
                    return float(np.median(v)) if v else None

                def medloc(key):
                    v = [(r.get("loc") or {}).get(key) for r in rs]
                    v = [x for x in v if isinstance(x, (int, float)) and np.isfinite(x)]
                    return float(np.median(v)) if v else None
                L.append("| %s | %s | %s | %d | %d | %s | %s | %s / %s | %s | %s |" % (
                    sp, kind, pulse, len(rs), len(cl),
                    ("%.1f (±%.1f)" % (rb["median_hz"], rb["spread_hz"] / 2)) if rb else "-",
                    ("%.1f (spread %.1f)" % (rt["median_hz"], rt["spread_hz"])) if rt else "-",
                    fmt(med("in_band_over_floor_db", "local")), fmt(med("in_band_over_floor_db", "far")),
                    fmt(medloc("delta_db")), fmt(medloc("index_db"), "%+.1f")))
            L.append("")
            if refs:
                L.append("**Against the references** (a peak within ±%.0f Hz of the reference's board f1 standing "
                         "≥%.0f dB over its 60 Hz neighbourhood in the local mic -- the firmware's own prominence "
                         "criterion; only shots on the reference's own spoke):\n" % (wa.LINE_TOL_HZ, wa.PRESENCE_PROM_DB))
                n_rows = 0
                for meta, m in refs:
                    f0 = meta.get("expect_f1_hz")
                    if not isinstance(f0, (int, float)):
                        continue
                    for (sp, kind, pulse), rs in sorted(groups.items(), key=lambda kv: str(kv[0])):
                        if sp != meta.get("spoke_id"):
                            continue
                        pres = [wa.line_presence(*s.metrics(r)[3]["_spectra"]["local"], f0) for r in rs]
                        hit = sum(1 for p in pres if p[0])
                        proms = [p[1] for p in pres if np.isfinite(p[1])]
                        L.append("- ref `%s` (f1 %.1f Hz, %s): sp%s %s pulse %s — present in %d/%d shots, median "
                                 "prominence there %s dB" % (meta.get("name"), f0, meta.get("status"), sp, kind, pulse,
                                                            hit, len(rs), fmt(float(np.median(proms)) if proms else None)))
                        n_rows += 1
                if not n_rows:
                    L.append("- no shots in this epoch on a reference's spoke (%s)" % ", ".join(
                        sorted({"sp%s" % meta.get("spoke_id") for meta, _ in refs})))
                L.append("")
        figs = [r for r in shots if r.get("png")][-3:]
        if figs:
            L.append("Figures (last %d of this epoch): %s\n" % (len(figs), ", ".join(
                "[#%d](%s)" % (r["id"], os.path.basename(r["png"])) for r in figs)))
        notes = [n for n in s.notes if n["epoch"] == e["id"]]
        if notes:
            L.append("Notes:\n")
            for n in notes:
                L.append("- %s — %s" % (n["t"], n["text"]))
            L.append("")
    L.append("## Shared rig lines (last `lines` run, per station)\n")
    for st, rig in sorted(s.rig.items()):
        L.append("- %s: %d line(s): %s" % (st, len(rig), ", ".join("%.0f Hz" % f for f, _ in rig) or "-"))
    L.append("")
    L.append("## References\n")
    for meta, m in refs:
        L.append("- `%s` — sp%s %s, fired=%s, %s/%s, f1 %s Hz, SNR %s dB, chain `%s`. Taken with the old single mic at "
                 "its old position: frequencies and shape compare, absolute levels do not." % (
                     meta.get("name"), meta.get("spoke_id"), meta.get("station"), meta.get("fired"), meta.get("status"),
                     meta.get("reason"), fmt(meta.get("expect_f1_hz")), fmt(meta.get("expect_snr_db")),
                     (meta.get("chain_digest") or "")[:12]))
    L.append("\nFull per-shot data: `shots.csv`; configurations: `epochs.csv`.\n")
    p = os.path.join(d, "report.md")
    with open(p, "w", encoding="utf-8") as f:
        f.write("\n".join(L))
    return p


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--session", help="session tag; an existing session with this tag is resumed")
    ap.add_argument("--heartbeat", nargs="?", const=0, type=int, metavar="N",
                    help="live check of both mics and exit (N beats; default: until Ctrl+C). Needs no session.")
    ap.add_argument("--offline", action="store_true", help="no board: references, import, analysis and report only")
    ap.add_argument("--host", default=cr.DEFAULT_HOST)
    ap.add_argument("-c", "--command", action="append", help="run these commands and exit (scripting/tests)")
    a = ap.parse_args(argv)
    if a.heartbeat is not None:
        heartbeat(a.host, SHOT_KEYS["timeout"], a.heartbeat or None)
        return 0
    if not a.session:
        ap.error("give --session <tag> for the workbench, or --heartbeat for a quick mic check")
    s = Session(a.session)
    s.knobs["host"] = a.host
    if not a.offline:
        ensure_board(a.host)
    wb = Workbench(s, a.offline)
    if a.command:
        for c in a.command:
            print("wb> " + c)
            wb.onecmd(c)
        s.save()
        return 0
    wb.cmdloop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
