# Workflow/transport probes

Read-only-of-the-firmware investigation tools that talk to a running board over the network.
Promoted from ad hoc scratch scripts once each had earned its place by finding something real:
`ws_client.py`'s shape comes from the acoustic-phase and acquisition-path investigations,
`socket_pressure.py` is what found the transport bug fixed in c684840.

These are not part of the release sweep and not part of `pio test`. They need a board on the
network, so they belong next to the other on-target tools, not in `test/`.

| tool | what it checks | needs |
|---|---|---|
| `ws_client.py` | drives or watches one `/ws` session — the shared client every other probe here is built on | a board reachable at `--host` (default `192.168.4.1`) |
| `socket_pressure.py` | the server stays answerable when open sockets approach `MAX_OPEN_SOCKETS` | same |

Board-specific serial capture lives one level up (`tools/serial_capture.py` for the DevKit,
`tools/nano_serial.py` for the Nano) — those are drivers for a UART, not the network, so they
sit with the other board tooling rather than here. Acoustic fixture capture
(`tools/capture_fetch.py`, `tools/capture_accept.py`) is a third category again: it produces
committed evidence, where these two only print a verdict.

## Why these two and not the rest of the scratch history

The investigation history held roughly a dozen single-purpose Node scripts that each opened
`/ws` and did one thing — start a session, watch phase events, query provenance, query state.
`ws_client.py` is what all of them had in common, factored out once, so a new one-off
investigation is an import and a few lines rather than another full WebSocket handshake
written by hand. `socket_pressure.py` is promoted because it is a standing regression check
for a bug that was silent on the serial console and only visible from the network side.

The `patch_*.py` scripts from the same period are deliberately NOT here: they were one-off
workarounds for `web_ui.h` editing friction (Bash stripping backslashes in shell heredocs),
not investigation tools, and the actual fix was a house rule — see docs/DEV_NOTES.md — not a script.
