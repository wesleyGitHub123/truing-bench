# Recorded UI sessions

Verbatim `/ws` frame logs captured off a running board, replayed through the web UI's own
script by `tools/ui_check.js`. Hand-built frames in that file test the cases someone thought
to write; these test that the page survives the exact sequence and shapes the firmware really
emits.

    <name>.json   { "meta": {board, build, ui, mode, acquisition, ...},
                    "frames": [ every /ws frame, in order, unmodified ] }

## Recapturing

    python tools/probe/session_capture.py --start --auto \
        --out test/fixtures/ui/session_fastdemo_auto.json

`--start` issues `START_TRUING`; `--auto` answers each wait with the intent its `WAIT_ISSUED`
frame names. Recapture when the telemetry schema changes (a new frame kind, a renamed field)
and `ui_check.js`'s replay assertions start failing for a reason that is not a UI regression.

`session_fastdemo_auto.json` is a `nano_esp32_fastdemo_mic` cycle with no hand pluck: every
tension attempt is rejected, the runout rows carry the geometric correction, and the run ends
`CONVERGED_GEOMETRIC_ONLY`. It exercises the ARMED/LISTENING cue wording, the rejection
translations, the terminal styling, and ~113 transitions' worth of "handle() must not throw".
