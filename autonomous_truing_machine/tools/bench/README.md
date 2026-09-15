# Solenoid bench

Two pieces, used together, for characterizing the JF-0530B push solenoids and their IRLB4132
driver channels **outside** the firmware — no acoustic subsystem, no orchestrator, no session.

| | |
|---|---|
| `solenoid_smoke/` | standalone ESP-IDF sketch for the Nano. LEFT = GPIO 9 (D6), RIGHT = GPIO 10 (D7) — the pins `board_nano_esp32.h` declares. Fires nothing on boot; every activation is a keystroke. |
| `solenoid_ctl.py` | the laptop side. Sends each keypress to the board, logs the board's replies and your own notes (`n`) into one timestamped record at `%TEMP%\solenoid_ctl.log`. |

Run it (the Nano's DTR/RTS reach GPIO0/reset, so this script — not a naive monitor — is the way
to open COM5):

    & "$env:USERPROFILE\.platformio\penv\Scripts\python.exe" "$env:USERPROFILE\truing_ws\tools\bench\solenoid_ctl.py"

Build and flash per `docs/DEV_NOTES.md`'s Nano rules (never `-t upload`; three regions with esptool).

## Keys

One shot runs five stages, each adjustable:

    push -> hold -> approach -> brake -> cutoff -> (catch delay) -> catch -> off

| key | knob |
|---|---|
| `l` / `r` | fire LEFT / RIGHT with the whole profile |
| `b` / `v` | fire LEFT / RIGHT **baseline** — every stage below forced to 0, tuned knobs left untouched. The A/B reference. |
| `L` / `R` | as `l`/`r` but a 500 ms hold, for reading V_DS |
| `a` | 10 alternating shots, any key aborts |
| `+` / `-` | shot width (hold at full duty) |
| `{` / `}` | push ramp — duty 0 → full |
| `[` / `]` | release ramp — duty full → brake duty |
| `k` / `K` | brake duty — level held before final cutoff |
| `m` / `M` | brake hold time |
| `g` / `G` | catch duty — strength of the post-cutoff pulse |
| `t` / `T` | catch delay — when it fires, after cutoff (1 ms steps) |
| `w` / `W` | catch width — how long it lasts (1 ms steps) |
| `s` | print every knob and both channels' state |
| `c`, `u` / `d`, `x` | manual jog: select channel, step live duty up/down, kill both |

Jog is separate from firing: it sets a duty and **holds** it until changed, for watching the
actuator's own response by hand. `x` clears live output only — never the tuned knobs, so a kill
mid-experiment doesn't lose a setting being dialled in.

Duty is PWM duty, not an analog voltage. The gate is switched hard between 0 V and 3.3 V at
20 kHz throughout; nothing at the gate node smooths that into a DC level (and the S3 has no
DAC), so a meter there reads a fixed-amplitude square wave with a moving pulse width. What does
vary smoothly is the **coil current**, which the solenoid's own inductance integrates — and
that is what sets how hard the plunger pulls. The `avg ~N mV` in the log is duty × 3.3 V, a
stand-in for "how far up the dial you are", not a gate measurement.

## Findings (2026-09-15, LEFT channel, free air, by ear)

**Pull-in / drop-out hysteresis.** Jogging duty up, the plunger extends progressively — stable
partial positions at roughly 765, 816, 867 /1023 (1/4, half, full). Jogging back down it holds
all the way to roughly 153–204 /1023, then lets go in one sudden motion. A 4–5× gap between
pull-in and drop-out, which is ordinary for a solenoid.

The asymmetry is the mechanism for everything below: magnetic force scales about as 1/gap², so
going up there is a stable balance at each duty, while going down there is none — the instant
the armature breaks free the gap opens, force collapses, the spring wins harder, and it runs
away home.

**Release ramp does not soften the landing.** Measured: it moves *when* the plunger lets go, not
how hard it lands. The ramp spends most of its length above drop-out with nothing happening,
crosses the threshold near its own end, and the mechanical return then runs at its own speed.
Reads as a pure time offset.

**Constant brake duty does not work, and cannot.** Any duty low enough to ever release is, by
the same 1/gap² collapse, already too weak to resist once the gap opens. There is no constant
level that both lets go and slows the fall — you get "never releases" or "runs away", with
nothing in between. Tested at 153/1023 for 20 ms: indistinguishable from baseline. That value
was at/below drop-out, which is exactly why.

**Catch pulse works in free air — and its tuning does not survive contact with a spoke.** A
short pulse at *pull-in* level, fired after cutoff and timed into the retraction transient, is
different in kind: pull-in duty is by definition strong enough to move the plunger across a
large gap, so it can decelerate one already in flight. In free air a quieter retraction was
found by ear.

Against an actual spoke it broke. The spoke truncates the outward travel: less stroke, less
stored spring energy, shorter return distance, shorter transit — so a delay tuned against the
free-air fall fires at the wrong moment, and the spoke impact removes energy on top of that.

The consequence is what settles it: **the catch tuning is a function of travel distance, and
travel distance is standoff.** B2 makes standoff a per-station variable (M2) and then checks it
for drift over 200 strikes (M4). A setting that must be re-tuned whenever the mount moves, and
that silently degrades as it drifts, is the wrong dependency to carry into the campaign.

## Status: parked

This route is parked, not deleted — the knobs stay, so it can be picked up without rebuilding.

**Revisit only if** B3.0 shows actuator emissions actually reach the measurement: its controls
(20 no-fire + 10 air shots per station, interleaved) exist precisely to answer that, and pass
only on 0 false clears and no coherent in-band line. Absent that evidence, retraction noise is
a room-annoyance, not a measurement problem, and the cheap answer is **foam or rubber at the
stop** — it absorbs the impact energy mechanically, needs no firmware, and cannot drift out of
tune.

Note the tradeoff is not symmetric: a plunger still resting on the spoke **damps the ring being
measured**, so fast retraction has a proven signal benefit against an unproven noise cost. The
signal side — strike location on the free span (B2-M1), pulse bracket (B2-M3), mic position —
is the better target, and is already scheduled.

**Not established by any of the above:** nothing here was measured with the microphone or
through the DSP. It is free-air, by ear, on one channel, on one rig, with no quantification of
"quieter". None of it constrains what the acoustic pipeline actually sees.
