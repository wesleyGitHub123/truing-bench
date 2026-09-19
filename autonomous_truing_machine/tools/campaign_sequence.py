"""Trial sequences for the solenoid campaign (docs/SOLENOID_CAMPAIGN_PLAN.md B3.0 and B3.2).

campaign_runner.py fires whatever a plan file says; this module writes the plan. Keeping the two
apart means the randomised, seeded structure is generated once, can be committed before any data
exists, and is identified in every capture by the plan's sha256 -- so "which sequence produced this
trial" is a lookup, not a recollection.

    python tools/campaign_sequence.py b30 --seed 20260919 --out docs/campaign_plans/b30.json
    python tools/campaign_sequence.py b32 --seed 20260919 --out docs/campaign_plans/b32.json
    python tools/campaign_sequence.py --selftest

A plan is {"stage", "seed", "blocks": [{"name", "station", "prompt", "trials": [...]}]}. A trial is
{"kind", "spoke", "pulse_ms", "arm"}:

    kind      strike    the station's actuator strikes a real spoke (the spoke is positioned first)
              air_shot  the actuator fires with the wheel turned so the plunger lands between spokes
              no_fire   the capture and analysis run but nothing is excited
    spoke     the spoke id. For an air_shot or no_fire it exists only so the derived station is
              right (even -> LEFT, odd -> RIGHT); it is never attributed to a real spoke.
    pulse_ms  a one-shot width in whole ms, 0 = the excitation profile's. Only strikes and air shots.

Blocks exist because the operator has to turn the wheel between them; everything inside a block is
fired back to back with no one touching the wheel. Stdlib only.
"""
import argparse
import hashlib
import json
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import draw_spoke_sets  # noqa: E402  (the registered B3.1 draw is the source of the exploration spokes)

REGISTERED_SEED = 20260919
CLASS_NAMES = draw_spoke_sets.CLASS_NAMES
# Amendment 2: levels drawn from the measured M3 brackets -- LEFT [40, 85], RIGHT [60, 95], intersection
# [60, 85] -- so both stations share 60/72/85 and LEFT adds its own short edge.
B32_LEVELS = {"LEFT": [40, 60, 72, 85], "RIGHT": [60, 72, 85]}
NOMINAL_SPOKE = {"LEFT": 0, "RIGHT": 1}


def station_for_spoke(spoke):
    return "LEFT" if spoke % 2 == 0 else "RIGHT"


def trial(kind, spoke, pulse_ms=0, arm=""):
    return {"kind": kind, "spoke": spoke, "pulse_ms": pulse_ms, "arm": arm}


def plan_sha256(plan):
    """Canonical hash: what a capture's campaign_plan_sha256 refers to."""
    blob = json.dumps(plan, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(blob).hexdigest()


def shuffled_capped(rng, items, max_run):
    """A random order of `items` in which no more than max_run equal neighbours occur. Rejection
    sampling from the caller's seeded stream, so it is deterministic and terminates fast for the
    small lists here."""
    items = list(items)
    for _ in range(100000):
        rng.shuffle(items)
        run = 1
        ok = True
        for a, b in zip(items, items[1:]):
            run = run + 1 if a == b else 1
            if run > max_run:
                ok = False
                break
        if ok:
            return items
    raise ValueError("could not satisfy max_run=%d for %r" % (max_run, items))


def b30_plan(seed=REGISTERED_SEED, air_per_station=10, nofire_per_station=10, max_run=2):
    """B3.0: 20 no-fire + 10 air shots per station = 40 controls, interleaved within each station's
    block. The wheel stays where the operator put it for the block: air shots need it at a gap, a
    no-fire control does not care. Each block is 10 + 10 so the no-fire captures are tagged 10 per
    station, but they are one group for analysis -- nothing was excited, so they have no station."""
    rng = random.Random(seed)
    stations = ["LEFT", "RIGHT"]
    rng.shuffle(stations)
    blocks = []
    for st in stations:
        kinds = shuffled_capped(rng, ["air_shot"] * air_per_station + ["no_fire"] * nofire_per_station, max_run)
        blocks.append({
            "name": "B3.0-%s" % st,
            "station": st,
            "prompt": "%s block: turn the wheel so the %s plunger lands BETWEEN two spokes (an air shot must "
                      "hit nothing), and hold it there. The no-fire controls in this block ignore the wheel. "
                      "%d air shots and %d no-fire controls follow." % (st, st, air_per_station, nofire_per_station),
            "trials": [trial(k, NOMINAL_SPOKE[st]) for k in kinds],
        })
    return {"stage": "B3.0", "seed": seed, "blocks": blocks}


def b32_blocks_for_station(rng, station, spokes, levels, reps, control_every):
    blocks = []
    order = list(spokes)
    rng.shuffle(order)
    for sp in order:
        strikes = [lvl for lvl in levels for _ in range(reps)]
        rng.shuffle(strikes)   # condition order randomised within a spoke
        trials = []
        for i, lvl in enumerate(strikes, start=1):
            trials.append(trial("strike", sp, lvl))
            if i % (control_every - 1) == 0:
                trials.append(trial("no_fire", sp))   # every control_every-th trial is a control
        blocks.append({
            "name": "B3.2-%s-sp%d" % (station, sp),
            "station": station,
            "prompt": "Position spoke %d (%s) at the %s plunger and hold it there. %d strikes at %s ms in "
                      "random order, a no-fire control after every %d strikes." % (
                          sp, CLASS_NAMES[sp % 4], station, len(strikes),
                          "/".join(str(l) for l in levels), control_every - 1),
            "trials": trials,
        })
    return blocks


def b32_air_block(rng, station, levels, air_per_level):
    """Amendment 4: air shots AT THE TESTED WIDTHS, one block per station. B3.0's air shots used the profile's
    20 ms pulse, so nothing else tested whether a 40-85 ms actuation couples noise into the window. The wheel
    is turned to a gap (nothing under the plunger) and held; the widths are shuffled within the block."""
    widths = [lvl for lvl in levels for _ in range(air_per_level)]
    rng.shuffle(widths)
    return {
        "name": "B3.2-%s-air" % station,
        "station": station,
        "prompt": "%s AIR block: turn the wheel so the %s plunger lands BETWEEN two spokes (it must hit nothing), "
                  "and hold it there. %d air shots at %s ms in random order; no controls are interleaved (the "
                  "no-fire controls are in the strike blocks)." % (
                      station, station, len(widths), "/".join(str(l) for l in levels)),
        "trials": [trial("air_shot", NOMINAL_SPOKE[station], w) for w in widths],
    }


def b32_plan(seed=REGISTERED_SEED, levels=None, reps=6, control_every=4, draw_seed=draw_spoke_sets.REGISTERED_SEED,
             air_per_level=6):
    """B3.2 exploration: per station, the B3.1 exploration spokes x that station's levels x reps, with a
    no-fire control every control_every-th trial, plus (Amendment 4) air_per_level air shots at each of that
    station's levels in one block. Station order and spoke order are seeded from `seed` exactly as before the
    amendment; the air blocks draw from their own seeded stream and are inserted at a seeded position among
    their station's strike blocks, so the strike blocks are unchanged and the air shots are spread in time
    rather than all taken at the end. air_per_level=0 gives the pre-amendment plan."""
    levels = levels or B32_LEVELS
    sets = draw_spoke_sets.draw(draw_seed)
    rng = random.Random(seed)
    air_rng = random.Random("b32-air-%d" % seed)
    stations = ["LEFT", "RIGHT"]
    rng.shuffle(stations)
    blocks = []
    for st in stations:
        st_blocks = b32_blocks_for_station(rng, st, sets[st]["exploration"], levels[st], reps, control_every)
        if air_per_level:
            st_blocks.insert(air_rng.randint(0, len(st_blocks)), b32_air_block(air_rng, st, levels[st], air_per_level))
        blocks += st_blocks
    plan = {"stage": "B3.2", "seed": seed, "spoke_draw_seed": draw_seed, "levels": levels, "reps": reps,
            "control_every": control_every, "blocks": blocks}
    if air_per_level:
        plan["air_per_level"] = air_per_level
    return plan


def summarise(plan):
    counts = {}
    for b in plan["blocks"]:
        for t in b["trials"]:
            key = (b["station"], t["kind"], t["pulse_ms"])
            counts[key] = counts.get(key, 0) + 1
    return counts


def selftest():
    # --- B3.0 ---
    p = b30_plan(1)
    assert p == b30_plan(1), "same seed must give the same plan"
    assert p != b30_plan(2), "a different seed must give a different plan"
    kinds = [t["kind"] for b in p["blocks"] for t in b["trials"]]
    assert kinds.count("air_shot") == 20 and kinds.count("no_fire") == 20 and len(kinds) == 40
    for b in p["blocks"]:
        ks = [t["kind"] for t in b["trials"]]
        assert ks.count("air_shot") == 10 and ks.count("no_fire") == 10, b["name"]
        assert all(station_for_spoke(t["spoke"]) == b["station"] for t in b["trials"]), "spoke must derive the block's station"
        assert all(t["pulse_ms"] == 0 for t in b["trials"])
        run = 1
        for a, c in zip(ks, ks[1:]):
            run = run + 1 if a == c else 1
            assert run <= 2, "more than 2 of one kind in a row"
    assert sorted(b["station"] for b in p["blocks"]) == ["LEFT", "RIGHT"]
    # --- B3.2 ---
    q = b32_plan(1)
    assert q == b32_plan(1) and q != b32_plan(2)
    sets = draw_spoke_sets.draw(draw_spoke_sets.REGISTERED_SEED)
    strike_blocks = [b for b in q["blocks"] if not b["name"].endswith("-air")]
    air_blocks = [b for b in q["blocks"] if b["name"].endswith("-air")]
    assert len(strike_blocks) == 8 and len(air_blocks) == 2, (len(strike_blocks), len(air_blocks))
    for b in strike_blocks:
        st = b["station"]
        sp = {t["spoke"] for t in b["trials"]}
        assert len(sp) == 1 and sp <= set(sets[st]["exploration"]), "only B3.1 exploration spokes"
        levels = B32_LEVELS[st]
        strikes = [t for t in b["trials"] if t["kind"] == "strike"]
        assert len(strikes) == 6 * len(levels)
        for lvl in levels:
            assert sum(1 for t in strikes if t["pulse_ms"] == lvl) == 6, "6 strikes per level per spoke"
        assert {t["pulse_ms"] for t in strikes} == set(levels), "no strike outside the level set"
        for i, t in enumerate(b["trials"], start=1):
            if i % 4 == 0:
                assert t["kind"] == "no_fire" and t["pulse_ms"] == 0, "control every 4th trial"
            else:
                assert t["kind"] == "strike"
        assert all(station_for_spoke(t["spoke"]) == st for t in b["trials"])
    # Amendment 4: 6 air shots at each tested width, per station, nothing else in the block
    for b in air_blocks:
        st = b["station"]
        assert all(t["kind"] == "air_shot" and station_for_spoke(t["spoke"]) == st for t in b["trials"])
        assert {t["pulse_ms"] for t in b["trials"]} == set(B32_LEVELS[st])
        for lvl in B32_LEVELS[st]:
            assert sum(1 for t in b["trials"] if t["pulse_ms"] == lvl) == 6, "6 air shots per level"
    assert sorted(b["station"] for b in air_blocks) == ["LEFT", "RIGHT"]
    # the strike blocks are exactly what the plan was before the amendment (same seed, same order)
    old = b32_plan(1, air_per_level=0)
    assert [b for b in q["blocks"] if not b["name"].endswith("-air")] == old["blocks"], "strike blocks changed"
    assert "air_per_level" not in old and q["air_per_level"] == 6
    # each air block sits among its own station's blocks: the stations still change over exactly once
    names = [b["station"] for b in q["blocks"]]
    assert names.count("LEFT") == 5 and names.count("RIGHT") == 5
    assert sum(1 for a, c in zip(names, names[1:]) if a != c) == 1, names
    per_station = {}
    for b in strike_blocks:
        per_station.setdefault(b["station"], set()).add(b["trials"][0]["spoke"])
    assert per_station["LEFT"] == set(sets["LEFT"]["exploration"]) and per_station["RIGHT"] == set(sets["RIGHT"]["exploration"])
    # the capture-count envelope the plan states: ~168 strikes (Amendment 2), plus controls
    n_strikes = sum(1 for b in q["blocks"] for t in b["trials"] if t["kind"] == "strike")
    assert n_strikes == 4 * 6 * 4 + 3 * 6 * 4 == 168, n_strikes
    # --- hash ---
    assert plan_sha256(p) == plan_sha256(json.loads(json.dumps(p))), "hash must survive a JSON round trip"
    assert plan_sha256(p) != plan_sha256(q)
    print("selftest ok: B3.0 40 controls, B3.2 %d strikes + %d no-fire controls + %d air shots at the tested widths" % (
        n_strikes, sum(1 for b in q["blocks"] for t in b["trials"] if t["kind"] == "no_fire"),
        sum(1 for b in q["blocks"] for t in b["trials"] if t["kind"] == "air_shot")))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("stage", nargs="?", choices=["b30", "b32"])
    ap.add_argument("--seed", type=int, default=REGISTERED_SEED)
    ap.add_argument("--out", help="write the plan here (JSON); otherwise print a summary")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        selftest()
        return
    if args.stage is None:
        ap.error("stage (b30 or b32) or --selftest")
    plan = b30_plan(args.seed) if args.stage == "b30" else b32_plan(args.seed)
    digest = plan_sha256(plan)
    print("%s plan, seed %d, sha256 %s" % (plan["stage"], args.seed, digest))
    for (st, kind, pulse), n in sorted(summarise(plan).items()):
        print("  %-5s %-8s pulse %-3s x%d" % (st, kind, pulse or "-", n))
    if args.out:
        os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
        with open(args.out, "w", encoding="utf-8", newline="\n") as f:
            json.dump(plan, f, indent=1, sort_keys=True)
            f.write("\n")
        print("wrote %s" % args.out)


if __name__ == "__main__":
    main()
