"""Turn a directory of capture bundles into a readable campaign summary.

campaign_recorder.py and campaign_runner.py both write the same flat bundle schema
(tools/capture_fetch.py's writer) into a directory with an index.txt. Neither tool decides
anything about what it collected -- this is the first read of that pile: how many shots per
spoke, how many reached each status, and what the front end's own diagnostics (SPEC §9.4 /
the audio-report fields added alongside MEASURE_ONCE) said about the capture path itself.

    python tools/campaign_report.py test/fixtures/acoustic/captures/_campaign
    python tools/campaign_report.py test/fixtures/acoustic/captures/_campaign --pass B2-M3

Prints a plain-text report to stdout. --pass filters to one campaign_label; omit it to
summarise everything the directory holds, grouped by whatever labels are present. Bundles
older than the audio-report/debug_triggered fields (anything predating commits d412bc1 /
e0f6d34) are counted normally; their extra fields just show as "n/a" rather than 0, so an old
and a new bundle are never silently conflated.

This does not decide whether a campaign passed anything -- a passing acceptance criterion
lives in the campaign plan (SOLENOID_CAMPAIGN.md's convention), not here. It only says what
was observed.
"""
import argparse
import json
import os
import statistics
import sys


def load_bundles(directory, label_filter=None):
    """Yields (name, doc) for every indexed bundle, skipping any that failed to parse."""
    index_path = os.path.join(directory, "index.txt")
    if not os.path.exists(index_path):
        raise SystemExit("no index.txt in %s -- nothing to report on" % directory)
    with open(index_path, "r", encoding="utf-8") as f:
        names = [ln.strip() for ln in f if ln.strip() and not ln.strip().startswith("#")]
    for name in names:
        json_path = os.path.join(directory, name + ".json")
        if not os.path.exists(json_path):
            print("!! %s: indexed but %s.json is missing" % (name, name), file=sys.stderr)
            continue
        with open(json_path, "r", encoding="utf-8") as f:
            try:
                doc = json.load(f)
            except ValueError as e:
                print("!! %s: %s.json did not parse (%s)" % (name, name, e), file=sys.stderr)
                continue
        if label_filter is not None and doc.get("campaign_label") != label_filter:
            continue
        yield name, doc


def group_of(doc):
    """(sort_key, label) of what a capture belongs to. A B3.0 no-fire control or air shot carries a nominal
    spoke id -- it exists only so the derived station is right -- and must never be counted under that
    spoke alongside real strikes, so controls form their own groups."""
    kind = doc.get("campaign_kind")
    if kind == "no_fire":
        return (1, "control: no_fire")
    if kind == "air_shot":
        return (2, "control: air_shot at %s" % doc.get("campaign_station", "?"))
    spoke = doc.get("spoke_id", "?")
    return (0, "spoke %s" % spoke) if not isinstance(spoke, int) else (0, "spoke %03d" % spoke)


def fmt_stat(values, digits=2):
    values = [v for v in values if v is not None]
    if not values:
        return "n/a"
    if len(values) == 1:
        return "%.*f (n=1)" % (digits, values[0])
    return "%.*f - %.*f, mean %.*f (n=%d)" % (
        digits, min(values), digits, max(values), digits, statistics.mean(values), len(values))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("directory", help="a bundle directory with an index.txt")
    p.add_argument("--pass", dest="pass_", default=None, help="only this campaign_label")
    p.add_argument("--out", default=None, help="write the report here too (default: stdout only)")
    args = p.parse_args()

    bundles = list(load_bundles(args.directory, args.pass_))
    if not bundles:
        raise SystemExit("no bundles matched in %s%s" %
                          (args.directory, " for --pass %r" % args.pass_ if args.pass_ else ""))

    lines = []

    def w(s=""):
        lines.append(s)

    w("Campaign report: %s" % args.directory)
    if args.pass_:
        w("Filtered to campaign_label=%r" % args.pass_)
    w("%d bundle(s)" % len(bundles))
    w("")

    by_group = {}
    for name, doc in bundles:
        by_group.setdefault(group_of(doc), []).append((name, doc))

    status_counts = {}
    debug_triggered_count = 0
    overrun_count = 0
    preroll_shortfall_count = 0
    snr_values = []
    for name, doc in bundles:
        status_counts[doc.get("status", "?")] = status_counts.get(doc.get("status", "?"), 0) + 1
        if doc.get("debug_triggered") is True:
            debug_triggered_count += 1
        if doc.get("audio_report") is True or "capture_overrun_events" in doc:
            if int(doc.get("capture_overrun_events", 0) or 0) > 0:
                overrun_count += 1
            pre_del = doc.get("pre_roll_words_delivered")
            pre_cfg = doc.get("pre_roll_words_configured")
            if pre_del is not None and pre_cfg is not None and int(pre_del) < int(pre_cfg):
                preroll_shortfall_count += 1
        snr = doc.get("expect_snr_db", doc.get("snr_db"))
        if isinstance(snr, (int, float)):
            snr_values.append(snr)

    w("Status breakdown:")
    for status, n in sorted(status_counts.items(), key=lambda kv: -kv[1]):
        w("  %-12s %d" % (status, n))
    w("")
    w("SNR (dB): %s" % fmt_stat(snr_values))
    w("debug_triggered captures: %d / %d" % (debug_triggered_count, len(bundles)))
    n_fired_false = sum(1 for _, d in bundles if d.get("fired") is False)
    if any("fired" in d for _, d in bundles):
        w("captures where nothing was excited (fired=false): %d / %d" % (n_fired_false, len(bundles)))
    w("captures with a per-capture overrun: %d / %d" % (overrun_count, len(bundles)))
    w("captures with a pre-roll shortfall (delivered < configured): %d / %d" %
      (preroll_shortfall_count, len(bundles)))
    w("")

    w("Per group (strikes by spoke; controls kept apart):")
    for key in sorted(by_group):
        entries = by_group[key]
        group_status = {}
        group_snr = []
        for _, doc in entries:
            group_status[doc.get("status", "?")] = group_status.get(doc.get("status", "?"), 0) + 1
            snr = doc.get("expect_snr_db", doc.get("snr_db"))
            if isinstance(snr, (int, float)):
                group_snr.append(snr)
        w("  %s: %d shot(s), status=%s, snr=%s" % (key[1], len(entries), group_status, fmt_stat(group_snr)))

    report = "\n".join(lines) + "\n"
    print(report, end="")
    if args.out:
        with open(args.out, "w", encoding="utf-8") as f:
            f.write(report)
        print("(also written to %s)" % args.out, file=sys.stderr)


if __name__ == "__main__":
    main()
