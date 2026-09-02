# Model preparation (host side, Phase 1b)

Generates the influence-matrix artifact the firmware consumes (SPEC §8.7,
§8.10, §11.4) and runs the Phase 1b host gate (SPEC §14.3, host subset).
Everything here runs offline in Python; the ESP32 never performs an SVD.

```bash
python -m venv .venv
.venv\Scripts\python -m pip install -r requirements.txt
.venv\Scripts\python -m pytest -q                    # the host gate
.venv\Scripts\python -m truing_model_prep.cli generate-fixture --out golden/fixture_sym32_artifact.json
.venv\Scripts\python -m truing_model_prep.cli report golden/fixture_sym32_artifact.json
```

`bike-wheel-calc` is pinned to commit `6fc380c` (verified: its own 52 tests pass
on Python 3.12 / numpy 2.5). Its conventions are reconciled in
`truing_model_prep/conventions.py`, the only module that knows both sides.

The fixture wheel is SYNTHETIC (spoke, hub and tolerance values mirror the
firmware fixtures; the rim section is the example section from bike-wheel-calc's
own examples). It describes no real wheel. A real artifact needs the donor
wheel's measured hub geometry, an estimated rim section with stated provenance,
the per-side `c` response slopes from a bounded-rotation experiment, and the
declared indexing origin (SPEC §11.1).
