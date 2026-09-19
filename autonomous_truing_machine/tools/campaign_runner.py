"""Actively drive MEASURE_ONCE (SPEC §12.5) to build a bench-characterisation campaign.

campaign_recorder.py is passive: it listens to a real orchestrator session and harvests
whatever the session happens to measure. This is the opposite -- there is no session, no
wheel, no operator waiting on a WAIT_FOR_OPERATOR turn. It sends the debug channel's
MEASURE_ONCE intent (`{"cmd": "DEBUG", "code": 1, "arg": <spoke_id>}`) directly, one shot at a
time, and fetches whatever capture that produced. That is the shape B2's M1-M5 bench
characterisation actually needs: fire, look at the capture, adjust standoff/angle/pulse, fire
again -- with no session ceremony between shots.

    python tools/campaign_runner.py --spokes 0,1 --count 5 --pass B2-M3 --note "standoff 8mm"
    python tools/campaign_runner.py --spokes 0 --count 1                 # one-off single shot
    python tools/campaign_runner.py --spokes 0,1 --count 10 --no-fire --pass B3.0-nofire   # control: capture, never excite
    python tools/campaign_runner.py --spokes 0 --count 6 --pulse-ms 60 --pass B3.2-L60      # one width for this run
    python tools/campaign_runner.py --plan docs/campaign_plans/b30.json     # B3.0: seeded, interleaved, block by block
    python tools/campaign_runner.py --plan docs/campaign_plans/b32.json     # B3.2 (plans come from tools/campaign_sequence.py)

--spokes takes a comma list of spoke_ids to cycle through (default: 0). --count is shots per
spoke (default 1); spokes are interleaved round-robin (sp0, sp1, sp0, sp1, ...) rather than run
back to back, so a systematic drift between early and late shots shows up per spoke rather than
being confounded with elapsed time.

Board admission: the debug channel itself must be enabled (debug_channel_enabled, SPEC §12.5 --
development/bring-up builds only, never the demo script) or every shot rejects with
REJECT_DEBUG_DISABLED and this tool says so plainly rather than retrying blindly.

Before B2, PRESENT is 0 for both stations (SOLENOID_CAMPAIGN.md) -- the orchestrator still
ACCEPTs a MEASURE_ONCE for a valid spoke (spoke_id range is the only precondition it checks),
but the acoustic subsystem's own excitation-availability check then fails the attempt before
any capture happens (EXCITATION_UNAVAILABLE), and the capture buffer's seq does not move. This
tool watches for exactly that: it fetches the current seq before each shot and refuses to file a
bundle if the seq did not advance, rather than silently re-saving a stale capture from some
earlier attempt as if it were this shot's.

No race to fetch against: truing_wire_session_handle() sends the ACK for a DEBUG intent only
after truing_orch_submit_intent() returns, and MEASURE_ONCE's dispatch calls the blocking
truing_acoustic_measure() inline -- by definition the same call that sets outcome_seq to match
capture_seq (see acoustic_real.c) has already returned by the time the ACK reaches this script.
An ACCEPT ack is therefore always safe to fetch immediately; this tool does not poll for the
409 capture_pending() can also 're report -- if one is ever seen here, that is itself a finding
worth reporting upstream, not something to retry past.

Writes bundles with tools/capture_fetch.py's own writer, so campaign_report.py and
review_packet.py read the exact same schema campaign_recorder.py produces. Defaults to
test/fixtures/acoustic/captures/_campaign (gitignored; promote the few that matter with
tools/capture_fetch.py or capture_accept.py, same as the passive recorder).

Run from autonomous_truing_machine/ with this machine joined to the board's AP.
"""
import argparse
import json
import os
import shutil
import sys
import time
import urllib.error

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "probe"))
from ws_client import WS  # noqa: E402

sys.path.insert(0, os.path.dirname(__file__))
from capture_fetch import fetch_bundle, write_bundle, add_to_index  # noqa: E402
import campaign_sequence  # noqa: E402

DEFAULT_HOST = "192.168.4.1"
DEFAULT_OUT = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "test", "fixtures", "acoustic", "captures", "_campaign")
MEASURE_ONCE_CODE = 1   # truing_debug_code.h: TRUING_DEBUG_CODE_MEASURE_ONCE

# SOLENOID_CAMPAIGN.md's Convention: spoke 0 -> LEFT, alternating from there (A1). Current rig_id
# tokens per SOLENOID_CAMPAIGN.md's rig registry (Amendment 2) -- override with --rig-id-left /
# --rig-id-right the day either station's registry block bumps to a new token, so a manifest never
# silently records a stale rig version.
DEFAULT_RIG_ID_LEFT = "LEFT/rig-1"
DEFAULT_RIG_ID_RIGHT = "RIGHT/rig-1"


def station_for_spoke(spoke_id):
    return "LEFT" if spoke_id % 2 == 0 else "RIGHT"


# MEASURE_ONCE's arg (lib/truing_core/include/truing/debug_code.h): bits 0..7 spoke_id, bit 8 no_fire,
# bits 9..15 reserved, bits 16..31 one-shot pulse override in ms (0 = the excitation profile's).
MEASURE_ONCE_NO_FIRE_BIT = 1 << 8
MEASURE_ONCE_PULSE_SHIFT = 16
PULSE_MAX_MS = 1000     # TRUING_EXCITATION_PULSE_MAX_MS; the board refuses more, but say so here first


def measure_once_arg(spoke_id, no_fire=False, pulse_ms=0):
    if no_fire and pulse_ms:
        raise SystemExit("--no-fire and --pulse-ms together make no sense: a pulse width for a strike "
                         "that is not going to happen")
    return spoke_id | (MEASURE_ONCE_NO_FIRE_BIT if no_fire else 0) | (pulse_ms << MEASURE_ONCE_PULSE_SHIFT)


def ts():
    return time.strftime("%H:%M:%S")


def log(m):
    print("%s  %s" % (ts(), m), flush=True)


def current_seq(host, timeout):
    """The board's capture seq right now, or 0 if nothing has ever been captured."""
    try:
        meta, _ = fetch_bundle(host, timeout)
        return int(meta.get("seq", 0))
    except urllib.error.HTTPError as e:
        if e.code == 404:   # a board that has not yet captured anything says so with a 404
            return 0
        raise
    except SystemExit as e:
        if "nothing captured" in str(e) or "404" in str(e):
            return 0
        raise


def wait_for_ack(ws, seq, timeout_s):
    """Reads frames until the ack for this seq arrives, or returns None on timeout."""
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        try:
            fr = ws.recv()
        except ConnectionError as e:
            raise SystemExit("socket closed waiting for ack: %s" % e)
        if not fr:
            continue
        try:
            f = json.loads(fr)
        except ValueError:
            continue
        if f.get("t") == "ack" and f.get("seq") == seq:
            return f
    return None


def fire_once(ws, host, timeout, seq_counter, spoke_id, arg=None):
    """Sends one MEASURE_ONCE for spoke_id (arg is spoke_id packed with any overrides; defaults to
    the bare spoke id), returns (ack, bundle_meta_or_None, pcm_or_None)."""
    seq_before = current_seq(host, timeout)
    seq_counter[0] += 1
    my_seq = seq_counter[0]
    ws.send_text({"cmd": "DEBUG", "code": MEASURE_ONCE_CODE, "arg": spoke_id if arg is None else arg,
                  "seq": my_seq})
    ack = wait_for_ack(ws, my_seq, timeout)
    if ack is None:
        raise SystemExit("no ack for MEASURE_ONCE sp%d (seq %d) within %.1fs" % (spoke_id, my_seq, timeout))
    if ack.get("verdict") != "ACCEPT":
        return ack, None, None
    meta, pcm = fetch_bundle(host, timeout)
    if int(meta.get("seq", -1)) == seq_before:
        return ack, "stale", None   # excitation unavailable / no capture happened
    return ack, meta, pcm


# Trial exclusion codes, declared before any data (plan: "nothing is deleted; exclusions only under
# pre-declared codes"). Every one is about the mechanics of the trial, never about what the
# analysis made of it, so replacing an excluded trial cannot bias a rate. The bundle of an excluded
# trial that did produce one is kept; the ledger says why it does not count.
EXCLUSION_ACK_REJECTED = "ACK_REJECTED"                   # the board refused the intent
EXCLUSION_NO_NEW_CAPTURE = "NO_NEW_CAPTURE"               # accepted, but no capture happened
EXCLUSION_FETCH_FAILED = "FETCH_FAILED"                   # the transport dropped
EXCLUSION_CAPTURE_NOT_OK = "CAPTURE_NOT_OK"               # capture_result != OK (overrun, timeout ...)
EXCLUSION_RECORD_CONTRADICTS = "RECORD_CONTRADICTS_REQUEST"   # fired / pulse_ms disagree with what was asked
MAX_TRIES_PER_TRIAL = 3


def role_selection(no_fire, pulse_ms):
    """The firmware-level selection (plan A7): what the board was asked to do, not why."""
    return "no_fire" if no_fire else ("pulse_override" if pulse_ms else "derived")


def take_shot(ws, args, ctx, spoke_id, kind, pulse_ms, name_stem, extra):
    """One MEASURE_ONCE and its bundle. Returns (outcome, doc): outcome is "kept" or an exclusion code,
    doc the written bundle or None. `kind` is the physical role -- strike, air_shot or no_fire."""
    no_fire = kind == "no_fire"
    try:
        ack, meta, pcm = fire_once(ws, args.host, args.timeout, ctx["seq_counter"], spoke_id,
                                   measure_once_arg(spoke_id, no_fire, pulse_ms))
    except (urllib.error.URLError, OSError) as e:
        log("!! sp%d %s: fetch failed (%s)" % (spoke_id, kind, e))
        return EXCLUSION_FETCH_FAILED, None
    if ack.get("verdict") != "ACCEPT":
        log("!! sp%d %s: rejected verdict=%s reason=%s" % (spoke_id, kind, ack.get("verdict"), ack.get("reason")))
        if ack.get("verdict") == "REJECT_DEBUG_DISABLED":
            raise SystemExit("debug_channel_enabled is false on this build/session; "
                             "MEASURE_ONCE cannot run until it is turned on")
        if ack.get("verdict") == "REJECT_STATE":
            # MEASURE_ONCE is admitted only in READY with no session. The usual causes: a session is
            # running, the board has not finished starting, or its INITIALIZE is waiting on a
            # reference confirmation (an image with composite navigation waits for a person to confirm
            # spoke 0 at LEFT; the campaign bench image does not, and should reach READY by itself).
            log("!! the board is not in READY: is a session running, is it still booting, or is it "
                "waiting for a reference confirmation in the UI (http://%s/)?" % DEFAULT_HOST)
        return EXCLUSION_ACK_REJECTED, None
    if meta == "stale":
        log("-- sp%d %s: ack accepted but no new capture (excitation unavailable -- PRESENT=0?)" % (spoke_id, kind))
        return EXCLUSION_NO_NEW_CAPTURE, None
    # Convention (SOLENOID_CAMPAIGN.md): even spoke_id -> LEFT, odd -> RIGHT. The manifest derives the
    # station itself rather than trusting a field this path never had the board populate.
    station = station_for_spoke(spoke_id)
    rig_id = ctx["rig_id_by_station"][station]
    meta = dict(meta, campaign_label=ctx["label"], campaign_session=ctx["session"],
                campaign_station=station, campaign_rig_id=rig_id,
                campaign_selection=role_selection(no_fire, pulse_ms), campaign_kind=kind)
    meta.update(extra)
    if not meta.get("debug_triggered"):
        log("!! sp%d %s: capture did not report debug_triggered=true -- check acoustic_real.c's one-shot marker"
            % (spoke_id, kind))
    name = "%s_seq%d" % (name_stem, int(meta.get("seq", 0)))
    doc = write_bundle(name, meta, pcm, ctx["note"], args.out)
    add_to_index(name, args.out)
    # The board's own record of what it did, against what was asked: a control that says it fired, or a
    # level that came out at another width, is a finding, not a trial.
    contradiction = None
    if no_fire and doc.get("fired") is not False:
        contradiction = "asked for a no-fire control but the capture says fired=%s" % doc.get("fired")
    elif not no_fire and doc.get("fired") is False:
        contradiction = "asked for a %s but the capture says fired=false" % kind
    elif pulse_ms:
        try:
            applied = float(doc.get("pulse_ms"))
        except (TypeError, ValueError):
            applied = None
        if applied is None or abs(applied - pulse_ms) > 0.01:
            contradiction = "asked for %d ms but the capture records pulse_ms=%s" % (pulse_ms, doc.get("pulse_ms"))
    if contradiction:
        log("!! sp%d %s: %s -- not counted" % (spoke_id, kind, contradiction))
        return EXCLUSION_RECORD_CONTRADICTS, doc
    if doc.get("capture_result") not in (None, "OK"):
        log("!! sp%d %s: capture_result=%s -- not counted" % (spoke_id, kind, doc.get("capture_result")))
        return EXCLUSION_CAPTURE_NOT_OK, doc
    log("   kept %s  %s/%s  snr=%s f1=%s  station=%s rig_id=%s  fired=%s pulse_ms=%s" % (
        name, doc.get("status"), doc.get("reason"), doc.get("expect_snr_db"), doc.get("expect_f1_hz"),
        station, rig_id, doc.get("fired"), doc.get("pulse_ms")))
    return "kept", doc


def run_plan(ws, args, ctx, plan, plan_sha):
    """Executes a campaign_sequence.py plan block by block. The operator turns the wheel between blocks;
    inside a block trials fire back to back. A trial that fails for a pre-declared mechanical reason is
    retried in place (order preserved) and ledgered; MAX_TRIES_PER_TRIAL failures in a row stop the run,
    because a trial that will not complete is a finding, not something to route around."""
    ledger_path = os.path.join(args.out, "%s_exclusions.jsonl" % ctx["label"])
    blocks = plan["blocks"]
    total = sum(len(b["trials"]) for b in blocks)
    global_index = sum(len(b["trials"]) for b in blocks[:args.start_block])
    kept = excluded = 0
    end_block = len(blocks) if args.end_block is None else args.end_block
    for bi in range(args.start_block, end_block):
        block = blocks[bi]
        log("=== block %d/%d  %s  (%d trials) ===" % (bi + 1, len(blocks), block["name"], len(block["trials"])))
        log(block["prompt"])
        if not args.no_prompt:
            input("    press Enter when the wheel is positioned... ")
        for t in block["trials"]:
            global_index += 1
            extra = {"campaign_stage": plan["stage"], "campaign_block": block["name"],
                     "campaign_trial": global_index, "campaign_arm": t["arm"],
                     "campaign_plan_sha256": plan_sha, "campaign_plan_seed": plan["seed"]}
            stem = "%s_t%03d_%s_sp%d" % (ctx["label"], global_index, t["kind"], t["spoke"])
            tries = 0
            while True:
                tries += 1
                log("[%d/%d] %s  %s  spoke %d%s  (try %d)" % (
                    global_index, total, block["name"], t["kind"], t["spoke"],
                    "  %d ms" % t["pulse_ms"] if t["pulse_ms"] else "", tries))
                outcome, doc = take_shot(ws, args, ctx, t["spoke"], t["kind"], t["pulse_ms"], stem, extra)
                if outcome == "kept":
                    kept += 1
                    break
                excluded += 1
                with open(ledger_path, "a", encoding="utf-8") as f:
                    f.write(json.dumps({"trial": global_index, "block": block["name"], "kind": t["kind"],
                                        "spoke": t["spoke"], "pulse_ms": t["pulse_ms"], "try": tries,
                                        "code": outcome, "bundle": doc.get("name") if doc else None,
                                        "plan_sha256": plan_sha}, sort_keys=True) + "\n")
                if tries >= MAX_TRIES_PER_TRIAL:
                    raise SystemExit("trial %d failed %d times in a row (last: %s) -- stopping; see %s" % (
                        global_index, tries, outcome, ledger_path))
            time.sleep(args.interval)
    return kept, excluded


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--spokes", default="0", help="comma list of spoke_ids to cycle through")
    p.add_argument("--count", type=int, default=1, help="shots per spoke (default 1)")
    p.add_argument("--interval", type=float, default=2.0, help="seconds between shots (default 2.0)")
    p.add_argument("--pass", dest="pass_", default=None, help="technique label, e.g. B2-M3")
    p.add_argument("--note", default="", help="what this run is testing, in your words")
    p.add_argument("--host", default=DEFAULT_HOST)
    p.add_argument("--out", default=DEFAULT_OUT)
    p.add_argument("--timeout", type=float, default=20.0)
    p.add_argument("--no-fire", action="store_true",
                    help="run the capture and analysis but never excite (B3.0 control); recorded fired=false")
    p.add_argument("--pulse-ms", type=int, default=0,
                    help="one-shot pulse width for every strike this run, in whole ms, instead of the "
                         "excitation profile's (B3.2 level sweep); 0 = the profile's")
    p.add_argument("--plan", help="a plan written by tools/campaign_sequence.py (B3.0 / B3.2). The plan "
                                  "decides spokes, kinds, widths and order; --spokes/--count/--no-fire/--pulse-ms "
                                  "are not used with it")
    p.add_argument("--start-block", type=int, default=0,
                    help="with --plan: resume at this block index (0-based) after an interruption")
    p.add_argument("--end-block", type=int, default=None,
                    help="with --plan: stop before this block index (0-based, exclusive), so one block runs "
                         "and the wheel can be turned before the next; the plan and its sha256 are unchanged")
    p.add_argument("--no-prompt", action="store_true",
                    help="with --plan: do not wait for Enter between blocks (only when nobody has to turn the wheel)")
    p.add_argument("--rig-id-left", default=DEFAULT_RIG_ID_LEFT,
                    help="LEFT station's current rig_id token (default %s; see "
                         "SOLENOID_CAMPAIGN.md's rig registry)" % DEFAULT_RIG_ID_LEFT)
    p.add_argument("--rig-id-right", default=DEFAULT_RIG_ID_RIGHT,
                    help="RIGHT station's current rig_id token (default %s)" % DEFAULT_RIG_ID_RIGHT)
    args = p.parse_args()
    rig_id_by_station = {"LEFT": args.rig_id_left, "RIGHT": args.rig_id_right}
    if not 0 <= args.pulse_ms <= PULSE_MAX_MS:
        raise SystemExit("--pulse-ms must be 0..%d (got %d)" % (PULSE_MAX_MS, args.pulse_ms))
    measure_once_arg(0, args.no_fire, args.pulse_ms)   # validates the combination before touching the board

    plan = plan_sha = None
    if args.plan:
        if args.no_fire or args.pulse_ms or args.spokes != "0" or args.count != 1:
            raise SystemExit("with --plan the plan decides: drop --spokes/--count/--no-fire/--pulse-ms")
        with open(args.plan, "rb") as f:
            plan = json.loads(f.read().decode("utf-8"))
        plan_sha = campaign_sequence.plan_sha256(plan)
        if not 0 <= args.start_block < len(plan["blocks"]):
            raise SystemExit("--start-block must be 0..%d" % (len(plan["blocks"]) - 1))
        if args.end_block is not None and not args.start_block < args.end_block <= len(plan["blocks"]):
            raise SystemExit("--end-block must be in %d..%d" % (args.start_block + 1, len(plan["blocks"])))
        spokes = sorted({t["spoke"] for b in plan["blocks"] for t in b["trials"]})
    else:
        try:
            spokes = [int(s) for s in args.spokes.split(",") if s.strip() != ""]
        except ValueError:
            raise SystemExit("--spokes must be a comma list of integers, e.g. 0,1 (got %r)" % args.spokes)
    if not spokes:
        raise SystemExit("no spoke_ids to fire")
    if any(not 0 <= sp <= 255 for sp in spokes):
        raise SystemExit("spoke_ids must be 0..255 (the arg packs them into 8 bits)")

    os.makedirs(args.out, exist_ok=True)
    session = time.strftime("%Y%m%d-%H%M")
    label = args.pass_ if args.pass_ else (plan["stage"] if plan else session)
    if plan:
        # the plan the bundles refer to by hash travels with them
        shutil.copyfile(args.plan, os.path.join(args.out, "%s_plan.json" % label))

    try:
        ws = WS(args.host, 80, "/ws")
    except OSError as e:
        raise SystemExit("could not reach %s (%s). Joined to the board's AP?" % (args.host, e))
    log("campaign runner attached  label=%s  session=%s  spokes=%s  out=%s" % (label, session, spokes, args.out))
    log("rig_id  LEFT=%s  RIGHT=%s  (override with --rig-id-left/--rig-id-right if either "
        "station's registry block has bumped)" % (args.rig_id_left, args.rig_id_right))
    if plan:
        log("plan %s  stage=%s  seed=%s  sha256=%s  blocks=%d" % (
            args.plan, plan["stage"], plan["seed"], plan_sha, len(plan["blocks"])))
    else:
        log("selection=%s%s" % (role_selection(args.no_fire, args.pulse_ms),
                                "  pulse=%d ms" % args.pulse_ms if args.pulse_ms else ""))
    log("active: sends DEBUG/MEASURE_ONCE, one shot at a time, and fetches the result")

    kind_text = "campaign pass %s" % label if (args.pass_ or plan) else "ad-hoc run %s" % session
    ctx = {"seq_counter": [0], "rig_id_by_station": rig_id_by_station, "label": label, "session": session,
           "note": "%s. %s" % (kind_text, args.note) if args.note else kind_text}
    kept = excluded = 0
    try:
        if plan:
            kept, excluded = run_plan(ws, args, ctx, plan, plan_sha)
        else:
            kind = "no_fire" if args.no_fire else "strike"
            for round_ in range(args.count):
                for spoke_id in spokes:
                    tag = "_nofire" if args.no_fire else ("_p%d" % args.pulse_ms if args.pulse_ms else "")
                    outcome, _doc = take_shot(ws, args, ctx, spoke_id, kind, args.pulse_ms,
                                              "%s_sp%d_r%d%s" % (label, spoke_id, round_, tag), {})
                    if outcome == "kept":
                        kept += 1
                    else:
                        excluded += 1
                    time.sleep(args.interval)
    except KeyboardInterrupt:
        log("stopped by operator")
    finally:
        ws.close()
    log("%s: %d kept, %d not counted%s" % (label, kept, excluded,
                                            " (see %s_exclusions.jsonl)" % label if plan and excluded else ""))


if __name__ == "__main__":
    main()
