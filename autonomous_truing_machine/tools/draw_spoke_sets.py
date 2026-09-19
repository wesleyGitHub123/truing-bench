"""B3.1 -- draw the class-balanced spoke sets (docs/SOLENOID_CAMPAIGN_PLAN.md, Amendment 1 C5).

Per station and per class: 2 exploration, 2 held-out, 1 reserve spoke. Run BEFORE any strike data
exists and commit the output, so which spokes were held out is fixed before anyone has seen how
any spoke behaves.

    python tools/draw_spoke_sets.py            # the registered seed
    python tools/draw_spoke_sets.py --seed N   # another draw, e.g. after a spoke turns out to be unreachable

The class of spoke i is (i mod 4), per the class map B2-M8 verified: 0 LEFT-leading, 1 RIGHT-leading,
2 LEFT-trailing, 3 RIGHT-trailing. A class therefore has n_spokes/4 = 8 spokes, evenly spaced around
the rim (every 4th index, 45 degrees apart on a 32-spoke wheel), numbered 0..7 by position k, spoke =
class + 4k.

"Spread around the wheel within each class" is made operational as: the two spokes of a role
(exploration pair, held-out pair) sit at least MIN_SEPARATION positions apart on that ring of 8, so a
role never lands on neighbouring spokes. Roles are drawn in the order exploration, held-out, reserve,
each from what is left; a draw that cannot place a pair is retried from the same random stream.
Stdlib only.
"""
import argparse
import random

N_SPOKES = 32
REGISTERED_SEED = 20260919   # the date it was drawn -- chosen before running, not tuned
MIN_SEPARATION = 3           # of 8 positions; 4 is diametrically opposite
CLASS_NAMES = {0: "LEFT-leading", 1: "RIGHT-leading", 2: "LEFT-trailing", 3: "RIGHT-trailing"}
STATION_CLASSES = {"LEFT": (0, 2), "RIGHT": (1, 3)}


def ring_distance(a, b, n):
    d = abs(a - b) % n
    return min(d, n - d)


def draw_class(rng, cls, n_spokes):
    n = n_spokes // 4
    while True:
        free = list(range(n))
        rng.shuffle(free)
        picked = {}
        ok = True
        for role, count in (("exploration", 2), ("held_out", 2), ("reserve", 1)):
            chosen = []
            for k in list(free):
                if len(chosen) == count:
                    break
                if all(ring_distance(k, c, n) >= MIN_SEPARATION for c in chosen):
                    chosen.append(k)
            if len(chosen) != count:
                ok = False
                break
            free = [k for k in free if k not in chosen]
            picked[role] = sorted(cls + 4 * k for k in chosen)
        if ok:
            return picked


def draw(seed, n_spokes=N_SPOKES):
    rng = random.Random(seed)
    sets = {}
    for station, classes in STATION_CLASSES.items():
        sets[station] = {role: [] for role in ("exploration", "held_out", "reserve")}
        for cls in classes:
            for role, spokes in draw_class(rng, cls, n_spokes).items():
                sets[station][role] += spokes
        for role in sets[station]:
            sets[station][role].sort()
    return sets


def check(sets, n_spokes=N_SPOKES):
    """The properties the plan asks for, asserted rather than assumed."""
    every = []
    for station, roles in sets.items():
        assert [len(roles[r]) for r in ("exploration", "held_out", "reserve")] == [4, 4, 2], station
        for role, spokes in roles.items():
            for cls in STATION_CLASSES[station]:
                assert sum(1 for sp in spokes if sp % 4 == cls) == (1 if role == "reserve" else 2), (station, role, cls)
            every += spokes
    assert len(every) == len(set(every)), "a spoke was assigned to two roles"
    assert all(0 <= sp < n_spokes for sp in every)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--seed", type=int, default=REGISTERED_SEED)
    args = ap.parse_args()
    sets = draw(args.seed)
    check(sets)
    print("seed %d, min separation %d of 8, %d spokes" % (args.seed, MIN_SEPARATION, N_SPOKES))
    for station, roles in sets.items():
        print("%s (classes %s)" % (station, ", ".join(CLASS_NAMES[c] for c in STATION_CLASSES[station])))
        for role, spokes in roles.items():
            print("  %-12s %s" % (role, spokes))
    used = sorted(sp for roles in sets.values() for spokes in roles.values() for sp in spokes)
    print("unused: %s" % [sp for sp in range(N_SPOKES) if sp not in used])


if __name__ == "__main__":
    main()
