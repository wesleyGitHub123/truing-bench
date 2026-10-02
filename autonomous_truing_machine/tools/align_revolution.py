"""Drive one full wheel revolution through the alignment campaign, spoke by spoke (lab image, laser receiver, roller).

    python tools/align_revolution.py            # from the firmware root; the board must be on s3_devkit_lab

Spokes 0..31 in order, the per-class recipe by spoke mod 4 (docs/ALIGN_CAMPAIGN.md), each trial its own workbench
invocation, a 5-strike 60 ms burst at each, and NO return move. The counter stops the run at 32, which is spoke 0 again.

PRECONDITIONS (this script moves the roller and fires the solenoids as soon as it starts):
  - the board is on `s3_devkit_lab`, reachable at the workbench's host, the roller wired and powered;
  - spoke 0 has been placed by hand just BEFORE the beam, the beam reads CLEAR, and nothing else is using the board;
  - session `align1` (below) holds the workbench's alignment settings (`index_dir +1`, `max`, slow ramp `200 1000`,
    `pluck always`), i.e. it is the session the campaign ran in.

It stops on the FIRST failed check and commands nothing further (the roller is left energised and idle): the trial
targeted the wrong spoke; a laser-guided class did not report ALIGNED; the blocked width left 55-80 steps (the signature of
the driver's step scale changing); the stop distance after the trigger left its per-class value by more than 20 steps; or
the trigger spacing broke the wheel's repeating 4-spoke pattern.
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SESSION = "align1"
N_SPOKES = 32
# class -> (name, speed steps/s, accel steps/s2, offset steps, strategy, expected stop distance after the trigger)
RECIPE = {
    0: ("NDS-leading", 1000, 1600, 0, "REACQUIRE", 314),
    1: ("DS-leading", 800, 1600, -20, "REACQUIRE_CLEAR", 201),
    2: ("NDS-trailing", 500, 500, 0, "STOP", 251),
    3: ("DS-trailing", 500, 600, 0, "STOP", 209),
}
# trigger-to-trigger gap after spoke k mod 4 (centre, tolerance), measured on the bench wheel 2026-10-03
GAP = {0: (1985, 220), 1: (700, 100), 2: (1963, 220), 3: (3290, 260)}


def wb(cmds, timeout=150):
    args = [sys.executable, os.path.join("tools", "workbench.py"), "--session", SESSION]
    for c in cmds:
        args += ["-c", c]
    p = subprocess.run(args, cwd=ROOT, capture_output=True, text=True, timeout=timeout)
    return p.stdout + p.stderr


def log(s):
    print(s, flush=True)


def stop(why, out=""):
    log("\n#### RUN STOPPED: %s" % why)
    if out:
        log(out)
    log("#### roller left energised and idle; nothing further was commanded")
    sys.exit(1)


def main():
    out = wb(["roll on", "align spoke 31"])
    if "spoke 31" not in out:
        stop("could not declare the start", out)
    prev_trigger = None
    counter = 0
    for k in range(N_SPOKES):
        name, hz, acc, off, strat, exp_over = RECIPE[k % 4]
        out = wb(["roll speed %d %d" % (hz, acc), "set pulse 60", "align set off %d" % off, "align set center no",
                  "align set pluckmode burst", "align set plucks 5",
                  "align trial %s 1 %s" % (name, strat)])
        m = re.search(r"== TRIAL stage \S+\s+(\S+) spoke (\d+) \((\w+) station\), break (\d+)", out)
        trig = re.search(r"TRIGGER break #\d+ at step (-?\d+)", out)
        tri = re.search(r"\[TRIAL\] (\w+)\s+-- .*overshoot (-?\d+) steps, final (-?\d+) steps from the span middle "
                        r"\(width (\S+?)\)", out)
        if not (m and trig and tri):
            stop("spoke %d: trial did not complete or the output was unreadable" % k, out)
        idx, brk, trigger = int(m.group(2)), int(m.group(4)), int(trig.group(1))
        over, final, width = int(tri.group(2)), int(tri.group(3)), tri.group(4)
        if idx != k:
            stop("expected spoke %d but the trial targeted spoke %d" % (k, idx), out)
        if strat in ("REACQUIRE", "REACQUIRE_CLEAR") and "[ALIGN] ALIGNED" not in out:
            stop("spoke %d: %s did not report ALIGNED" % (k, strat), out)
        if not width.isdigit() or not 55 <= int(width) <= 80:
            stop("spoke %d: blocked width %s outside 55-80 (step scale changed or the beam is not clean)" % (k, width), out)
        if abs(over - exp_over) > 20:
            stop("spoke %d: stop %d steps after trigger, expected %d" % (k, over, exp_over), out)
        spacing = ""
        if prev_trigger is not None:
            gap = trigger - prev_trigger
            centre, tol = GAP[(k - 1) % 4]
            spacing = "  spacing %d" % gap
            if abs(gap - centre) > tol:
                stop("spoke %d: trigger spacing %d from the last spoke, expected %d +-%d" % (k, gap, centre, tol), out)
        prev_trigger = trigger
        counter += 1
        extra = " ".join(l.strip() for l in out.splitlines() if "[BELIEF]" in l or "beam changed" in l or "!!" in l)
        log("spoke %2d/%d  %-12s %-15s trigger %6d  +%d  final %+d (width %s)%s  [break %d, %s]%s" % (
            k, N_SPOKES, name, strat, trigger, over, final, width, spacing, brk, m.group(3),
            ("   NOTE: " + extra) if extra else ""))
    log("\ncounter %d = spoke %d again: STOP. roller idle just past spoke 31." % (counter, counter % N_SPOKES))
    wb(["roll off"])
    log("roller off.")


if __name__ == "__main__":
    main()
