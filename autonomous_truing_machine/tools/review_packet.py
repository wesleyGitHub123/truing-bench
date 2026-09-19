"""Bundle a campaign directory's report and manifest into one document a reviewer can read
without running any of the other tools.

campaign_report.py answers "what happened"; this answers "how do I check that for myself."
It re-runs campaign_report's own summary (imported, not reimplemented) and appends a manifest
of every bundle in the directory -- name, spoke, status/reason, and the pcm_sha256 each bundle
already carries (written by tools/capture_fetch.py's writer) -- so a reviewer can confirm which
exact bytes a claim is about without re-fetching anything from a board that may no longer be on.

    python tools/review_packet.py test/fixtures/acoustic/captures/_campaign --pass B2-M3 \\
        --out docs/review_packets/B2-M3.md

Writes a single self-contained Markdown file. It does not copy the .pcm/.json files
themselves -- the packet is meant to travel with (or point back at) the directory it was built
from, not replace it. If the directory is one that should not travel as-is (it is gitignored,
per campaign_recorder.py's own note on why), say so in the handoff rather than assuming the
packet alone is sufficient evidence.
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from campaign_report import load_bundles, fmt_stat  # noqa: E402


def build_packet(directory, label_filter, title):
    bundles = list(load_bundles(directory, label_filter))
    if not bundles:
        raise SystemExit("no bundles matched in %s%s" %
                          (directory, " for --pass %r" % label_filter if label_filter else ""))

    lines = []

    def w(s=""):
        lines.append(s)

    w("# %s" % title)
    w("")
    w("Generated %s from `%s`%s." % (
        time.strftime("%Y-%m-%dT%H:%M:%S%z"), directory,
        " (campaign_label=%r)" % label_filter if label_filter else ""))
    w("")
    w("This is a review packet, not a verdict: it lists what the campaign produced and where")
    w("the bytes are. Whether it meets an acceptance criterion is decided against the campaign")
    w("plan, not by this document.")
    w("")
    w("## Summary")
    w("")

    status_counts = {}
    snr_values = []
    for _, doc in bundles:
        status_counts[doc.get("status", "?")] = status_counts.get(doc.get("status", "?"), 0) + 1
        snr = doc.get("expect_snr_db", doc.get("snr_db"))
        if isinstance(snr, (int, float)):
            snr_values.append(snr)

    w("- %d bundle(s)" % len(bundles))
    for status, n in sorted(status_counts.items(), key=lambda kv: -kv[1]):
        w("- status `%s`: %d" % (status, n))
    w("- SNR (dB): %s" % fmt_stat(snr_values))
    w("")

    w("## Manifest")
    w("")
    # `fired` and `pulse_ms` are what tell a strike from a no-fire control (which still carries its
    # spoke's station and debug_triggered=true), and one pulse level from another (the override
    # leaves excitation_digest unchanged) -- so they belong in the table a reviewer reads.
    w("| name | spoke | station | fired | pulse_ms | selection | cycle | attempt | status | reason | debug_triggered | pcm_sha256 |")
    w("|---|---|---|---|---|---|---|---|---|---|---|---|")
    for name, doc in sorted(bundles, key=lambda kv: kv[0]):
        w("| `%s` | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | `%s` |" % (
            name,
            doc.get("spoke_id", "n/a"), doc.get("station", "n/a"), doc.get("fired", "n/a"),
            doc.get("pulse_ms", "n/a"), doc.get("campaign_selection", "n/a"),
            doc.get("cycle_index", "n/a"), doc.get("attempt", "n/a"),
            doc.get("status", "n/a"), doc.get("reason", "n/a"),
            doc.get("debug_triggered", "n/a"),
            (doc.get("pcm_sha256") or "n/a")[:16]))
    w("")

    notes = sorted({doc.get("note") for _, doc in bundles if doc.get("note")})
    if notes:
        w("## Notes carried by these bundles")
        w("")
        for note in notes:
            w("- %s" % note)
        w("")

    return "\n".join(lines) + "\n"


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("directory", help="a bundle directory with an index.txt")
    p.add_argument("--pass", dest="pass_", default=None, help="only this campaign_label")
    p.add_argument("--title", default=None, help="packet title (default: derived from --pass or the directory)")
    p.add_argument("--out", required=True, help="where to write the packet (.md)")
    args = p.parse_args()

    title = args.title or ("Review packet: %s" % (args.pass_ or os.path.basename(os.path.normpath(args.directory))))
    packet = build_packet(args.directory, args.pass_, title)
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as f:
        f.write(packet)
    print("wrote %s (%d bytes)" % (args.out, len(packet)))


if __name__ == "__main__":
    main()
