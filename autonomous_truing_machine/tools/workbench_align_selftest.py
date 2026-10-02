"""Stage 0's count analysis (workbench.count_analysis), no hardware and no network.

    python tools/workbench_align_selftest.py

The first bench count runs (2026-10-02) saw the beam break 33 times in 32 spokes' travel with spacings that repeat
every 4 spokes (~1960, 715, 1960, 3280 steps), because the beam sits where spokes cross. The analysis once judged
every spacing against one global median and called the 8 short gaps "extra" and the 8 long gaps "missed". This
pins: a repeating pattern is NOT a miscount, a uniform wheel is period 1, and a real missed or extra break is
still reported.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import workbench as wb      # noqa: E402

BENCH_PATTERN = [1963, 718, 1963, 3255]


def edges_for(gaps, width=67, start=1000):
    out, pos = [], start
    for first in (True,) + (False,) * len(gaps):
        if not first:
            pos += gaps[len(out) // 2 - 1]
        out.append({"phase": "APPROACH", "sign": 1, "enter": True, "pos": pos})
        out.append({"phase": "APPROACH", "sign": 1, "enter": False, "pos": pos + width})
    return out


def analyse(gaps):
    return wb.count_analysis(edges_for(gaps), False, 32)


def main():
    a = analyse(BENCH_PATTERN * 8)
    assert len(a["enters"]) == 33
    assert a["period"] == 4, a["period"]
    assert a["missed"] == 0 and a["duplicates"] == 0, (a["missed"], a["duplicates"])
    assert a["rev_steps"] == sum(BENCH_PATTERN) * 8, a["rev_steps"]
    assert a["pitch_min"] == 718 and a["width_med"] == 67

    # noisy but the same pattern (the bench spread is a few percent)
    noisy = [g + d for g, d in zip(BENCH_PATTERN * 8, [0, 7, -12, 25, -9, 3, 18, -30] * 4)]
    a = analyse(noisy)
    assert a["period"] == 4 and a["missed"] == 0 and a["duplicates"] == 0

    a = analyse([2000] * 32)
    assert a["period"] == 1 and a["missed"] == 0 and a["duplicates"] == 0

    g = [2000] * 32
    g[10] = 4000                       # a spoke the beam never saw
    a = analyse(g)
    assert a["missed"] == 1 and a["duplicates"] == 0, (a["missed"], a["duplicates"])

    g = [2000] * 32
    g[10:11] = [700, 1300]             # a spurious extra break
    a = analyse(g)
    assert a["duplicates"] == 1, a["duplicates"]

    # The beam level at the start of a trial vs where the last trial left the wheel. 2026-10-03: a REACQUIRE_CLEAR trial
    # leaves spoke 1 ahead, the beam clear (belief = spoke 0, behind). If a strike nudged spoke 1 into the beam the
    # next trial's first break would be spoke 2, not spoke 1, and the stop would come a spoke late.
    assert wb.reconcile_belief(0, False, False, 1, 32) == (0, False)         # as expected: clear, spoke 0 behind
    assert wb.reconcile_belief(0, False, True, 1, 32) == (1, True)           # spoke 1 is in the beam: belief moves on
    assert wb.reconcile_belief(0, False, True, -1, 32) == (31, True)         # the other index direction
    assert wb.reconcile_belief(5, True, True, 1, 32) == (5, True)            # left in the beam, still in it
    assert wb.reconcile_belief(7, None, True, 1, 32) == (7, None)            # a declared spoke: no expectation
    try:
        wb.reconcile_belief(5, True, False, 1, 32)                           # left in the beam, now clear
    except ValueError:
        pass
    else:
        raise AssertionError("a clear beam where a spoke was left in it must be refused")

    # The driver's microstep read-back (2026-10-03: a reset halved every step count without a word): the state line
    # has to say so, and say nothing about a healthy driver.
    healthy = {"enabled": True, "moving": False, "position_steps": 0, "mres_ok": True, "mres_repairs": 0}
    assert "MICROSTEPS" not in wb.drive_line(healthy) and "rewritten" not in wb.drive_line(healthy)
    assert "MICROSTEPS REVERTED" in wb.drive_line(dict(healthy, mres_ok=False))
    assert "rewritten 2 time(s)" in wb.drive_line(dict(healthy, mres_repairs=2))
    assert "MICROSTEPS" not in wb.drive_line({"enabled": True, "moving": False})     # an older firmware: no claim

    print("selftest ok: bench 4-spoke spacing pattern is not a miscount; uniform is period 1; "
          "a missed and an extra break are still reported; belief vs beam level reconciles; "
          "a reverted microstep setting is flagged")


if __name__ == "__main__":
    main()
