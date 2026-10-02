"""Drive one full wheel revolution through the alignment campaign, spoke by spoke, and RECOVER from the bench's known
failures instead of stopping on them (lab image, laser receiver, roller, both solenoids).

    python tools/align_revolution.py            # from the firmware root; the board must be on s3_devkit_lab
    python tools/align_revolution.py 5          # start at spoke 5 (spokes 0-4 were done; the wheel is near spoke 5)

Spokes 0..31 in order, the per-class recipe by spoke mod 4 (docs/ALIGN_CAMPAIGN.md), a 5-strike 60 ms burst at each,
NO return move. The run ends when the counter reaches 32, which is spoke 0 again.

PRECONDITIONS (this script moves the roller and fires the solenoids as soon as it starts):
  - the board is on `s3_devkit_lab`, the roller, laser, mics and solenoids wired and powered;
  - spoke 0 (or the start spoke) is just BEFORE the beam, within a few hundred steps of either side is fine;
  - session `align1` holds the alignment settings (`index_dir +1`, `max`, slow ramp `200 1000`).

How it stays on the right spoke. The wheel's spoke spacing at the beam repeats every 4 spokes (about 1980, 700, 1965,
3290 steps), so the distance from the last trusted spoke to a new trigger says WHICH spoke triggered. Every stop is
identified that way BEFORE anything strikes; the beam level is only the board's first guess. The failures this has seen
on the bench, and what the script does about each:

  a strike nudged the next spoke INTO the beam    -> identified as one spoke late: back up before the right spoke, retry
  a strike pushed the spoke OUT past the beam     -> the workbench assumes "pushed on"; the identification confirms it
  the stop landed on another spoke for any reason -> same: back up to just before the right spoke, retry, no strike
  beam chatter (thousands of edges at a stop)     -> trial discarded; rescan to re-locate; halts if the beam chatters
                                                     with the wheel still (electrical: the receiver or its supply)
  the driver's microstep setting reverted         -> `roll on` rewrites it; positions are re-located by a rescan
  the board rebooted / the Wi-Fi dropped          -> waits for it, rejoins, `roll on`, re-locates by a rescan
  a laser-guided class did not end ALIGNED, or a  -> back up before the same spoke and retry
  stop distance is off its class value

Re-locating = a disarmed laser scan 8500 steps BACK (about one 4-spoke period), which reads the spacing pattern of the
spokes behind, so each one's class is known; the run's own count picks the index. It never strikes during recovery.
A spoke is retried at most 3 times, and the run stops after 12 recoveries in total: then something physical is wrong.
"""
import json
import os
import re
import subprocess
import sys
import time
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SESSION = "align1"
HOST = "192.168.4.1"
SSID = "truing-xxxxxx"
N_SPOKES = 32
# class -> (name, speed steps/s, accel steps/s2, offset steps, strategy, expected stop distance after the trigger)
RECIPE = {
    0: ("NDS-leading", 1000, 1600, 0, "REACQUIRE", 314),
    1: ("DS-leading", 800, 1600, -20, "REACQUIRE_CLEAR", 201),
    2: ("NDS-trailing", 500, 500, 0, "STOP", 251),
    3: ("DS-trailing", 500, 600, 0, "STOP", 209),
}
GAP = [1980, 700, 1965, 3290]    # enter-to-enter gap after a spoke of class c, measured on the bench wheel 2026-10-03
ID_TOL = 250                     # a trigger this close to a spoke's expected edge is that spoke (half the 700 gap is 350)
SCAN_BACK = 8500                 # one 4-spoke period (~7935) and a bit: at least 3 spokes, 2 gaps
MAX_SPOKE_RETRIES = 3
MAX_RECOVERIES = 12
CHATTER_EDGES = 60               # a trial or scan makes ~4-12 edges; the chatter episodes made thousands


def log(s):
    print(s, flush=True)


# ---- the board, directly (read-only) --------------------------------------------------------------------------
def get_json(path, timeout=6):
    with urllib.request.urlopen("http://%s/%s" % (HOST, path), timeout=timeout) as r:
        return json.loads(r.read().decode("utf-8"))


def board_up(wait_s=150):
    """/id once the board answers, rejoining its Wi-Fi if the laptop dropped off. None if it never comes back."""
    t_end = time.time() + wait_s
    tried_join = False
    while time.time() < t_end:
        try:
            return get_json("id", 4)
        except OSError:
            if not tried_join or time.time() > t_end - wait_s / 2:
                subprocess.run(["netsh", "wlan", "connect", "name=%s" % SSID], capture_output=True)
                tried_join = True
            time.sleep(4)
    return None


def laser():
    return get_json("debug/laser.json")


def drive():
    return get_json("debug/drive.json")


# ---- the workbench (everything that moves or strikes) --------------------------------------------------------
def wb(cmds, timeout=180):
    args = [sys.executable, os.path.join("tools", "workbench.py"), "--session", SESSION]
    for c in cmds:
        args += ["-c", c]
    try:
        p = subprocess.run(args, cwd=ROOT, capture_output=True, text=True, timeout=timeout)
        return p.stdout + p.stderr
    except subprocess.TimeoutExpired as e:
        return "!! workbench timed out after %d s: %s" % (timeout, (e.stdout or b"")[-400:])


def problems(out):
    return " | ".join(l.strip() for l in out.splitlines() if l.strip().startswith("!!"))


# ---- geometry ----------------------------------------------------------------------------------------------------
def offset(a, b):
    """Steps from spoke a's entering edge to spoke b's (unwrapped spoke numbers; negative when b is behind a)."""
    if b >= a:
        return sum(GAP[i % 4] for i in range(a, b))
    return -sum(GAP[i % 4] for i in range(b, a))


def identify(trigger, anchor):
    """Which spoke's edge is `trigger`, given a trusted (edge position, spoke). (spoke, error) or (None, error)."""
    pos, sp = anchor
    best = min(range(sp - 2, sp + 7), key=lambda j: abs(trigger - pos - offset(sp, j)))
    err = trigger - pos - offset(sp, best)
    return (best if abs(err) <= ID_TOL else None), err


class Run:
    def __init__(self, first):
        self.k = first              # the next spoke to align and strike (unwrapped: 0..31)
        self.anchor = None          # (entering-edge position, spoke) of the last spoke the run trusts
        self.uptime = None
        self.retries = 0
        self.recoveries = 0
        self.need_relocate = True   # always at the start: it also checks the hand placement

    def halt(self, why, out=""):
        log("\n#### RUN STOPPED at spoke %d: %s" % (self.k, why))
        if out:
            log(out)
        log("#### roller left energised and idle; resume with: python tools/align_revolution.py %d" % self.k)
        wb(["align set pluck always"])
        sys.exit(1)

    def recovered(self, what):
        self.recoveries += 1
        self.retries += 1
        log("   >> RECOVER (%d of %d, spoke %d try %d): %s" % (self.recoveries, MAX_RECOVERIES, self.k, self.retries + 1,
                                                                what))
        if self.recoveries > MAX_RECOVERIES:
            self.halt("too many recoveries: something physical is wrong (roller contact, beam, supply)")
        if self.retries >= MAX_SPOKE_RETRIES:
            self.halt("spoke %d failed %d times in a row" % (self.k, self.retries))

    # -- health: reboot, Wi-Fi, driver -------------------------------------------------------------------------------
    def health(self):
        ident = board_up()
        if ident is None:
            self.halt("the board does not answer (power? USB? Wi-Fi?)")
        if "drive" not in ident.get("mode", ""):
            self.halt("the board is not on the lab image (mode %s)" % ident.get("mode"))
        up = int(ident.get("uptime_s", 0))
        if self.uptime is not None and up < self.uptime:
            log("   !! the board REBOOTED (uptime %d s): its step counter restarted; re-locating" % up)
            self.need_relocate = True
            self.recoveries += 1
        self.uptime = up
        d = drive()
        if not d.get("uart_ok"):
            self.halt("the roller driver does not answer on its UART (wiring or its supply)")
        if not d.get("enabled") or d.get("faulted") or d.get("mres_ok") is False:
            why = ("its microstep setting had reverted (driver reset)" if d.get("mres_ok") is False else
                   "it was faulted" if d.get("faulted") else "its coils were off")
            log("   !! roller: %s -> `roll on`%s" % (why, "; re-locating" if d.get("mres_ok") is False else ""))
            if d.get("mres_ok") is False:
                self.need_relocate = True
            out = wb(["roll on"])
            d = drive()
            if not d.get("enabled") or d.get("faulted") or d.get("mres_ok") is False:
                self.halt("`roll on` did not bring the driver back", out)

    def chatter_now(self):
        """Is the beam chattering with the wheel still? (electrical: nothing the sequence can fix)"""
        a = laser()
        time.sleep(1.5)
        b = laser()
        return int(b.get("edges", 0)) - int(a.get("edges", 0)) > 4

    # -- re-locate by scanning back ------------------------------------------------------------------------------
    def relocate(self):
        k = self.k
        log("   [LOCATE] scanning %d steps back to read the spacing pattern behind the beam (no strikes)" % SCAN_BACK)
        e0 = int(laser().get("edges", 0))
        out = wb(["roll speed 800 1600", "laser scan -%d" % SCAN_BACK])
        if int(laser().get("edges", 0)) - e0 > CHATTER_EDGES:
            if self.chatter_now():
                self.halt("the beam chatters with the wheel still: check the receiver wiring and its supply")
            log("   [LOCATE] the scan saw a burst of chatter; trying again")
            return False
        enters = sorted(int(m.group(1)) for m in re.finditer(r"BREAK \(beam blocked\)\s+at step\s+(-?\d+)", out))
        if len(enters) < 3:
            log("   [LOCATE] only %d spokes seen in the scan (%s)" % (len(enters), problems(out) or "no error"))
            return False
        gaps = [b - a for a, b in zip(enters, enters[1:])]
        fits = []
        for p in range(4):          # the class of the first (furthest back) spoke seen
            err = max(abs(g - GAP[(p + i) % 4]) for i, g in enumerate(gaps))
            if err <= ID_TOL:
                fits.append((err, p))
        if not fits:
            log("   [LOCATE] the gaps %s do not fit the wheel's pattern %s" % (gaps, GAP))
            return False
        p = min(fits)[1]
        last_cls = (p + len(enters) - 1) % 4
        # The nearest spoke behind the start is normally k-1. Its class fixes it within 4: take k-2..k+1 (being past
        # spoke k after a reboot mid-move is likelier than being two spokes short of where the run thinks it is).
        last = next(j for j in range(k - 2, k + 2) if j % 4 == last_cls)
        self.anchor = (enters[-1], last)
        log("   [LOCATE] gaps %s -> the nearest spoke behind is spoke %d (%s), edge at step %d" % (
            gaps, last % N_SPOKES, RECIPE[last % 4][0], enters[-1]))
        return self.move_before(k)

    def move_before(self, k):
        """Put the wheel in the gap just before spoke k (half the gap from spoke k-1) and declare spoke k-1 behind."""
        pos, sp = self.anchor
        target = pos + offset(sp, k) - GAP[(k - 1) % 4] // 2
        here = int(drive().get("position_steps", 0))
        out = wb(["roll speed 800 1600", "roll %d" % (target - here)])
        if laser().get("beam") != "CLEAR":
            log("   [MOVE] the beam is BLOCKED where the gap before spoke %d should be (%s)" % (
                k % N_SPOKES, problems(out) or "the model is off"))
            self.need_relocate = True
            return False
        wb(["align spoke %d" % ((k - 1) % N_SPOKES)])
        log("   [MOVE] wheel at step %d, in the gap before spoke %d" % (target, k % N_SPOKES))
        return True

    # -- one spoke ------------------------------------------------------------------------------------------------
    def spoke(self):
        k = self.k
        name, hz, acc, off, strat, exp_over = RECIPE[k % 4]
        z0 = laser()
        out = wb(["roll speed %d %d" % (hz, acc), "set pulse 60", "align set off %d" % off, "align set center no",
                  "align set pluckmode burst", "align set plucks 5", "align set pluck never",
                  "align trial %s 1 %s" % (name, strat)])
        z1 = laser()
        trig = re.search(r"TRIGGER break #\d+ at step (-?\d+)", out)
        tri = re.search(r"\[TRIAL\] (\w+)\s+-- .*overshoot (-?\d+) steps, final (-?\d+) steps from the span middle "
                        r"\(width (\S+?)\)", out)
        edges = int(z1.get("edges", 0)) - int(z0.get("edges", 0))
        if edges > CHATTER_EDGES or int(z1.get("glitches", 0)) - int(z0.get("glitches", 0)) > 5:
            if self.chatter_now():
                self.halt("the beam chatters with the wheel still: check the receiver wiring and its supply", out)
            self.need_relocate = True
            return self.recovered("beam chatter during the trial (%d edges): discarded, re-locating" % edges)
        if not (trig and tri):
            self.need_relocate = True
            return self.recovered("the trial did not complete (%s)" % (problems(out) or "no trigger"))
        trigger, over, final, width = int(trig.group(1)), int(tri.group(2)), int(tri.group(3)), tri.group(4)
        j, err = identify(trigger, self.anchor)
        if j is None:
            self.need_relocate = True
            return self.recovered("trigger at %d fits no spoke (%+d from the nearest): re-locating" % (trigger, err))
        if j != k:
            self.anchor = (trigger, j)
            self.recovered("it stopped on spoke %d, not %d (a nudged spoke): no strike, backing up" % (
                j % N_SPOKES, k % N_SPOKES))
            self.move_before(k)
            return
        self.anchor = (trigger, k)
        if not width.isdigit() or not 55 <= int(width) <= 80:
            self.need_relocate = True
            return self.recovered("blocked width %s (not ~65): the step scale or the beam is off" % width)
        if strat in ("REACQUIRE", "REACQUIRE_CLEAR") and "[ALIGN] ALIGNED" not in out:
            self.recovered("%s did not end ALIGNED: backing up to retry" % strat)
            self.move_before(k)
            return
        if abs(over - exp_over) > 20:
            self.recovered("stop %d steps after the trigger, not %d: backing up to retry" % (over, exp_over))
            self.move_before(k)
            return
        p = wb(["set pulse 60", "align pluck %d" % (k % N_SPOKES)])
        heard = re.search(r"(\d+) of (\d+) strikes heard", p)
        moved = "  (the strikes moved the spoke)" if "beam changed" in p else ""
        if "!!" in p:
            moved += "  !! strike: " + problems(p)
        log("spoke %2d/%d  %-12s %-15s trigger %6d  +%d  final %+d (width %s)  id %+d  [%s]%s" % (
            k % N_SPOKES, N_SPOKES, name, strat, trigger, over, final, width, err,
            "struck" if heard else "strike?", moved))
        self.k += 1
        self.retries = 0

    def go(self):
        while self.k < N_SPOKES:
            self.health()
            if self.need_relocate:
                self.need_relocate = False
                if not self.relocate():
                    self.need_relocate = True
                    self.recovered("re-locating failed")
                    continue
            self.spoke()
        log("\ncounter %d = spoke 0 again: STOP. %d recoveries on the way." % (self.k, self.recoveries))
        wb(["align set pluck always", "roll off"])
        log("roller off.")


def main():
    first = int(sys.argv[1]) if len(sys.argv) > 1 else 0
    if not 0 <= first < N_SPOKES:
        sys.exit("start spoke must be 0..%d" % (N_SPOKES - 1))
    Run(first).go()


if __name__ == "__main__":
    main()
