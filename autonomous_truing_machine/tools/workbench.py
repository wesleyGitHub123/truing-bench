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

try:
    import msvcrt                                 # Windows only: a key press during `roll <steps>` marks a step count
except ImportError:
    msvcrt = None

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
LASER-STOP (lab)   roll speed <v> <a> ; laser trial <steps> [n]  (arm -> move -> the beam break requests the drive's
                   controlled stop -> report -> a row in laser_trials.csv) ; change v or a ; repeat ; laser log.
                   `laser scan <steps>` first, disarmed, shows how many steps the beam stays blocked per spoke: a
                   stop lands inside the beam only when the overshoot is shorter than that.

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
    if d.get("mres_ok") is False:
        flags.append("MICROSTEPS REVERTED: every step count is off-scale until `roll on` rewrites them")
    if d.get("mres_repairs"):
        flags.append("microsteps rewritten %d time(s) since boot: the driver reset" % d["mres_repairs"])
    drv = ("  driver I %2d/31 %s" % (d.get("cs_actual", 0), d.get("drv_flags", "ok"))) if d.get("drv_ok") else \
          ("  driver UART silent" if "drv_ok" in d else "")
    return "  roller                 %-8s %-6s pos %7d steps  %5d steps/s  %6d steps/s2%s%s" % (
        "ENERGISED" if d.get("enabled") else "off", "MOVING" if d.get("moving") else "idle", d.get("position_steps", 0),
        d.get("max_step_hz", 0), d.get("accel_steps_s2", 0), drv, "  !! " + ", ".join(flags) if flags else "")


SEVERE_FLAGS = {"OT", "S2GA", "S2GB", "S2VSA", "S2VSB"}   # driver shut down or about to: stop and look
WARN_FLAGS = {"OTPW", "OLA", "OLB"}                        # pre-warning / open load: watch, may be transient


def roller_beat_line(d, hist):
    """One short line for the mic heartbeat: what matters about the roller and nothing else. Healthy is quiet
    (`OK`); a warning or a critical state says so first and names the cause. `hist` (a dict the caller keeps)
    counts each driver flag seen so far, because the board reads them live and a transient between beats
    would otherwise vanish: `seen OLB x2` stays on the line."""
    import re
    state = "%s %s" % ("ON " if d.get("enabled") else "off", "MOVING" if d.get("moving") else "idle")
    pos = "pos %d" % d.get("position_steps", 0)
    severe, warn, notes = [], [], []
    if d.get("faulted"):
        severe.append("FAULT LATCHED 0x%x" % d.get("fault_detail", 0))
    if "drv_ok" not in d:
        cur = ""
    elif d["drv_ok"]:
        cur = "I %d/31" % d.get("cs_actual", 0)
        flags = set(re.findall(r"[A-Z0-9]+", d.get("drv_flags", "ok"))) - {"ok", "OK"}
        for f in sorted(flags):
            hist[f] = hist.get(f, 0) + 1
        severe += ["driver " + f for f in sorted(flags & SEVERE_FLAGS)]
        warn += ["driver " + f for f in sorted(flags & WARN_FLAGS)]
        if d.get("gstat", 0) & 0x6:
            severe.append("driver latched GSTAT 0x%x (drv_err/uv_cp)" % d["gstat"])
        if hist.pop("_uart_lost", None):
            notes.append("UART back")
        hist["_uart_ok"] = True
    else:
        cur = "UART silent"
        if hist.get("_uart_ok"):                         # it answered before and stopped: that is news
            severe.append("driver UART lost")
            hist["_uart_lost"] = True
    if d.get("diag_high"):
        warn.append("DIAG high")
    seen = ["%s x%d" % (f, n) for f, n in sorted(hist.items()) if not f.startswith("_")]
    level = ("!! CRITICAL" if severe else "!  WARN    " if warn else
             "no driver  " if cur == "UART silent" else "OK         ")   # never answered: nothing to call OK
    tail = ", ".join(severe + warn + notes)
    return "  roller  %s %s %s %s%s%s" % (level, state, pos, cur, "  <- " + tail if tail else "",
                                          "   (seen " + ", ".join(seen) + ")" if seen else "")


# ------------------------------------------------------------------ the laser receiver and laser-stop (lab image)
# GET /debug/laser.json (src/laser_bench.c): the break-beam receiver on GPIO 7 and the optional one-shot trigger
# that asks the bench drive for its own controlled stop on a beam break. Step counts are the drive's pulse
# counter (commanded, never proof the wheel followed); the beam level at the end is the physical observation.

LASER_FIELDS = ["n", "time", "trial", "speed_steps_s", "accel_steps_s2", "move_steps", "armed_blocked",
                "trigger_step", "final_step", "overshoot_steps", "ramp_ideal_steps", "stop_ms", "beam_at_stop",
                "beam_settled", "left_beam_after_steps", "edges_after_trigger", "glitches_total", "result", "driver",
                "note"]
LASER_DARK_S = 10.0     # "no laser seen" for this long while the receiver is driven: blocked, off or misaligned


def board_laser_state(host, timeout, query=""):
    """The lab image's laser receiver as a dict (GET /debug/laser.json), or None when this image has none."""
    try:
        raw, _ = cf.get("http://%s/debug/laser.json%s" % (host, "?" + query if query else ""), timeout)
    except urllib.error.HTTPError as e:
        if e.code == 404:
            return None
        raise
    return json.loads(raw.decode("utf-8"))


def ramp_stop_steps(hz, accel):
    """Pulses the firmware's ramp spends stopping FROM CRUISE (lib/truing_hal/src/step_ramp.c): about v^2/2a, or a
    single pulse when cruise is slow enough to start at (no acceleration phase). A trigger caught while still
    accelerating stops in fewer. The ramp's arithmetic, not a measurement: the trials measure what really happens."""
    if not hz or not accel or hz <= 0 or accel <= 0:
        return None
    if 0.676 * 1e6 * (2.0 / accel) ** 0.5 <= 1e6 / hz:
        return 1
    return int(hz * hz / (2.0 * accel))


def laser_beat_line(z, hist):
    """One line for the heartbeat, in the roller line's manner: healthy is quiet. `hist` (kept by the caller)
    remembers the glitch count so a burst of chatter between beats is flagged once."""
    beam = z.get("beam", "?")
    age = z.get("last_edge_age_ms")
    warn, notes = [], []
    glitches = int(z.get("glitches", 0))
    new_glitch = glitches - hist.get("glitches", glitches)
    hist["glitches"] = glitches
    if z.get("input") == "floating":
        level = "!  NO INPUT "
        warn.append("GPIO %s floats: the receiver's OUT is not reaching it" % z.get("gpio", "?"))
    elif new_glitch > 0:
        level = "!  CHATTER  "
        warn.append("%d glitch%s since the last beat (pulses shorter than the interrupt can follow)" %
                    (new_glitch, "es" if new_glitch > 1 else ""))
    elif beam == "BLOCKED" and (age is None or age > LASER_DARK_S * 1000):
        level = "DARK       "
        notes.append("no laser seen for %s: blocked, laser off, misaligned or receiver unpowered" %
                     ("ever" if age is None else "%.0f s" % (age / 1000.0)))
    else:
        level = "OK         "
    armed = ("ARMED #%d" % z["trial"]["trial"]) if z.get("armed") and z.get("trial") else "disarmed"
    last = z.get("last")
    tail_last = ""
    if last:
        tail_last = "  last #%d %s" % (last["trial"], last["result"])
        if last.get("fired"):
            tail_last += " %+d" % (last["final_pos"] - last["trigger_pos"])
    since = "never changed" if age is None else "changed %.1f s ago" % (age / 1000.0)
    tail = ", ".join(warn + notes)
    return "  laser   %s %-7s raw %s  edges %d  %s  %s%s%s" % (
        level, beam, z.get("level", "?"), int(z.get("edges", 0)), since, armed, tail_last,
        "  <- " + tail if tail else "")


def laser_trial_row(t, settled, move_steps, glitches, driver, note=""):
    """One finished trial (the board's `last` record) as a table/CSV row. Overshoot is signed along the move:
    positive = pulses sent past the trigger in the direction of travel."""
    sign = 1 if move_steps >= 0 else -1
    fired = bool(t.get("fired"))
    hz, acc = int(t.get("max_step_hz") or 0), int(t.get("accel_steps_s2") or 0)
    return {
        "n": "", "time": now(), "trial": t.get("trial"), "speed_steps_s": hz, "accel_steps_s2": acc,
        "move_steps": move_steps, "armed_blocked": int(bool(t.get("blocked_at_arm"))),
        "trigger_step": t.get("trigger_pos") if fired else "",
        "final_step": t.get("final_pos"),
        "overshoot_steps": sign * (t["final_pos"] - t["trigger_pos"]) if fired else "",
        "ramp_ideal_steps": ramp_stop_steps(hz, acc) if fired else "",
        "stop_ms": round(t.get("stop_us", 0) / 1000.0, 1) if fired else "",
        "beam_at_stop": ("BLOCKED" if t.get("blocked_at_end") else "CLEAR") if fired else "",
        "beam_settled": settled or "",
        "left_beam_after_steps": sign * (t["leave_pos"] - t["trigger_pos"]) if fired and t.get("left_beam") else "",
        "edges_after_trigger": t.get("edges_after", 0) if fired else "",
        "glitches_total": glitches, "result": t.get("result", "?"), "driver": driver or "", "note": note,
    }


def laser_trial_report(row):
    """Plain words for one trial, printed right after it."""
    head = "   LASER TRIAL %s  %s   %s steps/s, %s steps/s2" % (row["trial"], row["result"], row["speed_steps_s"],
                                                              row["accel_steps_s2"])
    if row["result"] == "NO_TRIGGER":
        return head + "\n     the move ended at step %s without a beam break" % row["final_step"]
    lines = [head,
             "     beam break at step %s -> last pulse at step %s: %+d steps after the trigger (ramp from cruise: ~%s), "
             "%s ms (+-2 ms)" % (row["trigger_step"], row["final_step"], row["overshoot_steps"],
                                 row["ramp_ideal_steps"], row["stop_ms"]),
             "     beam at the last pulse: %s   0.5 s later: %s   edges after the trigger: %s%s" % (
                 row["beam_at_stop"], row["beam_settled"] or "?", row["edges_after_trigger"],
                 ("   (it cleared %+d steps after the trigger)" % row["left_beam_after_steps"])
                 if row["left_beam_after_steps"] != "" else "")]
    if row["armed_blocked"]:
        lines.append("     (armed with the beam already blocked: this was the NEXT break)")
    if row["result"] == "BLOCKED_AGAIN":
        lines.append("     the beam cleared after the break and was blocked again at the stop: chatter at the spoke's edge, "
                     "or a LATER spoke -- compare with `laser scan`'s blocked width and spacing")
    if row["result"] == "INTERRUPTED":
        lines.append("     the coils were released or the driver faulted before the controlled stop finished")
    if row.get("note"):
        lines.append("     note: %s" % row["note"])
    return "\n".join(lines)


def laser_table(rows):
    """The session's trials side by side, for comparing speed/deceleration settings."""
    out = ["   %4s %5s %6s %7s %10s %10s %9s %6s %8s %8s %8s  %s" % (
        "n", "trial", "speed", "accel", "trigger", "final", "overshoot", "ideal", "stop_ms", "@stop", "settled",
        "result")]
    for r in rows:
        out.append("   %4s %5s %6s %7s %10s %10s %9s %6s %8s %8s %8s  %s" % (
            r.get("n", ""), r.get("trial", ""), r.get("speed_steps_s", ""), r.get("accel_steps_s2", ""),
            r.get("trigger_step", ""), r.get("final_step", ""), r.get("overshoot_steps", ""),
            r.get("ramp_ideal_steps", ""), r.get("stop_ms", ""), r.get("beam_at_stop", ""),
            r.get("beam_settled", ""), r.get("result", "")))
    return "\n".join(out)


def laser_edge_line(e, prev):
    """One beam edge for `laser watch` / `laser scan`, with the distance from the edges before it: a break to
    the following clear is how long the beam stayed blocked; break to break is the spacing. `prev` (kept by the
    caller) holds the last edge of each kind."""
    extra = []
    if e["edge"] == "LEAVE" and "ENTER" in prev:
        b = prev["ENTER"]
        extra.append("blocked for %+d steps, %.0f ms" % (e["pos"] - b["pos"], ((e["t_us"] - b["t_us"]) % 2 ** 32) / 1000.0))
    if e["edge"] == "ENTER" and "ENTER" in prev:
        extra.append("%+d steps since the previous break" % (e["pos"] - prev["ENTER"]["pos"]))
    prev[e["edge"]] = e
    return "   %s at step %7d  %s%s" % ("BREAK (beam blocked)" if e["edge"] == "ENTER" else "CLEAR (beam back)   ",
                                      e["pos"], "moving" if e.get("moving") else "still ",
                                      "   " + "; ".join(extra) if extra else "")


def laser_scan_summary(edges):
    """What a disarmed pass saw: blocked widths (break -> next clear) and break-to-break spacing, in steps."""
    widths, spacing, last_enter, open_break = [], [], None, False
    for e in edges:
        if e["edge"] == "ENTER":
            if last_enter is not None:
                spacing.append(e["pos"] - last_enter["pos"])
            last_enter, open_break = e, True
        elif open_break:                                  # a clear pairs with the break before it, once
            widths.append(e["pos"] - last_enter["pos"])
            open_break = False
    return widths, spacing


LASER_USAGE = ("laser [status | arm | disarm | probe | watch [s] | scan <+/-steps> | trial <+/-steps> [n] | log]")


def parse_laser(arg):
    """('status',) | ('arm',) | ('disarm',) | ('probe',) | ('log',) | ('watch', s|None) | ('scan', steps)
    | ('trial', steps, n)."""
    t = arg.split()
    if not t:
        return ("status",)
    head = t[0].lower()
    try:
        if head in ("status", "arm", "disarm", "probe", "log") and len(t) == 1:
            return (head,)
        if head == "watch" and len(t) <= 2:
            return ("watch", float(t[1]) if len(t) == 2 else None)
        if head == "scan" and len(t) == 2:
            return ("scan", cr.drive_move_arg(int(t[1])))
        if head == "trial" and len(t) in (2, 3):
            n = int(t[2]) if len(t) == 3 else 1
            if not 1 <= n <= 100:
                raise ValueError("laser trial <steps> [n]: n is 1..100")
            return ("trial", cr.drive_move_arg(int(t[1])), n)
    except ValueError as e:
        if "n is" in str(e) or "a move is" in str(e):
            raise
        raise ValueError(LASER_USAGE) from None
    raise ValueError(LASER_USAGE)


# ------------------------------------------------------------------ the alignment campaign (lab image)
# GET /debug/align.json (src/laser_bench.c + lib/truing_hal/src/beam_align.c): the REAL-TIME controller on the
# board -- count beam breaks, request the drive's controlled stop on the Nth from the beam interrupt, then a
# correction strategy -- and its run record. The host only configures, sequences, plucks and logs.
# docs/ALIGN_CAMPAIGN.md is the campaign's plan and its verdict rules.

# Spoke class by machine spoke index: the shipped artifact's Phi_t_class_map (model_prep conventions.py
# BWC_CLASS_BY_INDEX_MOD_4, indexing origin = spoke 0 is NDS leading, SPEC 6.5). The PHYSICAL spoke 0 must be the
# NDS-leading spoke the wheel declares; the station is the acoustic subsystem's parity rule (even LEFT, odd RIGHT).
SPOKE_CLASS_BY_MOD4 = {0: "NDS-leading", 1: "DS-leading", 2: "NDS-trailing", 3: "DS-trailing"}
CLASS_NAMES = {c.lower().replace("-", ""): c for c in SPOKE_CLASS_BY_MOD4.values()}
STRATEGIES = ("STOP", "REVERSE_FIXED", "RETURN_TO_EDGE", "REACQUIRE", "REACQUIRE_CLEAR")
# Campaign parameters, kept per session. None = not chosen yet: a run that needs it refuses and says so. None of
# them is a calibration; `pitch`/`width`/`rev_steps_measured` are filled only by a stage-0 count run.
ALIGN_DEFAULTS = {"dir": 1, "index_dir": None, "spokes": 32, "settle_ms": 300, "slow_hz": None, "slow_accel": None,
                  "rev_steps": None, "offset": 0, "center": False, "rmax": None, "max": None, "pluck": "aligned",
                  "plucks": 1, "pluckmode": "capture", "pgap": 0,
                  "beam_at_station": None, "pitch": None, "width": None, "rev_steps_measured": None,
                  "measured_by": None, "belief": None}
ALIGN_FIELDS = ["n", "time", "stage", "class", "spoke", "station", "strategy", "run", "dir", "speed", "accel",
                "slow_hz", "slow_accel", "rev_steps", "offset", "center", "settle_ms", "target_break", "start_pos",
                "start_blocked", "trigger_pos", "trigger_t_us", "stop_pos", "stop_t_us", "stop_ms", "overshoot_steps",
                "zone_after_stop", "blocked_after_stop", "width_used", "pitch_med", "corr_cmd", "reacquisitions",
                "reacq_pos", "reacq_offset", "same_spoke", "trim_cmd", "final_pos", "final_error_steps",
                "final_zone", "final_blocked", "edges_still", "fw_result", "fw_detail", "pluck", "pluck_n", "pluck_hits",
                "pluck_dsp_ok", "pluck_moved_spoke",
                "acoustic_status", "acoustic_reason", "acoustic_f1_hz", "capture_result", "shot", "verdict",
                "alignment_reference"]
COUNT_FIELDS = ["n", "time", "run", "speed", "accel", "dir", "expected_breaks", "start_blocked", "breaks",
                "missed", "duplicates", "glitches", "elapsed_s", "start_pos", "trigger_pos", "final_pos",
                "overshoot_steps", "pitch_med", "width_med", "rev_steps", "return_error_steps", "final_blocked",
                "fw_result", "verdict"]


def spoke_class(idx):
    return SPOKE_CLASS_BY_MOD4[idx % 4]


def parse_class(tok):
    c = CLASS_NAMES.get(tok.lower().replace("-", "").replace("_", ""))
    if c is None:
        raise ValueError("spoke class is one of %s" % ", ".join(SPOKE_CLASS_BY_MOD4.values()))
    return c


def reconcile_belief(belief, want_blocked, now_blocked, index_dir, n_spokes):
    """(belief, expected beam level) for the next trial, given the beam level now. `want_blocked` is what the last
    trial left (None = a spoke the operator declared: no expectation). A beam BLOCKED where the last trial left it
    clear means the next spoke ahead has already entered it: the belief moves on one spoke. A beam CLEAR where the
    last trial left a spoke IN it means the strikes pushed that spoke on past the beam (every nudge seen on this
    wheel, 17 of 129 trials, moved a spoke FORWARD, toward and through the beam, never back): the belief stays, the
    spoke is now behind, and the next break is the next spoke. That is an assumption, not an observation; the
    caller's trigger-spacing check is what catches it if the spoke went the other way."""
    if want_blocked is None or now_blocked == want_blocked:
        return belief, (now_blocked if want_blocked is not None else None)
    if now_blocked:
        return (belief + index_dir) % n_spokes, True
    return belief, False


def board_align_state(host, timeout, query=""):
    """The board's alignment controller as a dict (GET /debug/align.json), or None when this image has none."""
    try:
        raw, _ = cf.get("http://%s/debug/align.json%s" % (host, "?" + query if query else ""), timeout)
    except urllib.error.HTTPError as e:
        if e.code == 404:
            return None
        raise
    return json.loads(raw.decode("utf-8"))


def _median(v):
    v = sorted(v)
    return v[len(v) // 2] if v else None


def count_analysis(edges, start_blocked, n_spokes):
    """Stage 0 from the run's own edge log: break-to-break spacing and blocked widths along the approach, the
    breaks a missed spoke would leave (a spacing near twice its phase's median) and extra ones (under half), the
    repeating spacing pattern, and -- when the run started just BEFORE spoke 0 -- the revolution in steps, edge to
    edge."""
    enters, widths, open_at = [], [], None
    for e in edges:
        if e["phase"] not in ("APPROACH",) or e["sign"] != 1:
            continue
        if e["enter"]:
            enters.append(e)
            open_at = e["pos"]
        elif open_at is not None:
            widths.append(abs(e["pos"] - open_at))
            open_at = None
    pitches = [abs(b["pos"] - a["pos"]) for a, b in zip(enters, enters[1:])]
    med_p, med_w = _median(pitches), _median(widths)
    # The beam sits where spokes cross, so the spacing it sees need not be uniform: the bench wheel gives a repeating
    # 4-spoke pattern. Judge each spacing against the median of its OWN phase in the shortest period that fits,
    # never against one global median (which called every short gap an extra break and every long one a missed one).
    period, phase_med = _spacing_period(pitches)
    missed = dups = 0
    for i, p in enumerate(pitches):
        ref = phase_med[i % period]
        if ref and p > 1.5 * ref:
            missed += max(0, int(round(p / ref)) - 1)
        elif ref and p < 0.5 * ref:
            dups += 1
    rev = abs(enters[n_spokes]["pos"] - enters[0]["pos"]) if not start_blocked and len(enters) > n_spokes else None
    return {"enters": enters, "pitches": pitches, "widths": widths, "pitch_med": med_p, "width_med": med_w,
            "pitch_min": min(pitches) if pitches else None, "period": period, "pattern": phase_med,
            "missed": missed, "duplicates": dups, "rev_steps": rev}


def _spacing_period(pitches, max_period=8, tol=0.10, floor=60):
    """(period, per-phase median spacings): the smallest period 1..max_period for which at most a tenth of the
    spacings differ from their phase's median by more than max(tol * median, floor steps). Period 1 is a uniform
    wheel. Nothing fits -> period 1 (the global median), so a genuinely irregular run still shows up as missed/extra."""
    for p in range(1, max_period + 1):
        if len(pitches) < 2 * p:
            break
        meds = [_median(pitches[k::p]) for k in range(p)]
        bad = sum(1 for i, g in enumerate(pitches) if abs(g - meds[i % p]) > max(tol * meds[i % p], floor))
        if bad <= 0.1 * len(pitches):
            return p, meds
    return 1, [_median(pitches)]


def align_event_line(ev, st, t0):
    """One controller phase change as a tagged live line ([LASER] / [ROLLER] / [REVERSE] / [ALIGN])."""
    ph, dt = ev["phase"], ((ev["t_us"] - t0) % 2 ** 32) / 1e6
    strat = st.get("cfg", {}).get("strategy")
    if ph == "APPROACH":
        msg = "[ROLLER] APPROACH at step %d (beam %s)" % (ev["pos"], "BLOCKED" if ev["blocked"] else "CLEAR")
    elif ph == "STOPPING":
        msg = "[LASER] TRIGGER break #%s at step %d  ->  [ROLLER] DECELERATING (controlled stop)" % (
            st.get("cfg", {}).get("target_break", "?"), ev["pos"])
    elif ph == "SETTLE":
        msg = "[ROLLER] STOPPED at step %d (%+d steps after the trigger), settling" % (
            ev["pos"], (ev["pos"] - st.get("trigger_pos", ev["pos"])) * st.get("cfg", {}).get("dir", 1))
    elif ph == "CORRECT":
        msg = {"REVERSE_FIXED": "[REVERSE] fixed reverse",
               "RETURN_TO_EDGE": "[REVERSE] step-based return to the trigger edge",
               "REACQUIRE": "[REVERSE] slow reverse until the laser re-finds the same spoke",
               "REACQUIRE_CLEAR": "[REVERSE] slow reverse through the spoke until the beam clears on its far side"}.get(strat, "[ROLLER] correction")
        msg += " (beam %s at step %d)" % ("BLOCKED" if ev["blocked"] else "CLEAR", ev["pos"])
    elif ph == "TRIM":
        msg = "[ALIGN] centering / offset move from step %d" % ev["pos"]
    elif ph == "FINAL_SETTLE":
        msg = "[ALIGN] CHECK at step %d (beam %s)" % (ev["pos"], "BLOCKED" if ev["blocked"] else "CLEAR")
    elif ph == "DONE":
        msg = "[ALIGN] %s%s" % (st.get("result"), " / " + st["result_detail"] if st.get("result_detail") else "")
    else:
        msg = ph
    return "   %6.3fs  %s" % (dt, msg)


def align_verdict(fw_result, plucked, pluck_ok, acoustic_ok):
    """The trial verdict: the controller's geometric result first, then the pluck, then the capture."""
    if fw_result != "ALIGNED":
        return fw_result
    if plucked and not pluck_ok:
        return "PLUCK_FAILED"
    if plucked and not acoustic_ok:
        return "ACOUSTIC_CAPTURE_FAILED"
    return "ALIGNMENT_SUCCESS"


def policy_key(r):
    """The parameters that define a policy for its strategy -- and only those: a STOP trial is not a different
    policy because `rev` happened to be set."""
    s = r["strategy"]
    k = [s, str(r["speed"]), str(r["accel"])]
    if s != "STOP":
        k += ["slow %s/%s" % (r["slow_hz"], r["slow_accel"])]
    if s == "REVERSE_FIXED":
        k += ["rev %s" % r["rev_steps"]]
    if s in ("RETURN_TO_EDGE", "REACQUIRE", "REACQUIRE_CLEAR"):
        k += ["center %s" % r["center"], "off %s" % r["offset"]]
    return tuple(k)


def policy_str(k):
    return "%s @ %s steps/s, %s steps/s2%s" % (k[0], k[1], k[2], (", " + ", ".join(k[3:])) if len(k) > 3 else "")


def align_groups(rows):
    """Trials grouped by (class, policy): one policy per group, in trial order."""
    g = {}
    for r in rows:
        g.setdefault((r["class"],) + policy_key(r), []).append(r)
    return g


ADOPT_RUN = 5   # consecutive ALIGNMENT_SUCCESS trials of one policy on one class before it is called a candidate


def adoption_status(trials):
    """'CANDIDATE' when the last ADOPT_RUN trials of a policy all succeeded, else what is missing."""
    tail = trials[-ADOPT_RUN:]
    ok = sum(1 for r in tail if r["verdict"] == "ALIGNMENT_SUCCESS")
    if len(tail) == ADOPT_RUN and ok == ADOPT_RUN:
        return "CANDIDATE"
    return "%d/%d of the last %d" % (ok, len(tail), ADOPT_RUN)


ALIGN_USAGE = ("align [status | set <key> <value> | spoke <k> | count <steps/s>... | trial <class> [n] [strategy] "
               "| campaign <class|all> | log | abort]")


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


HEARTBEAT_MS = 250            # audio per beat: the level, stuck-line and clipping tests do not depend on the length
HEARTBEAT_STATE_EVERY = 3     # the roller and laser lines are polled on the first beat and every 3rd after (~0.2 s each)

HEARTBEAT_HELP = """
MIC HEARTBEAT -- a quarter second of both mics from the live stream, a few beats a second, nothing saved.
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


def visible_board_keys():
    """{ssid: passphrase} for the truing boards in range. The firmware derives both from the softAP MAC
    (src/net_transport.c): ssid truing-<mac3..5>, passphrase truing-<mac2..5>, and the BSSID is that MAC."""
    import re
    import subprocess
    try:
        out = subprocess.run(["netsh", "wlan", "show", "networks", "mode=bssid"], capture_output=True, text=True,
                             timeout=15).stdout
    except (OSError, subprocess.SubprocessError):
        return {}
    keys = {}
    for block in re.split(r"(?m)^SSID \d+ :", out)[1:]:
        m = re.match(r"\s*(truing-[0-9a-f]{6})\b", block)
        b = re.search(r"BSSID \d+\s*:\s*((?:[0-9a-f]{2}:){5}[0-9a-f]{2})", block, re.I)
        if m and b:
            mac = b.group(1).lower().split(":")
            if "".join(mac[3:6]) == m.group(1)[-6:]:          # only when the BSSID really is the softAP MAC
                keys[m.group(1)] = "truing-" + "".join(mac[2:6])
    return keys


def save_board_profile(ssid, passphrase):
    """Add a Windows Wi-Fi profile for a board this PC has not met before. True if netsh took it."""
    import os
    import subprocess
    import tempfile
    from xml.sax.saxutils import escape
    xml = ('<?xml version="1.0"?><WLANProfile xmlns="http://www.microsoft.com/networking/WLAN/profile/v1">'
           "<name>%s</name><SSIDConfig><SSID><name>%s</name></SSID></SSIDConfig><connectionType>ESS</connectionType>"
           "<connectionMode>manual</connectionMode><MSM><security><authEncryption><authentication>WPA2PSK"
           "</authentication><encryption>AES</encryption><useOneX>false</useOneX></authEncryption><sharedKey>"
           "<keyType>passphrase</keyType><protected>false</protected><keyMaterial>%s</keyMaterial></sharedKey>"
           "</security></MSM></WLANProfile>") % (escape(ssid), escape(ssid), escape(passphrase))
    fd, path = tempfile.mkstemp(suffix=".xml")
    try:
        with os.fdopen(fd, "w") as f:
            f.write(xml)
        r = subprocess.run(["netsh", "wlan", "add", "profile", "filename=" + path], capture_output=True, text=True,
                           timeout=20)
        return r.returncode == 0
    except (OSError, subprocess.SubprocessError):
        return False
    finally:
        os.unlink(path)


def ensure_board(host, wait_s=90.0, say=print):
    """Get this PC talking to the board with no manual steps: if it does not answer, join its Wi-Fi
    (truing-xxxxxx) and wait for it to boot. A board this PC has never joined gets its profile made from
    the AP's own MAC. With several boards in range each is tried in turn. True once /id answers."""
    if board_answers(host):
        return True
    import subprocess
    t_end = time.time() + wait_s
    say("   the board is not answering: joining its Wi-Fi and waiting for it (a fresh boot takes ~30 s)")
    last_join, attempt = 0.0, 0
    while time.time() < t_end:
        if time.time() - last_join > 15.0:
            last_join = time.time()
            keys = visible_board_keys()
            names = [n for n in truing_networks() if n in keys] or truing_networks()   # in range first
            saved = subprocess.run(["netsh", "wlan", "show", "profiles"], capture_output=True, text=True).stdout
            for n in keys:
                if n not in saved and save_board_profile(n, keys[n]):
                    say("   new board %s: saved its Wi-Fi profile" % n)
            if names:
                name = names[attempt % len(names)]
                attempt += 1
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
    ws, seq, beat, peaks, tap_run, roller_hist, laser_hist = None, 0, 0, {}, {}, {}, {}
    quick = True                                          # the live stream; falls back to captures on an old image
    try:
        while beats is None or beat < beats:
            try:
                if quick:
                    chans = quick_channels(host, timeout, HEARTBEAT_MS)
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
            if (beat - 1) % HEARTBEAT_STATE_EVERY:
                continue      # the board serves one request at a time, so these polls cannot overlap the stream: skip most
            try:
                drv = board_drive_state(host, timeout)
            except (OSError, ValueError):
                drv = None
            if drv is not None:                           # the lab image: the roller's state beside the mics
                say(roller_beat_line(drv, roller_hist))
            try:                                          # probe=1: the firmware refuses it while armed or moving
                lz = board_laser_state(host, timeout, "probe=1")
            except (OSError, ValueError):
                lz = None
            if lz is not None:                            # a lab image with the laser receiver input
                say(laser_beat_line(lz, laser_hist))
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
  laser [...]      the laser receiver + laser-stop, lab image only: `laser`, `laser arm`, `laser trial 6000 5`
  align [...]      roller/laser/pluck ALIGNMENT campaign (real-time on the board): `align count 300`, `align campaign all`
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

    def _await_roller(self, steps, start=0):
        """Poll until the move ends, printing the live step count. Ctrl+C sends STOP (a controlled deceleration) and keeps waiting."""
        d = self._drive_state()
        hz = max(1, int(d.get("max_step_hz") or 1))
        deadline = time.time() + abs(steps) / hz * 2.0 + 5.0
        stopping = False
        t0, last, last_print = time.time(), None, 0.0
        prev, marks, presses, listening = None, [], [], threading.Event()

        def listen():
            """Stamp each key press the instant it happens, independent of the (slow) position polling."""
            while not listening.is_set():
                if msvcrt.kbhit():
                    if msvcrt.getwch() != "\x03":
                        presses.append(time.time())
                else:
                    time.sleep(0.002)

        def place(upto, p_now):
            """Turn every press stamped at or before `upto` into a step count, interpolated between the two
            position samples that bracket it. Pressed after the newest sample: wait for the next one."""
            while presses and presses[0] <= upto:
                tp = presses.pop(0)
                p1 = prev if prev is not None else (upto, p_now)
                frac = min(1.0, max(0.0, (tp - p1[0]) / (upto - p1[0]))) if upto > p1[0] else 1.0
                at = int(round(p1[1] + (p_now - p1[1]) * frac))
                marks.append(at)
                gap = "" if len(marks) == 1 else "   (%+d since the previous mark)" % (at - marks[-2])
                print("   MARK %d at pos %d = %+d steps from the move start%s" % (len(marks), at, at - start, gap))

        if msvcrt:
            while msvcrt.kbhit():
                msvcrt.getwch()
            threading.Thread(target=listen, daemon=True).start()
            print("   press SPACE (or m) each time spoke 0 passes the reference; Ctrl+C stops the move")
        try:
            while time.time() < deadline:
                try:
                    time.sleep(0.01)
                    ta = time.time()
                    d = self._drive_state()
                    tm = (ta + time.time()) / 2
                    pos = d.get("position_steps", 0)
                    place(tm, pos)
                    prev = (tm, pos)
                    if pos != last and time.time() - last_print >= 0.25:
                        print("   t=%5.2fs  pos %7d steps  (%+d of %+d)" % (time.time() - t0, pos, pos - start, steps))
                        last, last_print = pos, time.time()
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
            if prev is not None:
                place(time.time() + 1.0, prev[1])
        finally:
            listening.set()
        if len(marks) > 1:
            print("   marks: %s  ->  %s steps between them" % (marks, [b - a for a, b in zip(marks, marks[1:])]))
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
            start = self._drive_state().get("position_steps", 0)
            armed = self._laser_armed_trial()
            if armed:
                print("   laser-stop is ARMED (trial %d): the next beam break stops this move" % armed)
            self._drive_cmd(cr.DRIVE_MOVE_CODE, op[1])
            self._await_roller(op[1], start)
            if armed:
                self._laser_finish(armed, op[1])

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

    # ---- the laser receiver and laser-stop (lab image)
    def _laser(self, query=""):
        z = board_laser_state(self.s.knobs["host"], float(self.s.knobs["timeout"]), query)
        if z is None:
            raise ValueError("this firmware has no laser receiver input: flash s3_devkit_lab (a build with the laser-stop)")
        return z

    def _laser_armed_trial(self):
        """The armed trial's number, or None (not armed, or an image without the laser receiver)."""
        try:
            z = board_laser_state(self.s.knobs["host"], float(self.s.knobs["timeout"]))
        except (OSError, ValueError):
            return None
        return z["trial"]["trial"] if z and z.get("armed") and z.get("trial") else None

    def _laser_csv(self):
        return os.path.join(self.s.dir, "laser_trials.csv")

    def _laser_rows(self):
        if not os.path.exists(self._laser_csv()):
            return []
        with open(self._laser_csv(), newline="", encoding="utf-8") as f:
            return list(csv.DictReader(f))

    def _laser_log(self, row):
        row = dict(row, n=len(self._laser_rows()) + 1)
        new = not os.path.exists(self._laser_csv())
        with open(self._laser_csv(), "a", newline="", encoding="utf-8") as f:
            w = csv.DictWriter(f, fieldnames=LASER_FIELDS)
            if new:
                w.writeheader()
            w.writerow(row)
        return row

    def _laser_finish(self, trial, move_steps, note=""):
        """After a move that ran armed: wait for the board to close the trial, read the beam again once the wheel
        has had 0.5 s to settle, then report the trial and log it. The row, or None. `note` marks a trial whose
        move was also stopped by hand: a break during THAT deceleration still counts as the trigger."""
        t_end = time.time() + 2.0
        while True:
            z = self._laser()
            if (z.get("last") or {}).get("trial") == trial:
                break
            if time.time() > t_end:
                self._laser("arm=0")
                print("   !! laser trial %d did not close (a move too short for the 2 ms poll to see?): disarmed" % trial)
                return None
            time.sleep(0.05)
        time.sleep(0.5)
        settled = self._laser()
        driver = ""
        drv = board_drive_state(self.s.knobs["host"], float(self.s.knobs["timeout"]))
        if drv:
            driver = ("FAULT 0x%x" % drv.get("fault_detail", 0) if drv.get("faulted") else
                      drv.get("drv_flags", "") if drv.get("drv_ok") else "UART silent")
        row = self._laser_log(laser_trial_row(z["last"], settled.get("beam"), move_steps,
                                              int(settled.get("glitches", 0)), driver, note))
        print(laser_trial_report(row))
        print("   logged: row %d of %s" % (row["n"], self._laser_csv()))
        return row

    def _wait_move(self, steps):
        """Quietly wait for a move to end. Ctrl+C asks for the controlled stop; True if it was pressed."""
        hz = max(1, int(self._drive_state().get("max_step_hz") or 1))
        deadline = time.time() + abs(steps) / hz * 2.0 + 5.0
        interrupted = False
        while time.time() < deadline:
            try:
                time.sleep(0.05)
                if not self._drive_state()["moving"]:
                    return interrupted
            except KeyboardInterrupt:
                if not interrupted:
                    self._drive_cmd(cr.DRIVE_STOP_CODE)
                    print("   stop requested: decelerating (the series ends after this trial)")
                    interrupted, deadline = True, time.time() + 5.0
        print("   !! still moving after the expected time: `roll stop`")
        return interrupted

    def _laser_status(self, z):
        age = z.get("last_edge_age_ms")
        checked = "" if z.get("input") == "unprobed" else " (checked %.1f s ago)" % (z.get("probed_age_ms", 0) / 1000.0)
        print("   laser receiver on GPIO %s: input %s%s, beam %s (raw %s)" % (
            z.get("gpio"), z.get("input"), checked, z.get("beam"), z.get("level")))
        if z.get("input") == "unprobed":
            print("   (no pull test with this high-impedance divider: a broken OUT wire reads CLEAR, so confirm the "
                  "wiring by waving a card through the beam under `laser watch`)")
        print("   edges %d (%d breaks, %d clears), glitches %d, found late by the poll %d, %s" % (
            z.get("edges", 0), z.get("enters", 0), z.get("leaves", 0), z.get("glitches", 0), z.get("resyncs", 0),
            "never changed" if age is None else "last change %.1f s ago" % (age / 1000.0)))
        print("   laser-stop %s; %d trials armed so far, %d stops requested" % (
            "ARMED (trial %d)" % z["trial"]["trial"] if z.get("armed") and z.get("trial") else "disarmed",
            z.get("trials", 0), z.get("stops_requested", 0)))
        if z.get("last"):
            t = z["last"]
            print("   last trial #%d: %s%s" % (t["trial"], t["result"], "  break at %d, last pulse at %d (%+d)" % (
                t["trigger_pos"], t["final_pos"], t["final_pos"] - t["trigger_pos"]) if t.get("fired") else ""))
        if z.get("input") == "floating":
            print("   !! GPIO %s floats: nothing drives it. Check the receiver's OUT -> divider -> GPIO wiring "
                  "(docs/LASER_BENCH.md)" % z.get("gpio"))

    def _laser_edges(self, seq, prev, edges):
        """Fetch and print the edges after `seq`; the new seq."""
        z = self._laser("since=%d" % seq)
        if z.get("log_lost"):
            print("   (%d edges came faster than this window and were lost)" % z["log_lost"])
        for e in z.get("log", []):
            seq = e["seq"]
            edges.append(e)
            print(laser_edge_line(e, prev))
        return seq

    def _laser_watch(self, seconds):
        z = self._laser()
        seq, prev, edges = z.get("log_seq", 0), {}, []
        print("   watching GPIO %s (beam %s now); turn the wheel by hand or `roll` from elsewhere. Ctrl+C stops."
              % (z.get("gpio"), z.get("beam")))
        t_end = time.time() + seconds if seconds else None
        try:
            while t_end is None or time.time() < t_end:
                time.sleep(0.05)
                seq = self._laser_edges(seq, prev, edges)
        except KeyboardInterrupt:
            pass
        print("   %d edges seen" % len(edges))

    def _laser_scan(self, steps):
        z = self._laser()
        if z.get("armed"):
            raise ValueError("laser-stop is armed: the scan would stop at the first break. `laser disarm` first")
        d = self._drive_state()
        if d.get("moving"):
            raise ValueError("the roller is moving: `roll stop`, or wait for it to finish")
        seq, prev, edges, stopping = z.get("log_seq", 0), {}, [], False
        print("   scanning %+d steps at %d steps/s, %d steps/s2 with the laser DISARMED (nothing stops on a break)"
              % (steps, d.get("max_step_hz", 0), d.get("accel_steps_s2", 0)))
        self._drive_cmd(cr.DRIVE_MOVE_CODE, steps)
        while True:
            try:
                time.sleep(0.05)
                seq = self._laser_edges(seq, prev, edges)
                d = self._drive_state()
                if not d["moving"]:
                    seq = self._laser_edges(seq, prev, edges)
                    break
            except KeyboardInterrupt:
                if not stopping:
                    self._drive_cmd(cr.DRIVE_STOP_CODE)
                    print("   stop requested: decelerating")
                    stopping = True
        widths, spacing = laser_scan_summary(edges)
        print("   %d breaks; blocked for %s steps; break to break %s steps" % (
            sum(1 for e in edges if e["edge"] == "ENTER"), widths or "-", spacing or "-"))
        print(drive_line(d))

    def _laser_trials(self, steps, n):
        rows = []
        for i in range(n):
            d = self._drive_state()
            if not d.get("enabled"):
                raise ValueError("the roller is off: `roll on` first")
            if d.get("moving"):
                raise ValueError("the roller is moving: `roll stop`, or wait for it to finish")
            z = self._laser()
            if z.get("input") == "floating":
                print("   !! GPIO %s floats: no receiver is driving it, so nothing can trigger" % z.get("gpio"))
            if z.get("beam") == "BLOCKED":
                print("   beam is BLOCKED now: the trigger fires on the NEXT break, after it clears")
            a = self._laser("arm=1")
            if not a.get("action_ok"):
                raise ValueError(a.get("detail", "arm refused"))
            trial = a["trial"]["trial"]
            print("   trial %d of %d (board trial %d): armed; moving %+d steps at %d steps/s, %d steps/s2" % (
                i + 1, n, trial, steps, d.get("max_step_hz", 0), d.get("accel_steps_s2", 0)))
            self._drive_cmd(cr.DRIVE_MOVE_CODE, steps)
            interrupted = self._wait_move(steps)
            row = self._laser_finish(trial, steps, "manual stop requested (Ctrl+C)" if interrupted else "")
            if row is not None:
                rows.append(row)
            if interrupted or row is None or row["result"] == "INTERRUPTED":
                break
            time.sleep(0.3)
        if len(rows) > 1:
            print(laser_table(rows))

    def do_laser(self, arg):
        """laser [status | arm | disarm | probe | watch [s] | scan <+/-steps> | trial <+/-steps> [n] | log]: the laser
        break-beam receiver (GPIO 7) and the bench laser-stop, lab image only (s3_devkit_lab).
          laser              the receiver now: input driven/floating, beam CLEAR/BLOCKED, edges, glitches, trials
          laser arm          one-shot: the next beam break while the roller moves requests its controlled stop (the
                             same deceleration as `roll stop`); then `roll <steps>` reports and logs the trial
          laser disarm       cancel an armed, unfired trial
          laser probe        re-check whether anything drives GPIO 7 (refused while armed or moving)
          laser watch [s]    print every beam edge live, with its step count and the distance from the last edge
          laser scan <steps> a move with the laser DISARMED, printing each break/clear: blocked widths and spacing
          laser trial <steps> [n]  n trials: arm, move, stop on the break, report, log a row (Ctrl+C ends the series)
          laser log          this session's trials as a table (laser_trials.csv in the session folder)
        `roll stop`, `roll off` and driver faults keep priority: the laser only ever requests the same stop. Step
        counts are pulses sent, never proof the wheel followed; the beam level at the stop is the observation."""
        if self.offline:
            if parse_laser(arg)[0] == "log":
                print(laser_table(self._laser_rows()))
                return
            raise ValueError("offline session: no board")
        op = parse_laser(arg)
        if op[0] == "status":
            self._laser_status(self._laser())
        elif op[0] in ("arm", "disarm", "probe"):
            z = self._laser({"arm": "arm=1", "disarm": "arm=0", "probe": "probe=1"}[op[0]])
            print("   %s%s" % ("" if z.get("action_ok") else "!! refused: ", z.get("detail", "")))
            self._laser_status(z)
        elif op[0] == "watch":
            self._laser_watch(op[1])
        elif op[0] == "scan":
            self._laser_scan(op[1])
        elif op[0] == "trial":
            self._laser_trials(op[1], op[2])
        else:
            rows = self._laser_rows()
            print(laser_table(rows) if rows else "   no laser trials in this session yet")
            print("   %s" % self._laser_csv())

    # ---- the alignment campaign (lab image): the real-time controller lives on the board
    def _acfg(self):
        cfg = self.s.knobs.setdefault("align", {})
        for k, v in ALIGN_DEFAULTS.items():
            cfg.setdefault(k, v)
        return cfg

    def _align(self, query=""):
        z = board_align_state(self.s.knobs["host"], float(self.s.knobs["timeout"]), query)
        if z is None:
            raise ValueError("this firmware has no alignment controller: flash s3_devkit_lab built from this branch")
        return z

    def _align_csv(self, name):
        return os.path.join(self.s.dir, name)

    def _csv_rows(self, name):
        p = self._align_csv(name)
        if not os.path.exists(p):
            return []
        with open(p, newline="", encoding="utf-8") as f:
            return list(csv.DictReader(f))

    def _csv_log(self, name, fields, row):
        path = self._align_csv(name)
        if os.path.exists(path):
            with open(path, newline="", encoding="utf-8") as f:
                header = next(csv.reader(f), [])
            if header != list(fields):
                # the columns changed since this file was started: keep the old file whole beside the new one, never
                # append rows under a header they do not match
                os.replace(path, "%s.old-%s" % (path, time.strftime("%H%M%S")))
        row = dict(row, n=len(self._csv_rows(name)) + 1)
        new = not os.path.exists(self._align_csv(name))
        with open(self._align_csv(name), "a", newline="", encoding="utf-8") as f:
            w = csv.DictWriter(f, fieldnames=fields, extrasaction="ignore")
            if new:
                w.writeheader()
            w.writerow(row)
        return row

    def _align_run(self, query):
        """Start one controller run on the board and follow it live: every phase change as a tagged line, every
        beam edge after the trigger. Ctrl+C aborts it (the drive's controlled stop). Returns (final state, all
        edges), and keeps the full record as align_run_<run>.json in the session folder."""
        z = self._align("start=" + query)
        if not z.get("action_ok"):
            raise ValueError("the board refused the run: %s" % z.get("detail"))
        run, seen_ev, n_edge, edges, aborted = z["run"], 0, 0, [], False
        t0 = z.get("start_t_us", 0)
        while True:
            try:
                time.sleep(0.06)
                z = self._align("since=%d" % n_edge)
                if z["run"] != run:
                    raise ValueError("another run replaced this one")
                for ev in z.get("events", [])[seen_ev:]:
                    print(align_event_line(ev, z, t0))
                seen_ev = len(z.get("events", []))
                for e in z.get("edges", []):
                    edges.append(e)
                    ts = ((e["t_us"] - t0) % 2 ** 32) / 1e6
                    if e["phase"] == "APPROACH":
                        if e["enter"]:
                            print("   %6.3fs  [LASER] break #%d at step %d" % (
                                ts, sum(1 for x in edges if x["enter"] and x["phase"] == "APPROACH"), e["pos"]))
                    else:
                        print("   %6.3fs  [LASER] %s at step %d (%s, zone %+d)%s" % (
                            ts, "BLOCKED" if e["enter"] else "CLEAR", e["pos"],
                            {1: "along", -1: "back", 0: "STILL"}.get(e["sign"], "?"), e["zone"],
                            "  <- REACQUIRED" if e["phase"] == "CORRECT" and e["enter"] and e["zone"] == 0 else
                            "  <- CLEARED the spoke" if e["phase"] == "CORRECT" and not e["enter"] and e["zone"] == -1 else ""))
                n_edge = len(edges)
                if not z.get("active") and n_edge >= z.get("n_edges", 0):
                    break
            except KeyboardInterrupt:
                if not aborted:
                    self._align("abort=1")
                    print("   abort requested: controlled stop")
                    aborted = True
        with open(self._align_csv("align_run_%d.json" % run), "w", encoding="utf-8") as f:
            json.dump({"state": z, "edges": edges}, f, indent=1)
        if z.get("edges_dropped"):
            print("   !! %d edges did not fit the board's log" % z["edges_dropped"])
        return z, edges

    def _align_preflight(self):
        d = self._drive_state()
        if not d.get("enabled"):
            raise ValueError("the roller is off: `roll on` first")
        if d.get("moving"):
            raise ValueError("the roller is moving: `roll stop`, or wait")
        if d.get("mres_ok") is False:
            # one more look first: the read shares the driver's single wire with whatever else is talking to it
            time.sleep(0.3)
            d = self._drive_state()
            if d.get("mres_ok") is False:
                raise ValueError("the driver's microstep setting is not the %s the step counts assume (a driver reset "
                                 "reverts it to its MS1/MS2 pins, 8 here, which halves every step count): `roll on` "
                                 "rewrites and verifies it" % (d.get("microsteps_want") or "wanted count"))
        lz = self._laser()
        if lz.get("input") == "floating":
            raise ValueError("GPIO %s floats: no receiver drives it" % lz.get("gpio"))
        return d, lz

    # stage 0 ----------------------------------------------------------------------------------------------
    def _align_count(self, speeds, breaks=None):
        """`breaks` overrides the stop count (default: one full turn, 33 from before the beam / 32 from in it)."""
        cfg = self._acfg()
        if not cfg["max"]:
            raise ValueError("set a travel bound first: `align set max <steps>` (generous: more than one wheel turn)")
        for hz in speeds or [None]:
            if hz:
                d = self._drive_state()
                self._drive_cmd(cr.DRIVE_MOTION_CODE, cr.drive_motion_arg(hz, int(d.get("accel_steps_s2") or 1600)))
            d, lz = self._align_preflight()
            start_blocked = lz["beam"] == "BLOCKED"
            n = breaks or int(cfg["spokes"]) + (0 if start_blocked else 1)
            print("== STAGE 0 count run: %d steps/s, %d steps/s2, dir %+d, stop on break %d (%s)" % (
                d["max_step_hz"], d["accel_steps_s2"], cfg["dir"], n,
                "operator-chosen stop count: the spoke in the beam afterwards is not inferred" if breaks else
                "spoke 0 IN the beam: its 32nd break is spoke 0 again" if start_blocked else
                "spoke 0 just BEFORE the beam: break 1 is spoke 0, break 33 is spoke 0 again"))
            st, edges = self._align_run("count&dir=%d&n=%d&max=%d&settle=%d" % (cfg["dir"], n, cfg["max"],
                                                                                 cfg["settle_ms"]))
            lz2 = self._laser()
            a = count_analysis(edges, start_blocked, int(cfg["spokes"]))
            glitches = int(lz2.get("glitches", 0)) - int(lz.get("glitches", 0))
            ret = None
            if a["rev_steps"] is not None and st.get("fired"):
                # where the wheel stopped relative to spoke 0's edge now, minus where it started relative to it then
                ret = ((st["final_pos"] - a["enters"][-1]["pos"]) - (st["start_pos"] - a["enters"][0]["pos"])) * cfg["dir"]
            # PASS is about COUNTING only. Where the stop left the wheel is reported beside it, never folded in:
            # counting reliability and positioning are separate verdicts.
            ok = (st["result"] == "COUNT_DONE" and st["breaks"] == n and a["missed"] == 0 and a["duplicates"] == 0
                  and glitches == 0 and not st.get("edges_dropped"))
            row = {"time": now(), "run": st["run"], "speed": st["approach_hz"], "accel": st["approach_accel"],
                   "dir": cfg["dir"], "expected_breaks": n, "start_blocked": int(start_blocked), "breaks": st["breaks"],
                   "missed": a["missed"], "duplicates": a["duplicates"], "glitches": glitches,
                   "elapsed_s": round(((st["end_t_us"] - st["start_t_us"]) % 2 ** 32) / 1e6, 2),
                   "start_pos": st["start_pos"], "trigger_pos": st["trigger_pos"], "final_pos": st["final_pos"],
                   "overshoot_steps": (st["stop_pos"] - st["trigger_pos"]) * cfg["dir"] if st.get("fired") else "",
                   "pitch_med": a["pitch_med"], "width_med": a["width_med"], "rev_steps": a["rev_steps"] or "",
                   "return_error_steps": "" if ret is None else ret, "final_blocked": int(bool(st.get("final_blocked"))),
                   "fw_result": st["result"], "verdict": "PASS" if ok else "FAIL"}
            row = self._csv_log("align_count.csv", COUNT_FIELDS, row)
            print("   [COUNT] %s: %d of %d breaks, missed %d, extra %d, glitches %d, %.1f s; spacing median %s steps, "
                  "blocked median %s steps%s; stopped %s steps past the last break, beam %s" % (
                      row["verdict"], st["breaks"], n, a["missed"], a["duplicates"], glitches, row["elapsed_s"],
                      a["pitch_med"], a["width_med"],
                      ("; one revolution = %d steps edge to edge, return error %+d steps" % (a["rev_steps"], ret))
                      if ret is not None else "; revolution not measured (%s)" % (
                          "stopped short of one full turn" if breaks else "started in the beam"),
                      row["overshoot_steps"], "BLOCKED" if st.get("final_blocked") else "CLEAR"))
            if a["period"] > 1:
                print("   spacing is NOT uniform: it repeats every %d spokes (%s steps, break to break)" % (
                    a["period"], ", ".join(str(x) for x in a["pattern"])))
            print("   (PASS means the laser counted %d spoke passages cleanly. It does NOT show the roller can align a "
                  "spoke with a station.)" % int(cfg["spokes"]))
            # Which spoke is in the beam now: the 32nd/33rd break was spoke 0 if the count was clean; the stop then
            # carried the wheel zone/2 spokes further (zone accounting on the board).
            zone = int(st.get("final_zone", 0))
            if breaks or not ok or (zone % 2 == 0) != bool(st.get("final_blocked")):
                cfg["belief"] = None
            elif zone == 0:
                cfg["belief"] = 0
            elif cfg["index_dir"] in (1, -1):
                cfg["belief"] = (cfg["index_dir"] * (zone // 2)) % int(cfg["spokes"])
            else:
                cfg["belief"] = None
            if ok and a["pitch_med"]:
                cfg.update(pitch=a["pitch_med"], width=a["width_med"], measured_by="count run %d @ %d steps/s" % (
                    st["run"], st["approach_hz"]))
                if a["rev_steps"]:
                    cfg["rev_steps_measured"] = a["rev_steps"]
                print("   measured: spacing %d, blocked width %d steps (%s)" % (a["pitch_med"], a["width_med"],
                                                                             cfg["measured_by"]))
            cfg["belief_blocked"] = None
            self.s.save()
            print("   now in / just behind the beam: %s" % (
                "spoke %d" % cfg["belief"] if cfg["belief"] is not None else
                "UNKNOWN (the stop left the wheel in a gap or the count was not clean) -- place a spoke, `align spoke <k>`"))
        print("   %s" % self._align_csv("align_count.csv"))

    # stages 1-3 -------------------------------------------------------------------------------------------
    def _align_trial(self, cls, strategy, stage):
        cfg = self._acfg()
        if cfg["index_dir"] not in (1, -1):
            raise ValueError("which way do spoke numbers run? `align set index_dir +1|-1` (does spoke 1 or spoke 31 "
                             "reach the beam next when the roller turns along dir?)")
        if cfg["belief"] is None:
            raise ValueError("which spoke is in the beam? place one and `align spoke <k>`")
        if strategy != "STOP" and not (cfg["slow_hz"] and cfg["slow_accel"]):
            raise ValueError("%s moves at the slow ramp: `align set slow <steps/s> <steps/s2>`" % strategy)
        if strategy == "REVERSE_FIXED" and not cfg["rev_steps"]:
            raise ValueError("REVERSE_FIXED needs `align set rev <steps>`")
        d, lz = self._align_preflight()
        # The beam level at the start has to agree with where the last trial said it left the wheel. If the last trial
        # left it CLEAR just behind a spoke and that spoke is IN the beam now (a strike nudged it, a hand, creep), the
        # controller will not count that spoke's break, so the target break would be one too many: it would stop a
        # spoke late. That is the spoke-2-passes-and-spoke-3-stops failure of 2026-10-03.
        was, was_blocked = cfg["belief"], cfg.get("belief_blocked")
        cfg["belief"], cfg["belief_blocked"] = reconcile_belief(
            cfg["belief"], cfg.get("belief_blocked"), lz["beam"] == "BLOCKED", cfg["index_dir"], int(cfg["spokes"]))
        if cfg["belief"] != was:
            print("   [BELIEF] the beam is BLOCKED at the start but the last trial left it clear: the next spoke "
                  "ahead is already in the beam -> now spoke %d" % cfg["belief"])
        if was_blocked and lz["beam"] != "BLOCKED":
            print("   [BELIEF] the last trial left spoke %d IN the beam but the beam is CLEAR now: the strikes pushed it on "
                  "past (assumed forward, as every earlier nudge was) -> it is behind, the next break is the next spoke" % was)
        n_sp = int(cfg["spokes"])
        s = int(cfg["belief"])
        for j in range(1, 5):
            idx = (s + cfg["index_dir"] * j) % n_sp
            if spoke_class(idx) == cls:
                break
        max_steps = int(cfg["max"]) if cfg["max"] else (int((j + 1.5) * cfg["pitch"]) if cfg["pitch"] else None)
        if not max_steps:
            raise ValueError("no travel bound: run `align count` first (it measures the spacing) or `align set max`")
        rmax = cfg["rmax"] or (4 * int(cfg["pitch"]) if cfg["pitch"] else None)
        if strategy in ("REACQUIRE", "REACQUIRE_CLEAR") and not rmax:
            raise ValueError("%s needs a search bound: `align set rmax <steps>`" % strategy)
        station = cr.station_for_spoke(idx)
        print("== TRIAL stage %s  %s spoke %d (%s station), break %d along dir %+d, %s, %d steps/s %d steps/s2" % (
            stage, cls, idx, station, j, cfg["dir"], strategy, d["max_step_hz"], d["accel_steps_s2"]))
        q = "align&dir=%d&n=%d&max=%d&settle=%d&strat=%s&rev=%d&off=%d&center=%d&shz=%d&sacc=%d&rmax=%d" % (
            cfg["dir"], j, max_steps, cfg["settle_ms"], strategy, cfg["rev_steps"] or 0, cfg["offset"] or 0,
            1 if cfg["center"] else 0, cfg["slow_hz"] or 0, cfg["slow_accel"] or 0, rmax or 0)
        st, edges = self._align_run(q)
        dirn = cfg["dir"]
        fired = st.get("fired")
        w = st.get("width_used") if st.get("width_used", -1) > 0 else None
        res = st["result"]
        # belief: zone/2 spokes along dir from the target; lost when the controller cannot vouch for the zone
        if res in ("ALIGNED", "OVERSHOOT", "UNDERSHOOT", "REVERSE_FAILED", "REACQUISITION_FAILED") and fired:
            cfg["belief"] = (idx + cfg["index_dir"] * (int(st["final_zone"]) // 2)) % n_sp
        else:
            cfg["belief"] = None
        cfg["belief_blocked"] = bool(st.get("final_blocked")) if cfg["belief"] is not None else None
        self.s.save()
        row = {"time": now(), "stage": stage, "class": cls, "spoke": idx, "station": station, "strategy": strategy,
               "run": st["run"], "dir": dirn, "speed": st["approach_hz"], "accel": st["approach_accel"],
               "slow_hz": cfg["slow_hz"] or "", "slow_accel": cfg["slow_accel"] or "", "rev_steps": cfg["rev_steps"] or "",
               "offset": cfg["offset"], "center": int(bool(cfg["center"])), "settle_ms": cfg["settle_ms"],
               "target_break": j, "start_pos": st["start_pos"], "start_blocked": int(bool(st["start_blocked"])),
               "trigger_pos": st["trigger_pos"] if fired else "", "trigger_t_us": st["trigger_t_us"] if fired else "",
               "stop_pos": st["stop_pos"] if fired else "", "stop_t_us": st["stop_t_us"] if fired else "",
               "stop_ms": round(((st["stop_t_us"] - st["trigger_t_us"]) % 2 ** 32) / 1000.0, 1) if fired else "",
               "overshoot_steps": (st["stop_pos"] - st["trigger_pos"]) * dirn if fired else "",
               "zone_after_stop": st["zone_after_stop"] if fired else "",
               "blocked_after_stop": int(bool(st["blocked_after_stop"])) if fired else "",
               "width_used": w or "", "pitch_med": st["pitch_med"] if st["pitch_med"] > 0 else (cfg["pitch"] or ""),
               "corr_cmd": st["corr_cmd"], "reacquisitions": st["reacquisitions"],
               "reacq_pos": st["reacq_pos"] if st.get("reacquired") else "",
               "reacq_offset": (st["reacq_pos"] - st["target_leave_pos"]) * dirn
               if st.get("reacquired") and st.get("target_left") else "",
               "same_spoke": ("ok" if st["same_spoke_ok"] else "FAILED") if st.get("same_spoke_checked") else "unchecked",
               "trim_cmd": st["trim_cmd"], "final_pos": st["final_pos"],
               # signed distance from the middle of the target's measured blocked span: |e| < width/2 is inside it
               "final_error_steps": ((st["final_pos"] - st["trigger_pos"]) * dirn - w // 2) if fired and w else "",
               "final_zone": st["final_zone"] if fired else "", "final_blocked": int(bool(st["final_blocked"])),
               "edges_still": st["edges_still"], "fw_result": res, "fw_detail": st["result_detail"],
               "pluck": "no", "pluck_moved_spoke": "", "acoustic_status": "", "acoustic_reason": "",
               "acoustic_f1_hz": "", "capture_result": "", "shot": "",
               "alignment_reference": "laser beam; beam_at_station=%s" % cfg["beam_at_station"]}
        plucked = pluck_ok = acoustic_ok = False
        n_pl = max(1, int(cfg.get("plucks") or 1))
        # `always` plucks wherever the wheel ended, so the strike result judges the position -- but never after the
        # controller lost the plot (no trigger, an abort, a fault, an ambiguous beam).
        lost = res in ("ABORTED", "FAULT", "LASER_MISSED", "LASER_AMBIGUOUS")
        if (res == "ALIGNED" and cfg["pluck"] in ("aligned", "always")) or (cfg["pluck"] == "always" and fired
                                                                              and not lost):
            plucked = True
            before = self._laser()
            fired_n = hits = dsp_ok = 0
            burst = cfg.get("pluckmode") == "burst"
            if burst:
                try:
                    pulse = int(self.s.knobs["pulse"] or 0) or 20
                    print("   [PLUCK] BURST %d strikes, %s station on spoke %d, %d ms each" % (n_pl, station, idx, pulse))
                    hits, gap = self._strike_burst(station, n_pl, pulse, int(cfg.get("pgap") or 0))
                    fired_n = n_pl
                    row.update(pluck="fired (burst, gap %d ms)" % gap, acoustic_status="burst",
                               acoustic_reason="impacts counted from the mic stream", shot="burst")
                except (ValueError, OSError, SystemExit) as e:
                    row["pluck"] = "FAILED: %s" % e
                    print("   !! burst failed: %s" % e)
            for k in range(1, 1 if burst else n_pl + 1):
                print("   [PLUCK] FIRE %d/%d %s station on spoke %d" % (k, n_pl, station, idx))
                try:
                    saved = self.s.knobs["spoke"]
                    self.s.knobs["spoke"] = idx
                    try:
                        self._one("strike", 0, 1, show=False)
                    finally:
                        self.s.knobs["spoke"] = saved
                    rec = self.s.shots[-1]
                    b = rec.get("board", {})
                    p_ok = b.get("fired") is not False
                    # a HIT is a strike the capture heard: an impact onset. It is NOT the DSP accepting the
                    # tension estimate (LOW_SNR / AMBIGUOUS_PEAK rejections still saw the strike), which is
                    # counted apart as `dsp_ok`.
                    a_ok = (b.get("capture_result") in (None, "OK") and b.get("status") is not None
                            and b.get("reason") != "NO_ONSET_DETECTED")
                    dsp_ok += int(p_ok and a_ok and str(b.get("status")).lower() != "rejected")
                    fired_n += int(p_ok)
                    hits += int(p_ok and a_ok)
                    row.update(pluck="fired" if p_ok else "NOT FIRED", acoustic_status=b.get("status"),
                               acoustic_reason=b.get("reason"), acoustic_f1_hz=b.get("expect_f1_hz") or "",
                               capture_result=b.get("capture_result") or "", shot=rec["name"])
                    print("   [ACOUSTIC] CAPTURE %d/%d %s/%s f1 %s  (%s)  -> %s" % (
                        k, n_pl, b.get("status"), b.get("reason"), b.get("expect_f1_hz"), rec["name"],
                        "HIT (strike heard)" if p_ok and a_ok else "NO HIT"))
                except (ValueError, OSError, SystemExit) as e:
                    row["pluck"] = "FAILED: %s" % e
                    print("   !! pluck %d/%d failed: %s" % (k, n_pl, e))
            pluck_ok, acoustic_ok = fired_n == n_pl, hits == n_pl
            row.update(pluck_n=n_pl, pluck_hits=hits, pluck_dsp_ok=dsp_ok)
            after = self._laser()
            moved = int(after.get("edges", 0)) != int(before.get("edges", 0)) or after.get("beam") != before.get("beam")
            row["pluck_moved_spoke"] = int(moved)
            if moved:
                print("   [PLUCK] the beam changed during the strikes: the spoke moved")
            if burst:
                print("   [PLUCK] %d of %d strikes heard (impacts counted from the mic stream; no per-strike analysis "
                      "in burst mode)" % (hits, n_pl))
            else:
                print("   [PLUCK] %d of %d strikes heard (impact onset); the DSP accepted the tension estimate on %d of "
                      "%d (the rest were rejected for LOW_SNR/AMBIGUOUS_PEAK etc.)" % (hits, n_pl, dsp_ok, n_pl))
        row["verdict"] = align_verdict(res, plucked, pluck_ok, acoustic_ok)
        row = self._csv_log("align_trials.csv", ALIGN_FIELDS, row)
        print("   [TRIAL] %s  -- %s spoke %d, %s: overshoot %s steps, final %s steps from the span middle (width %s), "
              "zone %s%s" % (row["verdict"], cls, idx, strategy, row["overshoot_steps"], row["final_error_steps"],
                             w, row["final_zone"], ", reacquired %s (%s)" % (row["reacq_offset"], row["same_spoke"])
                             if st.get("reacquired") else ""))
        return row

    def _align_pluck(self, idx):
        """Strike spoke `idx` from its station, as a trial's burst would, without moving anything. For a sequencer that
        checks which spoke really stopped at the beam BEFORE it strikes (tools/align_revolution.py)."""
        cfg = self._acfg()
        if self._drive_state().get("moving"):
            raise ValueError("the roller is moving: `roll stop`, or wait")
        station = cr.station_for_spoke(idx)
        n_pl = max(1, int(cfg.get("plucks") or 1))
        pulse = int(self.s.knobs["pulse"] or 0) or 20
        before = self._laser()
        print("   [PLUCK] BURST %d strikes, %s station on spoke %d, %d ms each" % (n_pl, station, idx, pulse))
        hits, gap = self._strike_burst(station, n_pl, pulse, int(cfg.get("pgap") or 0))
        after = self._laser()
        if int(after.get("edges", 0)) != int(before.get("edges", 0)) or after.get("beam") != before.get("beam"):
            print("   [PLUCK] the beam changed during the strikes: the spoke moved")
        print("   [PLUCK] %d of %d strikes heard, gap %d ms (impacts counted from the mic stream)" % (hits, n_pl, gap))

    # the adaptive ladder ---------------------------------------------------------------------------------
    def _align_series(self, cls, strategy, n, stage):
        rows = []
        for _ in range(n):
            r = self._align_trial(cls, strategy, stage)
            rows.append(r)
            if r["fw_result"] in ("ABORTED", "FAULT", "LASER_MISSED", "LASER_AMBIGUOUS") or self._acfg()["belief"] is None:
                print("   series stopped: %s (the next trial would not know which spoke it targets)" % r["fw_result"])
                break
        return rows

    def _align_campaign(self, cls):
        """Hypothesize -> run -> classify -> choose the next bounded test, for ONE spoke class. Rules, in order:
          1. STOP x3. All ALIGNMENT_SUCCESS -> repeat STOP x5 (stage 3).
          2. Else, if every stop fired and the overshoot spread is under half the measured blocked width, the stop
             is predictable enough for a step-based correction: RETURN_TO_EDGE (centered) x3 -> x5 if all succeed.
          3. Else (or if 2 failed): REACQUIRE (centered) x3 -> x5 if all succeed.
          4. Nothing passed: report what failed; no strategy is called adoptable."""
        cfg = self._acfg()
        print("\n######## CAMPAIGN %s ########" % cls)
        s1 = self._align_series(cls, "STOP", 3, "1")
        ok = lambda rows, k: len(rows) == k and all(r["verdict"] == "ALIGNMENT_SUCCESS" for r in rows)  # noqa: E731
        if ok(s1, 3):
            print("   decision: STOP aligned 3/3 -> stage 3 repeat x%d" % ADOPT_RUN)
            return ("STOP", self._align_series(cls, "STOP", ADOPT_RUN, "3"))
        if len(s1) < 3:
            return (None, s1)
        overs = [int(r["overshoot_steps"]) for r in s1 if r["overshoot_steps"] != ""]
        widths = [int(r["width_used"]) for r in s1 if r["width_used"] != ""]
        spread = (max(overs) - min(overs)) if len(overs) == 3 else None
        width = _median(widths) or cfg["width"]
        print("   STOP: %d/3 aligned; overshoot %s steps (spread %s); blocked width %s steps" % (
            sum(1 for r in s1 if r["verdict"] == "ALIGNMENT_SUCCESS"), overs, spread, width))
        tried = []
        saved_center = cfg["center"]
        cfg["center"] = True
        try:
            if spread is not None and width and spread * 2 < width:
                print("   decision: the stop is repeatable within half a spoke width -> step-based RETURN_TO_EDGE x3")
                tried.append("RETURN_TO_EDGE")
                s2 = self._align_series(cls, "RETURN_TO_EDGE", 3, "2")
                if ok(s2, 3):
                    print("   decision: RETURN_TO_EDGE 3/3 -> stage 3 repeat x%d" % ADOPT_RUN)
                    return ("RETURN_TO_EDGE", self._align_series(cls, "RETURN_TO_EDGE", ADOPT_RUN, "3"))
                if len(s2) < 3:
                    return (None, s2)
            else:
                print("   decision: the stop is not predictable enough for a fixed step correction -> laser-guided "
                      "REACQUIRE")
            if not (cfg["slow_hz"] and cfg["slow_accel"]):
                print("   !! REACQUIRE needs `align set slow <steps/s> <steps/s2>`: stopping here")
                return (None, [])
            tried.append("REACQUIRE")
            s2 = self._align_series(cls, "REACQUIRE", 3, "2")
            if ok(s2, 3):
                print("   decision: REACQUIRE 3/3 -> stage 3 repeat x%d" % ADOPT_RUN)
                return ("REACQUIRE", self._align_series(cls, "REACQUIRE", ADOPT_RUN, "3"))
            print("   decision: no strategy passed (%s): nothing adoptable for %s yet" % (", ".join(tried), cls))
            return (None, s2)
        finally:
            cfg["center"] = saved_center
            self.s.save()

    def _align_log(self):
        counts = self._csv_rows("align_count.csv")
        if counts:
            print("   STAGE 0  laser count reliability")
            print("   %3s %5s %6s %6s %6s %6s %5s %4s %5s %6s %6s %7s  %s" % (
                "n", "run", "speed", "accel", "breaks", "missed", "extra", "glit", "time", "pitch", "rev", "ret_err",
                "verdict"))
            for r in counts:
                print("   %3s %5s %6s %6s %3s/%-2s %6s %5s %4s %5s %6s %6s %7s  %s" % (
                    r["n"], r["run"], r["speed"], r["accel"], r["breaks"], r["expected_breaks"], r["missed"],
                    r["duplicates"], r["glitches"], r["elapsed_s"], r["pitch_med"], r["rev_steps"],
                    r["return_error_steps"], r["verdict"]))
        rows = self._csv_rows("align_trials.csv")
        if not rows:
            print("   no alignment trials in this session yet")
            return
        print("\n   STAGES 1-3  per class and policy (alignment reference: %s)" % rows[-1]["alignment_reference"])
        if "beam_at_station=True" not in rows[-1]["alignment_reference"]:
            print("   !! `aligned` means the spoke blocks the BEAM. It means aligned with the STATION only if the beam "
                  "crosses the strike point: `align set beam_at_station yes` once you have checked that")
        print("   %-13s %-52s %4s %-24s %-11s %-11s %s" % (
            "class", "policy", "n", "verdicts", "overshoot", "final err", "adoption"))
        by_class = {}
        for key, rs in sorted(align_groups(rows).items()):
            cls, pol = key[0], key[1:]
            v = {}
            for r in rs:
                v[r["verdict"]] = v.get(r["verdict"], 0) + 1
            overs = [int(r["overshoot_steps"]) for r in rs if r["overshoot_steps"] != ""]
            errs = [int(r["final_error_steps"]) for r in rs if r["final_error_steps"] != ""]
            status = adoption_status(rs)
            by_class.setdefault(cls, []).append((pol, status))
            print("   %-13s %-52s %4d %-24s %-11s %-11s %s" % (
                cls, policy_str(pol), len(rs),
                " ".join("%s:%d" % (k.replace("ALIGNMENT_", "").replace("_FAILED", "_F"), n) for k, n in v.items()),
                ("%d..%d" % (min(overs), max(overs))) if overs else "-",
                ("%+d..%+d" % (min(errs), max(errs))) if errs else "-", status))
        cands = {c: [p for p, st in v if st == "CANDIDATE"] for c, v in by_class.items()}
        print("\n   VERDICT (a candidate = the last %d trials of that policy on that class all ALIGNMENT_SUCCESS)" %
              ADOPT_RUN)
        for c in SPOKE_CLASS_BY_MOD4.values():
            print("   %-13s %s" % (c, "; ".join(policy_str(p) for p in cands.get(c, [])) or "no adoptable policy yet"))
        common = set.intersection(*[set(cands.get(c, [])) for c in SPOKE_CLASS_BY_MOD4.values()])
        if common:
            print("   ONE GLOBAL POLICY is supported for all four classes: %s" % "; ".join(
                policy_str(p) for p in sorted(common)))
        elif all(cands.get(c) for c in SPOKE_CLASS_BY_MOD4.values()):
            print("   every class has a candidate but not the same one: CLASS-DEPENDENT policies")
        else:
            print("   not every class has evidence yet: no global or class policy can be called")
        print("   %s" % self._align_csv("align_trials.csv"))

    def do_align(self, arg):
        """align [status | set <key> <value> | spoke <k> | count <steps/s>... | trial <class> [n] [strategy] |
        campaign <class|all> | log | abort]: the roller / laser / acoustic ALIGNMENT campaign (lab image). The control
        decisions run on the board in real time (beam interrupt -> controlled stop -> correction); this only
        configures, sequences, plucks and logs. docs/ALIGN_CAMPAIGN.md has the plan.
          align                       controller state, laser, campaign parameters, which spoke is believed in the beam
          align set <key> <value>     dir +1|-1, index_dir +1|-1, spokes 32, settle <ms>, slow <steps/s> <steps/s2>,
                                      rev <steps>, off <steps>, center on|off, rmax <steps>, max <steps>,
                                      pluck aligned|always|never, beam_at_station yes|no,
                                      plucks <n> (strikes per trial), pluckmode capture|burst, pgap <ms> (burst gap)
          align spoke <k>             declare: spoke k is in the beam now (you placed it)
          align pluck <k>             strike spoke k now from its station (burst, `plucks` strikes of the `pulse` knob)
          align count [v1 v2 ...]     STAGE 0: count 32 passages at each speed (steps/s): missed/extra/return error
          align trial <class> [n] [STOP|REVERSE_FIXED|RETURN_TO_EDGE|REACQUIRE|REACQUIRE_CLEAR]   n trials on the next spokes of a class
          align campaign <class|all>  the adaptive ladder per class (STOP -> step-based or laser-guided -> repeat)
          align log                   stage-0 table, per-class policy table, adoption verdicts
          align abort                 end a run now (controlled stop)
        Classes: NDS-leading, DS-leading, NDS-trailing, DS-trailing (spoke m mod 4, the artifact's class map)."""
        if self.offline and not arg.strip().startswith("log"):
            raise ValueError("offline session: no board")
        t = shlex.split(arg)
        head = t[0].lower() if t else "status"
        cfg = self._acfg()
        if head == "status":
            z = self._align()
            print("   controller: run %s %s, phase %s, result %s %s; beam %s" % (
                z.get("run"), "ACTIVE" if z.get("active") else "idle", z.get("phase"), z.get("result", "NONE"),
                z.get("result_detail", ""), "BLOCKED" if z.get("beam_blocked") else "CLEAR"))
            self._laser_status(self._laser())
            print("   campaign parameters (provisional, chosen by you): %s" % ", ".join(
                "%s=%s" % (k, cfg[k]) for k in ("dir", "index_dir", "spokes", "settle_ms", "slow_hz", "slow_accel",
                                                "rev_steps", "offset", "center", "rmax", "max", "pluck", "plucks", "pluckmode", "pgap",
                                                "beam_at_station")))
            print("   measured (stage 0): spacing %s, blocked width %s, revolution %s steps  [%s]" % (
                cfg["pitch"], cfg["width"], cfg["rev_steps_measured"], cfg["measured_by"] or "not yet"))
            b = cfg["belief"]
            print("   believed in / just behind the beam: %s" % ("spoke %d (%s)" % (b, spoke_class(b)) if b is not None
                                                                  else "UNKNOWN -- `align spoke <k>`"))
        elif head == "set":
            if len(t) < 3:
                raise ValueError("align set <key> <value>")
            k, v = t[1].lower(), t[2:]
            if k in ("dir", "index_dir"):
                if v[0] not in ("+1", "1", "-1"):
                    raise ValueError("%s is +1 or -1" % k)
                cfg[k] = -1 if v[0] == "-1" else 1
            elif k == "slow":
                if len(v) != 2:
                    raise ValueError("align set slow <steps/s> <steps/s2>")
                cfg["slow_hz"], cfg["slow_accel"] = int(v[0]), int(v[1])
            elif k in ("spokes", "settle", "rev", "off", "rmax", "max", "plucks", "pgap"):
                cfg[{"settle": "settle_ms", "rev": "rev_steps", "off": "offset"}.get(k, k)] = int(v[0])
            elif k == "center":
                cfg["center"] = v[0].lower() in ("on", "1", "yes", "true")
            elif k == "pluckmode":
                if v[0] not in ("capture", "burst"):
                    raise ValueError("align set pluckmode capture|burst  (capture = one analysed capture per strike; "
                                     "burst = all strikes in one request at the shortest safe gap, impacts counted "
                                     "from the mic stream)")
                cfg["pluckmode"] = v[0]
            elif k == "pluck":
                if v[0] not in ("aligned", "always", "never"):
                    raise ValueError("align set pluck aligned|always|never")
                cfg["pluck"] = v[0]
            elif k == "beam_at_station":
                cfg["beam_at_station"] = v[0].lower() in ("yes", "1", "true")
            else:
                raise ValueError("unknown key %r -- `help align`" % k)
            self.s.save()
            print("   %s set" % k)
        elif head == "spoke":
            cfg["belief"] = int(t[1]) % int(cfg["spokes"])
            cfg["belief_blocked"] = None
            self.s.save()
            print("   spoke %d (%s) is in the beam" % (cfg["belief"], spoke_class(cfg["belief"])))
        elif head == "pluck":
            if len(t) < 2:
                raise ValueError("align pluck <spoke>")
            self._align_pluck(int(t[1]) % int(cfg["spokes"]))
        elif head == "count":
            breaks = None
            speeds = []
            for x in t[1:]:
                if x.lower().startswith("breaks="):
                    breaks = int(x.split("=", 1)[1])
                else:
                    speeds.append(int(x))
            self._align_count(speeds, breaks)
        elif head == "trial":
            if len(t) < 2:
                raise ValueError("align trial <class> [n] [strategy]")
            cls = parse_class(t[1])
            n = int(t[2]) if len(t) > 2 and t[2].isdigit() else 1
            strat = (t[3] if len(t) > 3 else t[2] if len(t) > 2 and not t[2].isdigit() else "STOP").upper()
            if strat not in STRATEGIES:
                raise ValueError("strategy is one of %s" % ", ".join(STRATEGIES))
            self._align_series(cls, strat, n, "manual")
        elif head == "campaign":
            classes = list(SPOKE_CLASS_BY_MOD4.values()) if len(t) > 1 and t[1].lower() == "all" else [parse_class(t[1])]
            for c in classes:
                strat, rows = self._align_campaign(c)
                if strat is None and rows and rows[-1]["fw_result"] in ("ABORTED", "FAULT", "LASER_MISSED",
                                                                        "LASER_AMBIGUOUS"):
                    print("   campaign stopped: fix the cause, `align spoke <k>`, then resume with this class")
                    break
            self._align_log()
        elif head == "log":
            self._align_log()
        elif head == "abort":
            z = self._align("abort=1")
            print("   %s" % z.get("detail"))
        else:
            raise ValueError(ALIGN_USAGE)

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

    @staticmethod
    def _open_listen(url, timeout, wait_s=10.0):
        """Open a listen/strike stream, waiting out a 409 that says the board is still busy (a previous stream or a
        capture settling): the board refuses BEFORE it fires anything, so asking again cannot double-strike. A 409 that
        says no solenoid is enabled on the station is permanent and is raised at once. Timeouts are NOT retried: a
        request that timed out may already have fired."""
        import urllib.request
        t_end = time.time() + wait_s
        while True:
            try:
                return urllib.request.urlopen(url, timeout=timeout)
            except urllib.error.HTTPError as e:
                body = ""
                try:
                    body = e.read().decode("utf-8", "replace")
                except OSError:
                    pass
                if e.code != 409 or "PRESENT=0" in body or time.time() > t_end:
                    raise
                time.sleep(0.3)

    def _strike_burst(self, station, shots, pulse, gap):
        """Fire `shots` strikes from ONE solenoid in a single request, `gap` ms apart (never under 4 x the pulse, the
        25 % duty bound `burst` holds), and count the impacts the mic stream shows (a 50 ms block whose peak sits
        TAP_DB over the quiet baseline, a run of such blocks counted once). Returns (impacts heard, gap used). No per-strike analysis: that is the
        point -- the capture path spends seconds per strike."""
        import urllib.request
        fs = {"LEFT": 0, "RIGHT": 1}[station]
        gap = max(int(gap), 4 * int(pulse), 100)
        ms = 400 + shots * gap + 800
        url = "http://%s/debug/listen.pcm?ms=%d&station=2&fn=%d&fp=%d&fg=%d&fs=%d" % (
            self.s.knobs["host"], ms, shots, pulse, gap, fs)
        self._drop_ws()
        blk = 2400                                            # 50 ms of frames, two words each: strikes 100 ms
        need = blk * 2 * 4                                    # apart (a 20 ms pulse) must land in different blocks
        peaks = {"left_slot": [], "right_slot": []}
        hits, was_hit = 0, False
        with self._open_listen(url, ms / 1000.0 + 20) as r:
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
                hit = False
                for slot, arr in (("left_slot", w[0::2]), ("right_slot", w[1::2])):
                    h = wa.mic_health(arr)
                    pk = h["peak_dbfs"] if h["peak_dbfs"] is not None else -999.0
                    base = np.median(peaks[slot][-8:]) if len(peaks[slot]) >= 2 else None
                    this_hit = h["state"] != "DEAD" and base is not None and pk > base + TAP_DB
                    hit = hit or this_hit
                    if h["state"] != "DEAD" and not this_hit:
                        peaks[slot].append(pk)
                hits += 1 if (hit and not was_hit) else 0       # one impact = a run of loud blocks, counted once
                was_hit = hit
        return min(hits, shots), gap

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
