"""Host-side model preparation for the truing firmware (SPEC 4.4, 8.7-8.11, 11.4, Phase 1b).

Generates the influence-matrix artifact from wheel-class configuration with
`bike-wheel-calc`, computes the per-layout pseudoinverses offline, evaluates
the structural properties the truing solve depends on, and provides the host
reference implementation of the two-part solve used for firmware parity.

Nothing here runs on the ESP32 (SPEC 8.7: the pseudo-inverse is computed
offline). The firmware consumes only the artifact this package writes.
"""

GENERATOR_VERSION = "truing_model_prep 0.1.0"
BIKE_WHEEL_CALC_COMMIT = "6fc380c3576307d825d24fdffbbcbc192a720300"
