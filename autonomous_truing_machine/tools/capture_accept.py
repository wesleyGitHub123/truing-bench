"""Promote a replayed result to the expectation a capture is regression-tested against.

test_acoustic_replay writes <name>.observed.json next to every bundle it replays. This copies
those expect_* values into <name>.json, so the next run asserts against them.

    python tools/capture_accept.py <name> [<name> ...]
    python tools/capture_accept.py --all --dry-run

Only for bundles whose expectations came from the host in the first place, or for a
deliberate re-baseline after an intended DSP change. It refuses a bundle whose expectations
came from the board (expect_origin "board") unless --force is given, because overwriting
those does not fix a host/target divergence - it hides it, and the fixture stops being
evidence about the board at the moment it would have become useful.
"""
import argparse
import json
import os
import sys

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CAPTURES = os.path.join(REPO_ROOT, "test", "fixtures", "acoustic", "captures")


def load(path):
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def accept(name, out_dir, force, dry_run):
    bundle_path = os.path.join(out_dir, name + ".json")
    observed_path = os.path.join(out_dir, name + ".observed.json")
    if not os.path.exists(bundle_path):
        print("%s: no bundle" % name)
        return False
    if not os.path.exists(observed_path):
        print("%s: no observed result - run the replay test first" % name)
        return False

    bundle, observed = load(bundle_path), load(observed_path)
    origin = bundle.get("expect_origin")
    # "board" is host/target parity evidence; "golden" is the research pipeline's own reference
    # for a recorded excerpt. Overwriting either with a host result does not resolve a
    # disagreement, it deletes the record of one.
    if origin in ("board", "golden") and not force:
        print("%s: expectations came from the %s. Overwriting them would hide a divergence\n"
              "    rather than explain it. Use --force only if you know why they differ."
              % (name, "BOARD" if origin == "board" else "RESEARCH REFERENCE"))
        return False

    changes = {k: v for k, v in observed.items() if k.startswith("expect_") and bundle.get(k) != v}
    if not changes:
        print("%s: already matches" % name)
        return False
    for key, value in sorted(changes.items()):
        print("%s: %s %r -> %r" % (name, key, bundle.get(key), value))
    if dry_run:
        return False
    bundle.update(changes)
    if origin in ("board", "golden") and force:
        # The bundle stops being parity evidence the moment its numbers are overwritten; say so
        # in the file rather than leaving a board-labelled fixture carrying host numbers.
        bundle["expect_origin"] = "host"
        bundle["expect_note"] = "re-baselined on the host; original board expectations overwritten"
    with open(bundle_path, "w", encoding="utf-8") as f:
        json.dump(bundle, f, indent=2, sort_keys=True)
        f.write("\n")
    return True


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("names", nargs="*")
    p.add_argument("--all", action="store_true", help="every bundle in index.txt")
    p.add_argument("--dir", default=CAPTURES)
    p.add_argument("--force", action="store_true", help="re-baseline even board-sourced expectations")
    p.add_argument("--dry-run", action="store_true", help="show what would change")
    args = p.parse_args()

    names = list(args.names)
    if args.all:
        index = os.path.join(args.dir, "index.txt")
        with open(index, "r", encoding="utf-8") as f:
            names += [ln.strip() for ln in f if ln.strip() and not ln.startswith("#")]
    if not names:
        p.error("name a bundle, or pass --all")

    written = sum(1 for n in names if accept(n, args.dir, args.force, args.dry_run))
    print("%d bundle(s) updated" % written)
    return 0


if __name__ == "__main__":
    sys.exit(main())
