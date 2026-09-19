# Implementation notes

Decisions, specification reconciliations and phase status that are not
derivable from the code itself. The specification
(`docs/SPEC.md`) is the authority; where a
note below records a reconciliation, the specification's consistency guardrail
was applied and the more specific normative rule was followed.

## Phase status

| Phase | Status |
|---|---|
| 1a Scaffolding | Implemented: data model, configuration, status/provenance, HAL contracts with stub/synthetic implementations, Wheel Navigation contract with manual and synthetic implementations, both PlatformIO environments, NVS persistence, on-device bring-up. See the commit history for the verification performed at each step. |
| 1d Orchestrator | Implemented (`lib/truing_orchestrator`): the SPEC 7 state machine with both loops, operator waits, wait-instance correlation, abort at safe points, admission with bounded re-measurement, safety bound and deadband, verification-grade convergence gating, stall/max-cycle aborts, current-state snapshot and current-cycle provenance. Consumes Wheel Navigation by outcome only and the truing calculation through the `truing_calc` contract (stub + synthetic double until 1c). Host-tested end to end with zero hardware; self-played on the DevKitC-1 (docs/BRINGUP_LOG.md). |
| 1g Runout manual entry | Implemented (`runout_manual.c`): a real implementation; entries arrive through `SUBMIT_RUNOUT` and are consumed by `read_snapshot()` without blocking. |
| 1b Host model preparation | Implemented (`../model_prep`, Python): `bike-wheel-calc` verified (commit 6fc380c, its 52 tests pass); per-spoke analytical Φ_u/Φ_v/Φ_t generation; artifact assembly with per-layout pseudoinverses, rank/conditioning, common-mode evaluation, fingerprints and load checks; host reference two-part solve; SPEC 14.3 host gate (29 tests). Golden fixture artifact and parity cases in `model_prep/golden/`. Two findings below need the owner's attention before mean-tension targeting can ever be enabled. |
| 1c Firmware truing calculation | Implemented. Host side: `model_prep/truing_model_prep/export.py` writes the compact float32 binary artifact (SHA-256 content hash, per-spoke Fourier coefficients, Φ_t, per-layout masks / rank / conditioning / Φ†) and the C fixtures. Core: `truing/sha256` and `truing/artifact` (parse, the three SPEC 8.7 checks, R3 limits, expansion on the rim grid at load). Calculation: `lib/truing_calc/src/calc_artifact.c`, the REAL implementation behind the unchanged `truing_calc` contract (SPEC 8.3.1 residual, 8.7.1 layout-pure inversion, 8.4 two-part solve, 8.6 cost, 8.11 policy, Eq. 2 targets, per-side verification). Parity with the golden host cases on the host and on the DevKitC-1 (docs/BRINGUP_LOG.md); the orchestrator self-play now runs the real calculation against a wheel simulated through the same influence model. Not covered: loading an artifact from the `artifacts` partition or over the protocol (1e), and asymmetric Part 2 (refused by design, SPEC 8.4). |
| 1f Acoustic subsystem | Implemented. The four seams of SPEC 9.2: layer 1 as `truing_hal/audio_source_if` (fixed 48 kHz / 24-in-32 format enforced at open; recorded-buffer and synthetic sources on the host; `src/audio_i2s.c` on the target with a core-1 drain task, PSRAM ring, pre-trigger tail and overrun detection); layers 2 and 4 ported to C in `lib/truing_dsp` from the research repository (Hann + zero-padded FFT + parabolic refinement, scipy-semantics prominence peaks with the relative-depth gate, Hilbert-envelope window truncation via a Bluestein transform, RMS-envelope onset detection, the four tension models); layer 3 as the research repository's current rule, "lowest strong peak in the f1 band", output marked `presumed_fundamental` with every estimate `suspect` / PROVISIONAL_MODE_ID (SPEC 4.4.1); the composed subsystem `truing_hal/acoustic_real.c` behind the unchanged one-call contract with a pluck actuator contract (GPIO pulse on target, fake on host). Every DSP constant is chain-profile configuration (blob schema 2). Verified against the research pipeline on recorded campaign excerpts (acoustic_prep) on the host and on the DevKitC-1 (docs/BRINGUP_LOG.md). Not done: the WiFi-load capture stress test (no WiFi stack until 1e), any damping ritual, and a validated tension model or L_eff (research, SPEC 9.5). |
| 1e Comms | Implemented. `lib/truing_proto`: a framework-free wire protocol (bounded JSON writer and reader; telemetry event frames; the SPEC 12.2 current-state query; `GET_CURRENT_CYCLE_PROVENANCE`; command decoding enforcing the SPEC 12.3 `wait_id` contract) and a session layer that runs on the orchestrator's own task. `src/net_transport.c`: SPEC 12.1 soft-AP, HTTP server and one websocket endpoint. `src/web_ui.h`: the operator UI, served from flash. The SPEC 9.4 capture-under-WiFi-load check deferred from 1f now runs, with its scope stated below. Not done: the SPEC 12.1 USB CDC fallback (the frames are transport-independent, so this is a driver, not a protocol change), loading an artifact over the protocol, and the SPEC 12.5 debug channel, which decodes but is refused by the orchestrator unless enabled. |
| 1h, 2+ | Not started. 1h (integration) depends on all of the above. |

## Repository layout

```
Truing Repo/                      Git root (independent of the acoustic research repo)
└── autonomous_truing_machine/    PlatformIO project; the specification lives here
    ├── lib/truing_core/          framework-free core (SPEC P3): no ESP-IDF, no RTOS, no I/O
    ├── lib/truing_hal/           subsystem contracts (acoustic, runout, navigation, wheel drive,
    │                             operator intent source, telemetry, clock) + stub/synthetic impls
    ├── lib/truing_fixtures/      SYNTHETIC fixture configurations for tests and bring-up (not defaults)
    ├── lib/truing_board/         board profiles (all pins, SPEC 4.3)
    ├── lib/truing_calc/          truing-calculation contract (SPEC 8) with stub and synthetic double; real impl is 1c
    ├── lib/truing_orchestrator/  the state machine (SPEC 7) and the auto-operator test driver
    ├── lib/truing_proto/         SPEC 12 wire protocol and session; no transport, no framework
    ├── src/                      ESP-IDF application: bring-up, NVS config store, workflow self-play demo,
    │                             WiFi AP + websocket transport and the web UI
    ├── test/                     PlatformIO native (Unity) test suites
    └── docs/                     this file, BUILD.md
```

The specification's `SPEC.md` at the repo root (SPEC 1.2) is realised as the
file the project owner placed in `autonomous_truing_machine/`; it was not
renamed or moved.

## Decisions (trivial implementation details, SPEC 17.2)

- **Language: C11** for the core and HAL. It compiles identically under the
  xtensa toolchain and the old host MinGW GCC, needs no runtime, and the
  framework-free boundary is a plain function-pointer vtable per interface.
- **Enumerations have an explicit `UNSET = 0` value** that validation rejects,
  because C zero-initialises structures and SPEC 6.2 says `status` has no
  default. A record that was never assigned a status is invalid by construction.
- **Reason-code extensions beyond the SPEC 13.2 minimum set:** `CANCELLED`
  (a measurement discarded because ABORT cancelled it at the acoustic boundary,
  SPEC 7.4/12.3) and `WHEEL_REFERENCE_LOST` (the navigation subsystem cannot
  vouch for the wheel position, SPEC 10A). The specification calls its list a
  minimum set; both are recorded here rather than folded into an existing code
  with a different meaning.
- **Stub implementations carry `source_impl = synthetic`.** The enumeration has
  no "stub" value; a stub is not real hardware, so any session using one is
  correctly flagged `contains_non_real_implementations`. The manual (Stage 1)
  navigation implementation is `real`: its position knowledge comes from a real
  operator on a real wheel.
- **`ABORT` in `READY` is rejected** (nothing is operating). SPEC 12.3 admits it
  in "all operational states"; READY is not one.
- **Machine profile (station geometry) contents are profile-lifecycle data**,
  like the chain profile, and are not `SET_PARAMETER` targets (SPEC 11.6,
  12.3.1). No parameter id was added for them.
- **Wheel coordinates:** spoke `i` sits at rim angle `2πi/n_spokes` and rim
  index `k` at `2πk/n_rim_angles`; spoke 0 is the origin (SPEC 6.5), so with
  the Stage 1 policy `n_rim_angles = n_spokes` runout stop `k` is spoke
  `k`'s location (SPEC 8.9).
- **Two numerical guards are not physical tolerances:**
  `TRUING_RIM_ANGLE_GRID_EPS_RAD` (a stored runout angle must equal the grid
  angle for its index) and `TRUING_DECLARATION_MATCH_REL_EPS` (two declarations
  of the same fact must agree). Physical tolerances are configuration.
- **Capacity bounds** (`TRUING_MAX_SPOKES = 36`, `TRUING_MAX_CANDIDATE_PEAKS =
  8`) are structural, from SPEC 11.1's supported spoke counts and the research
  pipeline's retained peak count.
- **Fixture values are test data.** Reference-wheel figures that appear in
  `lib/truing_fixtures` (c = 473 N/rev, N_lat = 6, N_rad = 13, the reference
  tolerances) are fixture content, never defaults (SPEC P5). Nothing in the
  firmware loads them unless the `s3_devkit_provision` build is flashed on
  purpose, and that build logs loudly that fixture data is in use.
- **Bring-up I2S probe.** The firmware executes the SPEC 9.4.1 bring-up check
  (`i2s_new_channel()` succeeds with PSRAM enabled) with probe-only DMA sizing;
  it builds no capture path. Phase 2 derives the real sizing from the WiFi-load
  stress test.
- **Build path.** PlatformIO's ESP-IDF integration refuses paths with spaces;
  ESP-IDF builds run through a directory junction (docs/BUILD.md). The
  `platform` is pinned to `espressif32@6.10.0` because this PC also has a fork
  registered under the short name `espressif32`.
- **Orchestrator execution model.** A cooperative `step()` machine: each call does
  one bounded autonomous action and reports what it needs (idle, operator wait,
  settle delay, navigation poll, terminal). Intents are validated and recorded
  by `submit_intent()`; transitions happen only inside `step()`. A FreeRTOS task
  on core 0 drives it on the target; tests drive it directly.
- **VERIFY re-measurement is the next cycle's state.** `VERIFY` re-measures
  through the same interfaces as the measurement phase (SPEC 5.1) and stamps the
  records with the next cycle index; the following `MEASURE_WHEEL_STATE` finds
  the cycle complete and passes through to admission. The SPEC 7.1 transitions
  are preserved, nothing is measured twice, and no record is re-stamped. If no
  adjustment was applied (every spoke within the deadband and tolerance), VERIFY
  evaluates the current state without re-measuring.
- **Progress baseline.** `J` of the state a plan corrects (from the solve) is the
  first baseline; each verification's `J` becomes the next. `cycles_run` counts
  outer cycles that reached `EVALUATE_CONVERGENCE`; `max_cycles` bounds it.
- **Truing calculation contract.** The orchestrator never sees Φ, Φ† or weights:
  `truing_calc_if` exposes model availability with artifact identity, policy
  layout selection, R3 conditioning against artifact-recorded values, the solve,
  optional target prediction and per-channel verification. The synthetic double
  is scripted test data (turns proportional to lateral runout; displacement-only
  cost) and is never a demonstration configuration.
- **Geometry-only policy.** SPEC 8.11 sanctions "operator/config requests
  geometry-only" but §11.2 lists no field for it, so it is an orchestrator
  dependency flag (`geometry_only_policy`), not persisted configuration.
- **Navigation fault while positioning for adjustment.** SPEC 10A.10 leaves the
  terminal handling of an unrecoverable navigation fault open. Provisionally the
  run terminates `ABORT_OPERATOR` carrying the navigation reason; a fault while
  positioning for a *measurement* records that measurement as `unavailable` and
  collection continues (SPEC 13.1). Recorded measurements are never touched.
- **`CONFIRM_POSITIONED` after `ABORT`.** A terminal result clears the wait, so
  a late confirmation for the abandoned wait is rejected (no active wait), not
  accepted into a dead session. `truing_orch_reset_to_ready()` starts a new
  session; it routes through INITIALIZE again if the wheel reference was lost.

- **Compact artifact binary (Phase 1c).** Little-endian, float32 matrices, a
  120-byte header (magic `TRIA`, schema 1, numeric artifact id, 31-char name,
  generating fingerprint, SHA-256 content hash over the payload, dimensions,
  flags, payload length) followed by weights/tolerances, c per side, the
  common-mode diagnostics, `T_target_assumed`, `n_mt` (only when identified),
  per-spoke Fourier coefficients, Φ_t, the class map and, per layout, mask /
  rank / null dimension / condition number / Φ†. Sizes for sym32: 30 KB in
  flash, ~51 KB expanded in RAM. Documented in `export.py`; the loader in
  `truing/artifact.c` is the other half of the same contract and any change
  must bump the schema version on both sides.
- **Weights and tolerances are artifact-bound on the target too.** The loader
  rejects an artifact whose recorded `tol_*`, `trust_*` or `c_side_*` differ
  from the active configuration (float32 equality), because every Φ† was
  computed with them baked in (SPEC 8.7.1). This is a shape check, reported
  as ARTIFACT_INVALID; the configuration is never edited to match.
- **The sym32 fixture wheel's expected fingerprint is the golden artifact's
  generating fingerprint**, declared as a separate hex string emitted by the
  exporter next to the blob and parsed independently of it. The two are
  compared at load, never derived from each other, so the SPEC 11.4 "sole
  authority" is still the configuration and a regenerated artifact with a
  different fingerprint is refused until the configuration is updated.
- **Float32 parity tolerance.** The parity document states 1e-5 relative;
  the host build measures a worst deviation of 9.8e-6 on d_ls over 4 cases ×
  2 layouts, i.e. it meets the stated figure with no margin. The C tests
  assert 1e-4 relative (1e-6 rev absolute floor) so a different `libm`
  (`cosf`/`sinf` in the expansion) or FMA contraction cannot make the gate
  flap, and the measured figure is printed and recorded instead.
- **The expanded artifact lives in static internal RAM** (`artifact_store.c`),
  so the per-cycle solve is matrix-vector work with no heap use, and the
  corruption checks in the bring-up use PSRAM scratch copies.
- **Auto-operator response callback.** The self-play's simulated wheel can
  answer an adjustment through the loaded influence model (u += Φ_u d,
  v += Φ_v d), which makes the on-target self-play a consistency check of the
  solver against its own model (one cycle to convergence), not a test of
  truing physics. The default synthetic response is unchanged.

- **Acoustic DSP constants live in the chain profile (Phase 1f).** SPEC 9.5
  makes window and gate constants configuration; the chain profile now carries
  all of them (capture and pre-trigger lengths, excitation pulse, the
  measurement SNR gate, onset framing/thresholds/refractory, decay-floor
  truncation, zero-pad factor, search band, prominence and depth gates,
  candidate count, f2 ratio band, SNR annulus) and the tension-model profile
  carries the two-mode length bounds. The blob schema is 2; schema-1 blobs in
  NVS are rejected and the fixture provisioning build re-provisions them.
- **The port reproduces scipy/numpy semantics exactly where they matter:**
  `find_peaks` local maxima (plateau midpoint, strict rise) and prominence
  (scan through equal samples to the first higher one); the band test on the
  bin frequency before refinement; the depth gate on refined magnitudes;
  `uniform_filter1d(mode="nearest")` window placement; an unpadded Hilbert
  transform at the segment's own length (Bluestein chirp-z over power-of-two
  FFTs) so the decay-floor decision is the reference's, not an approximation.
  The one deliberate deviation: an all-zero capture reports no onset, where
  `onset.py` would fire at threshold zero on an input it never sees.
- **Bounded peak storage with a running depth gate.** A 2^18-point spectrum
  holds thousands of 6 dB-prominent noise maxima before the 30 dB depth gate;
  the reference collects them all and filters. The C port keeps a running
  band maximum and prunes on every rise, which yields the same set with 128
  slots; overflow (only possible with the depth gate disabled) is reported as
  AMBIGUOUS_PEAK rather than analysed.
- **Absolute onset floor in the fixture chain profile** (−55 dBFS rms). The
  research configuration disables it because campaign files always contain
  plucks; a device capture may not, and the relative rule then fires on noise
  whose "peak" can clear the SNR gate against its own annulus median. The
  fixture value sits 14 dB above the recorded noise floor and below every
  recorded pluck's relative threshold; it is chain characterisation, a bench
  result changes it. Reason-code extension `CAPTURE_OVERRUN` marks a capture
  the front end dropped samples from (SPEC 9.4).
- **Provenance follows the front end.** The subsystem's `source_impl` is REAL
  only on the I2S source; on a recorded or synthetic source the estimate is
  RECORDED / SYNTHETIC even though layers 2–4 are the real code, so a
  self-play session is flagged non-real (SPEC 6.2).
- **`sigma_n` is NaN on a single pluck.** The research propagates repeat
  scatter (S4.4) across identical excitations; one measurement has none.
- **Measurement SNR gate.** Below `measurement_min_snr_db` (fixture 12 dB, the
  research report's warning threshold) the estimate is REJECTED / LOW_SNR
  rather than annotated, because the solver must not consume a noise-derived
  number. The research pipeline annotates instead; that difference is a
  firmware safety rule, not a DSP change.

## Phase 1f performance — 16.2 s to 3.7 s per pluck, and where the floor now is

The correctness-first implementation took 16.2 s per pluck on target against
about 0.6 s on the host, with identical numbers. It was profiled per stage on
the DevKitC-1 and then optimised; the profiler is still in the build
(`src/bringup_acoustic.c`) as the regression evidence.

**What the profile actually said**, against the four candidates guessed when
the phase closed: two were right, one was badly wrong, and the largest cost
was not on the list in the right place.

| Stage | Before | After | What changed |
|---|---|---|---|
| Onset | 93 ms | 61 ms | platform settings only |
| Hilbert envelope | 5,991 ms | 2,085 ms | cached chirp and kernel; inverse by conjugation |
| Smoothing | 4,177 ms | 38 ms | running window sum instead of O(n x width) |
| Transform | 5,810 ms | 1,412 ms | half-angle twiddle table; magnitude folded into the split |
| Peak finding | 126 ms | 55 ms | platform settings only |
| SNR | 27 ms | 15 ms | platform settings only |
| **Total** | **16,220 ms** | **3,666 ms** | **4.4x** |

The guess that the unbounded prominence scan dominated was **wrong**: it is
126 ms, under 1% of the total. It was left exactly as it is, which also keeps
`scipy.signal.find_peaks` semantics intact.

The real dominant cost was **software double-precision trigonometry**, at a
measured 26.5 us per cos+sin pair, because the ESP32-S3 has no double FPU and
three loops evaluated transcendentals on every call at constant length. That
alone was about 5.4 s.

**The platform defaults were worth 1.6x on their own** and had nothing to do
with the DSP: the CPU ran at 160 MHz, the compiler at -Og, and the data cache
was 32 KB with 32-byte lines. Those are now set in `sdkconfig.defaults`.

### Numerical deviations

None observable. All three recorded plucks still reproduce the Python
reference exactly — 0.0000 Hz in f1, 0.000 dB in SNR — and every native suite
passes. Two changes are equivalent mathematically rather than bitwise, and are
therefore held to the fixtures rather than by construction:

- the running-sum smoother, which differs from independent per-window
  summation in floating-point association (held to the retained
  `truing_envelope_smooth_reference` within 1e-6 relative);
- the inverse Bluestein transform, now obtained from
  `IDFT(x) = conj(DFT(conj(x)))` so that one cached kernel serves both
  directions; the intermediate roundings differ from a conjugated chirp.

Everything else is bit-identical by construction, including the half-angle
twiddle table (halving the step and doubling the index round identically for
power-of-two lengths) and folding the magnitude reduction into the split.

### The floor, and what would move it

84% of the remaining 3,666 ms is transform butterflies against PSRAM: four
65,536-point transforms in the Hilbert (1,974 ms) and one 131,072-point
transform in the spectrum (1,094 ms). At 262,144 points on a 24,000-sample
window the working set cannot fit in internal RAM, so every butterfly crosses
the cache. Arithmetic tidying no longer helps — the loop micro-optimisations
in the last commit bought 46 ms.

Measured and rejected: **PSRAM at 120 MHz is 1.8x slower**, not faster
(the 131,072-point transform goes from 1,094 to 2,092 ms and a pluck from 3.7
to 6.6 s), because the octal PSRAM drops to a slower access mode when its
clock is not matched by the flash clock. The setting is pinned at 80 MHz with
that note.

Two options remain, and **both are the owner's call because both change the
correctness oracle**:

1. **A cache-blocked or radix-4 transform.** Four to six passes over memory
   instead of sixteen. Estimated 3.7 s to roughly 2.2 s. It changes
   floating-point association, so the golden values would have to be
   re-validated rather than preserved by construction.
2. **`zero_pad_factor` 8 to 4.** Halves the spectrum transform, estimated 3.7 s
   to roughly 3.1 s. It changes the bin grid and therefore the golden values.
   It is a research parameter that lives in the acoustic repository's
   `config/dsp.yaml`, so it is not a firmware decision.

At 3.7 s a 32-spoke measure pass plus a verify pass is under four minutes of
DSP, down from seventeen.

> **Superseded, 2026-09-04.** Option 1 above was half wrong and the error was
> worth catching: *radix-4* changes floating-point association, but *cache
> blocking on its own does not*. The butterflies of one pass are mutually
> independent, so the order in which a pass sweeps its blocks is free; every
> butterfly still runs on the same two operands with its own operations in the
> same order, and only the cache misses change. Blocking was therefore
> implementable **by construction** rather than needing the oracle re-validated,
> and it is now in (`bc3fa9c`), worth -16%. Conflating the two costs was what had
> put it behind an approval gate it did not need.
>
> The estimate was also drawn against the wrong cause. Most of the gap was never
> the pass count: a transform small enough to stay in the 64 KB data cache runs
> at 0.20 us per butterfly whether it is addressed in internal RAM or PSRAM,
> while the 131,072-point one runs at 0.98 us. It is misses, not bandwidth and
> not arithmetic.
>
> A pluck is now **3,070 ms**, and a 32-spoke measure plus verify pass is about
> 3.3 minutes of DSP. The floor is still 0.20 us per butterfly; reaching it needs
> a compact per-block twiddle table, since the twiddles of the larger blocked
> passes stride the whole table and begin missing on their own account. That one
> does change the plan's storage contract and its callers, and is not done.
> Option 2 is unchanged and still the owner's call.
>
> See `docs/BRINGUP_LOG.md` for the measurements, and note the separate finding
> there that the 3,666 ms figure this section was written against was itself
> luck: the acoustic scratch had landed on a favourable cache-line offset, and
> Phase 1e's allocations moved it (`5fcc74d`).

One wrinkle worth knowing: the **first** pluck of a session costs 7.3 s
because it builds the cached window, chirp, kernel and twiddle tables. That is
once per workspace, not per pluck.

## Phase 1e comms — what the protocol decides, and what it deliberately does not

**The wire layer rules on frames; the orchestrator rules on state.** The decoder
checks that a frame parses, names a known command, carries the fields that command
requires, and obeys the SPEC 12.3 `wait_id` rule. It says nothing about
admissibility. That keeps every SPEC 12.3 rule in one implementation and lets a
decoded command go to `truing_orch_submit_intent()` unexamined.

The `wait_id` rule splits across the two layers along exactly that line:

| Case | Rejected by | As |
|---|---|---|
| Omitted on a confirmation | the wire | `MISSING_WAIT_ID` — there is nothing to correlate |
| Zero on a confirmation | the wire | `MISSING_WAIT_ID` — zero is the "not answering a wait" sentinel |
| Present but not the active wait | the orchestrator | `STALE_INTENT` — only it knows which wait is live |
| Present on `ABORT` / `START_TRUING` | the wire | `UNEXPECTED_WAIT_ID` (see below) |

**Two interpretations worth flagging, because SPEC 12.3 does not spell them out.**
It says "ABORT and START_TRUING carry no wait_id" and, separately, that a client
which "omits or fabricates wait_id must be rejected, not accommodated". Read
together, a `wait_id` on an intent that does not answer a wait is refused rather
than ignored — extended to `SET_PARAMETER` and `DEBUG` on the same reasoning, since
neither answers a wait either. A client sending one has misunderstood the protocol,
and silently dropping the field would hide that. If the owner prefers tolerance
here, it is one branch in `truing_wire_decode_command()`.

**A duplicate confirmation on a still-open wait is idempotent, not stale.** The wait
is consumed inside `truing_orch_step()`, not at submission, so re-sending the same
answer before the machine steps merely records it twice. SPEC 7.3 is about a delayed
confirmation satisfying a *later* wait, and the `wait_id` prevents that. Noted
because the obvious test asserts the opposite and fails.

**Vocabulary comes from the firmware's own `truing_*_str()` tables**, and the
decoder's reverse lookups are derived from those same tables, so the protocol and
the logs cannot drift. The cost is that the tables are not uniformly cased —
statuses and stations are lowercase, reason codes and states uppercase — and that is
preserved rather than tidied, so a client reads the tokens that appear in the logs.

**Non-finite floats become `null`.** JSON has neither NaN nor infinity, and in this
firmware a non-finite float always means "no value here": an unmeasured reading, or a
rotation the position authority declines to vouch for. `null` says that.

**A frame that does not fit its buffer is dropped whole, never truncated.** For
best-effort telemetry losing a frame is correct; sending half of one is not.

**The session runs on the orchestrator's task.** The orchestrator is not thread-safe
and its snapshot and provenance readers take a mutable pointer, so answering a query
from the web server's task would race the state machine. The transport therefore only
moves bytes to and from two calls, which is what SPEC 12.4 already means by intent
being an event and the transport a driver. The payoff is that a command can be
submitted and its real SPEC 12.3 verdict returned in the same acknowledgement.

The consequence is that the outbound sink must never block, since it is called on the
control path SPEC 12.2 forbids telemetry from blocking. It copies the frame onto a
queue and returns; a sender task does the socket write. A slow or dead client fills
the queue and frames are dropped — permitted, and why the current-state query exists.

**Frame budgets.** Events and the state snapshot fit 512 bytes; provenance is the only
large frame at **6113 bytes of an 8192-byte budget** for a 36-spoke wheel, measured on
target. It is answered on demand, never streamed, so its size costs the telemetry path
nothing.

**Credentials are derived from the board's MAC** and logged at boot. A checked-in
passphrase would be a secret published to everyone who can read the source.

## The acoustic measurement lifecycle, and why it is telemetry rather than a wait

One call into the acoustic subsystem is one estimate out (SPEC §9.1), and that contract is
unchanged. But from the station that single call is two very different things back to back: a
**~1 s window the operator must pluck into**, then **~2–3 s of FFT they must not**. Nothing
outside the subsystem could tell those apart, so the page showed one *Measuring* card for both
and the only way to learn when to pluck was to guess. Worse, the orchestrator's bounded retry
(SPEC §7.4) re-opens a fresh window **without changing state**, so a failed attempt emitted no
`STATE_TRANSITION` and the operator was never told to try again.

That is an observability gap, not a wording problem. Mid-measurement the demo task is inside
`real_measure` for the whole four seconds, so even `GET_CURRENT_STATE` cannot be answered —
commands are serviced between orchestrator steps, and the measurement *is* a step.

`acoustic_real` therefore emits three best-effort events through an **optional observer**, wired
by the composition root exactly the way the pluck seam is:

| phase | emitted | means |
|---|---|---|
| `LISTENING` | immediately before `capture()` | the window is open — pluck now |
| `ONSET_DETECTED` | after the onset pass | the pluck was heard; stop plucking |
| `ANALYZING` | before `truing_dsp_analyze_window` | the spectral analysis proper |

**Measured on the Nano** (`nano_esp32_fastdemo_mic`, real INMP441), per attempt:
`LISTENING` → `ONSET_DETECTED` ≈ **1.08 s** (the capture window), `ONSET_DETECTED` →
`ANALYZING` ≈ **1.75 s**, `ANALYZING` → result ≈ **1.2 s**. So the plan's assumption that
window selection is cheap and the FFT is the cost is wrong by a factor of one and a half —
the Hilbert envelope and gating before `ANALYZING` cost *more* than the transform after it.
`ANALYZING` is therefore not "the start of the expensive part"; **`ONSET_DETECTED` is**, and it
is also the frame that tells the operator to stop. The split is kept because two cards read
better than one long one, but no timing conclusion should be drawn from `ANALYZING` alone.

**Why an observer and not `acoustic_if.h`.** The generic interface is the orchestrator's contract
and it stays a one-call contract. Only the application knows there is a transport to push to, so
only the application wires this.

**Why the net sink and not the telemetry ring.** The ring is drained *between* orchestrator
steps. A `LISTENING` event posted to it would arrive after the window it announces had already
closed — it would tell the operator to pluck into a window that ended four seconds ago, which is
worse than silence. The observer enqueues straight onto the transport's zero-tick queue instead.

**What it does not do.** The window is fixed-length and cannot end early on a pluck: onset
detection runs over the finished capture, so `ONSET_DETECTED` necessarily arrives at window
*end*, not when the string was struck. Early exit would be a DSP redesign. And every one of these
frames is droppable by the §12.2 contract — `test_the_estimate_is_identical_with_and_without_an_observer`
pins that a build with no observer measures identically, and the page falls back to the ordinary
measuring card when frames go missing. Capture timing never depends on the browser.

**Attempt numbers come from the subsystem, not the orchestrator.** Consecutive calls for the same
spoke in the same cycle *are* the retries, so `acoustic_real` counts them and the station can say
"attempt 2" without the orchestrator having to publish its own retry bookkeeping.

**The actuator is wired, and the payload tells the truth about it.** `excitation` is reported as
`HAND` or `ACTUATOR` from what actually happened this attempt — `pluck_commanded` is set by the
`fire()` call at the seam — and the page needs both before it says "Plucking spoke N": an
actuator that is wired but whose `fire()` failed reports `HAND`, and the ARMED lead-in runs for
that attempt, so every frame agrees with what the operator should do. The real front end wires
the GPIO actuator (`pluck_gpio`, `BOARD_PLUCK_ACTUATOR_GPIO`) in `orch_demo.c`; with no solenoid
attached the pulse is commanded into the pin and the hand pluck stays the excitation.

### SPEC 9.4 capture under WiFi load — what was actually tested

Deferred from 1f, which had no WiFi stack to load the capture with. Measured on the
DevKitC-1: a 1 s capture with the radio transmitting and a compute task at DSP
priority on core 1 produced **zero DMA overruns**, with a 53.3 ms worst read gap
against the 100 ms drain budget.

**Its scope, stated rather than implied.** The load is raw 802.11 injection on the
board's own AP interface, and the driver caps that near **91 frames/s** however it is
driven — letting it own the sequence numbers did not move it. No station was
associated, so no TCP path was in the picture. This is a genuinely active radio, not a
saturated one. **A capture taken while an associated browser streams telemetry through
a measurement cycle is a bench check and has not been performed**; the firmware would
report it as `CAPTURE_OVERRUN` (SPEC 13.2) if it failed. The check reports NOT
PERFORMED, rather than passing, when injection cannot start.

### A pre-existing watchdog this slice surfaced

The task watchdog was already firing before 1e — 66 reports in a boot — because an
acoustic analysis legitimately holds core 0 for 3.7 s (7.3 s for the first of a
session) inside a single orchestrator step, so the idle task cannot run. It mattered
now because the web server shares core 0. Fixed by yielding once per orchestrator
step — it must be *per step*, since one step can BE a measurement, so yielding every
64 would mean minutes of starvation — and by widening the watchdog to 20 s to cover a
single analysis rather than silencing it. Boot now reports **zero**.

This is a symptom, not the disease. SPEC 4.5 splits the cores and capture is already
pinned to core 1, but acoustic layers 2-4 still run inline on the control core. Moving
them is Phase 2 work and is listed under open items.

## Ambient-referenced SNR: measured on the campaign, and rejected (plan item I2)

The firmware's SNR gate (`truing_peaks_snr_db`, peaks.c) is a local spectral contrast: the
peak's magnitude minus the median log-magnitude of an annulus of bins 100-300 Hz either side
of f1. It never reads the pre-roll, `noise_floor_dbfs`, or anything time-domain, and the
question was whether an ambient-referenced measure — signal level against the ~200 ms of
pre-excitation ambient every bundle carries — would separate plucks from no-pluck controls
better. `tools/ambient_snr.py` computes it over all 64 bundles on disk (campaign passes A-D
plus the three checked-in fixtures; per-bundle numbers in
`test/fixtures/acoustic/captures/_campaign/ambient_snr.csv`, gitignored — evidence, not
tests). AC-coupled, on the same `(word >> 8)` scale the DSP sees.

**The result is a rejection, and it closes the question the right way round: the annulus SNR
is the better discriminator, and by a wide margin.**

| group | n | ambient-ref SNR (median / range) | annulus SNR (median / range) |
|---|---|---|---|
| A cleared — ring-down in window (7) | 7 | +0.7 / −1.4..+6.4 dB | +19.0 / +16.0..+24.6 dB |
| B cleared — strike in window (2) | 2 | +19.3 / +14.2..+24.4 dB | +15.7 / +13.5..+17.9 dB |
| C ambient, no pluck (18) | 18 | +4.3 / −2.4..+19.7 dB | +5.9 / +2.2..+9.9 dB |
| D hand plucks, 0 clears (12) | 12 | +4.7 / −3.3..+11.6 dB | +5.5 / +2.0..+10.9 dB |

- **The annulus SNR separates; the ambient-referenced SNR does not.** No-pluck controls top
  out at 9.9 dB annulus; cleared captures start at 13.5 dB — the existing 12.0 dB gate sits
  in that gap (the same conclusion the A4 sweep reached, by a second, independent method).
  Ambient-referenced, the ranges overlap hard: controls reach +19.7 dB while clears median
  +0.7 dB.
- **Why: the thing analysed is a tonal ring-down, and it is broadband-quiet.** A ring-down is
  spectrally prominent at f1 but its RMS decays into ambient within the 500 ms window — nine
  of the campaign's clears measure *near zero or negative* ambient-ref, several captures have
  a window QUIETER than their own pre-roll. A broadband RMS ratio is blind to exactly the
  feature that carries f1, and it counts the strike's broadband attack (pass B clears: +14 to
  +24 dB) as signal — the regime that otherwise always fails.
- **Ambient is not stationary at capture scales.** Pass-C windows reach +19.7 dB over their
  own pre-roll — rain gusts and ambient drift between the 200 ms pre-roll and the analysed
  window. An ambient-referenced gate would therefore admit room noise while rejecting genuine
  plucks: the exact silent-wrong-answer the pass-C controls exist to catch. This is also why
  the not-implemented preflight fields stay unimplemented: a noise-floor check measured on the
  pre-roll would be measuring a different moment than the one the verdict comes from.
- **Caveat that stands:** the pre-roll length is derived from metadata (`n_words` minus
  `capture_us`), not measured — audio_i2s.c clamps it to `ring_filled` and no field records
  how many pre-trigger words were delivered. "Assumed", not "confirmed". Recording the
  delivered pre-roll count in the capture diagnostics would firm this up for any future
  ambient analysis; deferred — it crosses the `audio_source_if` capture contract, and no
  ambient measure is gate-bound now.

Consequence for Phase B: the annulus SNR survives its own audit; the local spectral contrast
is not the problem on this chain. The Phase-B retune candidates remain `prominence_db = 12`
and window placement (the solenoid), fitted against the final excitation — with discrimination
against pass-C-style no-pluck controls as the acceptance rule, never raw pass rate.

## Board profiles stop at GPIO, and that is the right place for now

Four board-profile macros are read by **nothing** in the firmware:
`BOARD_HAS_SEPARATE_DEBUG_PORT`, `BOARD_HAS_PLAIN_STATUS_LED`,
`BOARD_STATUS_LED_GPIO`, `BOARD_STATUS_LED_ACTIVE_LOW`. They describe the boards;
they do not configure them. The header now says so, because a macro that looks
like configuration and is not is worse than no macro at all — the Nano declares
`BOARD_HAS_SEPARATE_DEBUG_PORT 0` and the console is configured identically for
both boards regardless.

**Should the console become board-specific? No, on the evidence.** The shared
`sdkconfig.defaults` sets UART0 as the primary console with the USB-Serial/JTAG
mirror as secondary, and that is portable across both boards as actually
deployed: the DevKit's UART0 reaches its CH343 bridge, and the Nano, which has no
bridge, carries the same log out of the mirror on its native USB. Both are proven
on hardware — every measurement in this repository from the Nano came off that
mirror. The Nano loses console *input*, which nothing uses.

The one place it does not hold is under the Nano's **stock Arduino bootloader**,
which owns the USB PHY differently. That is a deployment model this project does
not use: Arduino DFU writes an application only, into an OTA slot, so it cannot
place our bootloader or partition table at all. The supported route is the raw
esptool write in `docs/BUILD.md`. Supporting stock DFU as well would mean a
second partition layout, an OTA-slot-aware image and a console configuration that
survives the Arduino bootloader, for a deployment path with no project benefit.
**Recorded as unsupported rather than built.**

There is also no clean mechanism to reach for even if it were wanted. PlatformIO
passes only `-DSDKCONFIG=<generated path>` to ESP-IDF and never sets
`SDKCONFIG_DEFAULTS`, so both environments read the one shared defaults file; a
board manifest's `build.esp-idf.sdkconfig_path` relocates the *generated* file,
not the defaults. Per-board kconfig would need a custom pre-script. If that day
comes, `BOARD_HAS_SEPARATE_DEBUG_PORT` is the right thing for it to key on, which
is why the macro is kept rather than deleted.

## Phase 1c results (host)

| Check | Result |
|---|---|
| Native suites | 16 suites, 107 cases, all pass (`pio test -e native`) |
| Python suites | 32 pass, 1 skipped (`model_prep`, includes the binary export round trip) |
| Parity, d_ls per layout | worst relative deviation 9.8e-6 over 4 cases × FULL + TENSION_ABSENT |
| Parity, contract solve | plan turns equal the host `d_adj` under TENSION_ABSENT; FULL refused with MEAN_TENSION_MODEL_UNAVAILABLE exactly as the host recorded |
| Exact linear state | plan = −d_applied to 1e-4 rev; predicted lateral targets 0 to 1e-4 mm |
| Workflow on the real calculation | CONVERGED_GEOMETRIC_ONLY in 1 cycle, 22 adjustments, final max lateral 0.057 mm from a 0.274 mm start (the deadband keeps small turns unapplied) |

## Specification reconciliations

- **v1.1 Wheel Navigation clarification (SPEC 10A, 11.6).** The read-only
  "Position Tracking" subsystem of v1.0 was replaced, before any consumer
  existed, by the Wheel Navigation / Positioning contract: requests name a
  wheel feature and the station that needs it; outcomes are `DONE`,
  `PENDING_OPERATOR`, `IN_MOTION`, `REFUSED`, `FAULT`; the manual Stage 1
  implementation resolves through `WAIT_FOR_OPERATOR`; station angles are
  machine-profile data; actuator coordinates stay behind the wheel-drive
  contract. Positioning prompts gained a `station` and a rim-angle target,
  the session header a `machine_profile_id`, telemetry a navigation event, and
  the board profiles reserved wheel-drive pins. The specification was updated
  in the same change so both stay consistent.
- **SPEC 7.5.1 `consecutive_non_improving_cycles ≥ 2`** is guidance until the
  threshold is derived; validation enforces `≥ 1` so a derived value of 1 is
  not rejected by the data layer.
- **SPEC 8.9 `n_rim_angles = n_spokes`** is enforced as a cross-configuration
  rule (`truing_config_check_pair`), not inside `WheelState`, which accepts any
  supported grid so the data model does not encode the Stage 1 policy.

- **Two acoustic stations (SPEC 11.6, 16 station geometry, 10A.3, 9.1) — 2026-09-14.** The
  Stage 1 rig has two excitation solenoids, one per wheel flange, each at its own station on
  its own side of the wheel, because one fixed solenoid cannot reach both flanges.
  - **Stations.** The single `acoustic` station id became `acoustic_left` and `acoustic_right`,
    both required. The machine profile is now `profile_id` 2 with placeholder angles until the
    bench measures them. This closes SPEC 16's station-geometry item **for this rig only**, with
    the extension 10A.3 anticipates ("a station may be added").
  - **Decision ownership moved, deliberately.** SPEC 9.1 says the acoustic subsystem "owns none
    of" positioning, and 10A.3 says relocating a station never changes a consumer. Both are bent
    in one bounded way. *Which* acoustic station a spoke goes to depends on which solenoid can
    reach it, and that is a property of the excitation composite. So the rule lives in
    `truing_acoustic_station_for_spoke()` (spoke 0 → LEFT, alternating). The orchestrator asks it
    for the target station and still never sees an angle. The subsystem uses the same function to
    pick the actuator. The station *angles* stay machine-profile data owned by navigation.
  - **No hand-pluck fallback.** Readiness is a new acoustic contract hook, `ready()`, that
    admission checks. A session is refused while any station lacks an actuator, and a failed fire
    rejects the attempt before any capture.
- **Reason-code extension `EXCITATION_UNAVAILABLE` (SPEC 13.2).** A station's actuator is not
  installed or its fire() failed, so nothing was excited. Also the admission refusal for an
  acoustic subsystem that is not ready. Appended after `CAPTURE_OVERRUN`.
- **Excitation leaves the chain profile (SPEC 11.3).** SPEC 11.3's chain profile never listed an
  excitation field; the firmware had added `excitation_pulse_ms`. It moved into
  `truing_excitation_profile_t` (a pulse per acoustic station, its own digest) and the chain
  digest became `truing.chain_profile/2`. A per-station pulse change therefore never invalidates
  DSP evidence. The three checked-in capture bundles were migrated by derivation, with their
  original digest kept as `chain_digest_v1` (see `test/fixtures/acoustic/captures/README.md`).
- **Composite wheel navigation (SPEC 10A.2, 6.2) — 2026-09-19, plan B1b / A6.** The acoustic
  demonstration images fire real solenoids over a synthetic runout. With synthetic navigation the
  orchestrator believed each spoke was at its acoustic station while nothing moved, so the derived
  station's actuator struck whatever spoke was there and the result was filed under a spoke that was
  never struck. `navigation_composite` routes by station: LEFT and RIGHT go to `navigation_manual`
  (the operator moves the wheel), every other station to `navigation_synthetic`. Simulated requests
  are refused until the physical reference exists.
  - **Where it applies.** Real-front-end images without the campaign debug channel
    (`nano_esp32_mic`, `nano_esp32_fastdemo_mic`, `s3_devkit_fastdemo_mic`). The **campaign bench
    image keeps synthetic navigation**: `MEASURE_ONCE` names the spoke per shot and is admitted only
    in READY, so a composite there would stall INITIALIZE at the reference confirmation. This is a
    decision taken while implementing, reversible, and the campaign never runs a session. `/id`
    reports `composite_navigation` and the UI wording follows that fact, not an inference.
  - **What it removes.** The orchestrator's belief in a position that nothing produced. **What it
    does not verify.** Placement: `sensor_confirmed` stays false, so a spoke put at the wrong
    station, or a wheel put on backwards, is still filed under the wrong spoke with no reason code.
    `MEASURE_ONCE` bypasses navigation entirely.
  - **Reinterpretation, for the owner.** `query()` answers from the physical child only, so a
    stand-in `DONE` (rim positioned at the runout station) is not reflected in the reported
    `wheel_position`. That bends 10A.2's "DONE = the feature is at the station" for simulated
    stations, defensible only because that data is simulated and the source is labelled.
  - **Provenance label, open.** The composite's `source_impl` is the least real of its children, so
    it reports SYNTHETIC. The vocabulary has no "mixed" value; adding one is a spec reconciliation
    left to the owner. The mix appears in `impl_name` and telemetry, but only the boolean derived
    from `source_impl` reaches the session record, so "operator-confirmed at the acoustic stations"
    is not in the session header.
  - **Evidence.** Host tests only (routing and authority, reference gate, weakest-link provenance,
    pending-drop and stop, bad configuration, and N = 1 and N = 2 workflow runs). Nothing has run on
    a board or with an operator.

## Model-preparation findings (Phase 1b) — need the owner's decision

Measured on the SYNTHETIC sym32 fixture (bike-wheel-calc example rim section,
2.0 mm spokes, 45 mm flanges at 35 mm offset, 3-cross, 1000 N, N_lat 6, N_rad 13,
32 rim angles); numbers will differ for the real wheel but the structure will not.

1. **Equal tightening is not displacement-invisible in the analytical model.**
   Tightening every spoke by 1/√32 turn produces zero lateral response (5e-16 mm)
   but a UNIFORM radial contraction of the rim of 0.025 mm (non-uniform remainder
   2e-4 mm). Consequently the tolerance-normalised displacement block has full
   rank (32) for BOTH layouts (σ_min 1.07, effective condition 47.9,
   `expected_null_dim = 0`), and condition (i) of SPEC 8.4 Part 1 fails with
   residual 2.027, of which the uniform radial part is 2.027 and the remainder
   0.017. The artifact records all of this and sets `identified = false`; the
   runtime therefore refuses mean-tension targeting and truing runs under
   `TENSION_ABSENT` (SPEC 8.4 fallback), which is the conservative behaviour the
   specification mandates. **The open question:** whether condition (i) is meant
   to hold for *runout* (non-uniform displacement, i.e. after removing the n = 0
   radial mode, which a tare at the reference location would absorb) or for raw
   displacement including a uniform radius change. The specification's
   "structurally rank-deficient TENSION_ABSENT" expectation is not borne out by
   the model under the raw reading. This is a solver-semantics decision (SPEC
   consistency guardrail §1) and was NOT decided here; the raw reading is
   implemented and the decomposition is recorded so either choice can be made
   without regenerating anything but the flag.
2. **Condition (ii) deviation of 0.38 %.** The tension response to equal
   tightening is uniform to within 0.75 % between the two symmetric class pairs
   (B-lead/A-trail: 110.9 N per turn; A-lead/B-trail: 111.8 N per turn); within
   each pair it is exactly equal. The fixture's `n_mt_tension_tolerance` (1e-3)
   is tighter than that. Whether the project tolerance should admit this
   lead/trail effect is a configuration decision; the gate asserts the recorded
   rule and the pair structure, not a pass.
3. **Single-solve sensitivity to gauge noise.** With 0.01 mm noise on every
   lateral and radial reading, one `TENSION_ABSENT` solve on this wheel recovers
   a 0.2-turn-scale disturbance with ~36 % error (norm bound 0.80 turn). This is
   the reason the outer cycle loop and the non-decrease abort exist (SPEC 8.1,
   7.5); it is not a defect, but it sets expectations for Stage 1 dial
   readings.
4. **FULL-layout inversion and the tension scale.** Because the tension residual
   is `T − s·T_norm` with `s` the current scale, the mean of the tension
   disturbance is removed before the inversion; `d_ls` equals `d_applied` plus
   exactly the pseudoinverse image of that removed mean (identity holds to
   1e-15; ~1e-5 relative at trust_tension = 1e-5). The gate asserts both.

Reconciliations made for 1b: the nipple thread pitch (spoke shortening per
revolution) is required to express Φ per revolution and is not named in the
SPEC 11.1 field list; it is treated as spoke geometry, recorded in the
generating parameters and fingerprinted (SPEC 8.7). The Rayleigh-Ritz mode
count of the bike-wheel-calc solution (36) and the dense fit sample counts
(256, ≥ 4× the floor) are generator parameters, recorded and fingerprinted. The
fixture rim section is bike-wheel-calc's own example section, labelled as such.

## Phase 1c observation for the owner — `adjustment_deadband` semantics

SPEC 11.2 says adjustments below `adjustment_deadband` are skipped "when the
wheel state at that spoke is already within tolerance", and `plan.c` does
exactly that. On the target self-play (docs/BRINGUP_LOG.md, 2026-09-03) this
produced twelve APPLY_ADJUSTMENT prompts of −0.000 rev at spokes whose own
turn was numerically zero but whose rim index was out of tolerance because of
a neighbour's error. A human operator would find those prompts pointless;
skipping every sub-deadband turn regardless of local state would remove them
without changing any applied adjustment. This is a rule change, so it is not
made here.

## Phase 1f results (host)

| Check | Result |
|---|---|
| Native suites | 18 suites, all pass (`pio test -e native`) |
| Python suites | acoustic_prep 2 pass; model_prep 32 pass, 1 skipped |
| Golden parity, 3 recorded plucks | worst deviation 0.0000 Hz in f1, 0.000 dB in SNR, 0.004 dB in peak prominence |
| Recorded noise floor | rejected, NO_ONSET_DETECTED, no tension |
| Workflow on the real DSP | CONVERGED_GEOMETRIC_ONLY in 1 cycle, 64 acoustic calls, 64 estimates, 0 rejections, every stored tension suspect / PROVISIONAL_MODE_ID |

## Open items carried forward (do not invent)

Everything in SPEC 16 and 10A.10. In addition, for this phase: no real wheel
configuration exists yet (hub geometry, `c` per side, the influence fingerprint
and the tension-model parameters all require measurement or model preparation),
so a normal session cannot be admitted until 1b and a measurement campaign
supply them; the firmware reports this honestly as "not provisioned".

**Known limitation: acoustic spoke identity vs the solver's `indexing_origin` (2026-09-14).**

- **What it is.** The acoustic subsystem's convention (spoke 0 is the spoke the LEFT solenoid
  strikes, alternating) is deliberately **not** reconciled with the artifact-bound
  `indexing_origin` (SPEC 6.5). That origin is what maps spoke indices to influence-matrix
  classes and to the nipple-adjustment instructions.
- **Why the demo is safe.** It is safe while runout and the solve are synthetic, as in the fast
  demo.
- **What must happen first.** **Before any real physical truing session** (real runout, real
  adjustments on a real wheel), the two identities must be reconciled. That reconciliation
  belongs to the mechanical-designation campaign (Side A/B, hub/flange geometry), not to the
  acoustic subsystem.
- **Related hazard.** The station convention is compiled. A wheel mounted flipped, or an S0 the
  LEFT solenoid cannot reach, would strike a neighbouring spoke and file it under the wrong index
  with no reason code (SPEC 17.3 #11 class). Setting any `BOARD_PLUCK_ACTUATOR_*_PRESENT` to 1
  therefore requires the bench attribution check (plan B2 M8) to have passed on the mounted
  wheel.
- **Update (2026-09-17, campaign plan Amendment 1).** The current donor wheel turns out to have
  four physical spoke classes (side x leading/trailing), and the operator's description of the
  rig lines up with the solver's generator order at offset 0: `indexing_origin = (Side B,
  LEADING)`, with RIGHT = Side A observed for this wheel's orientation (rotor on RIGHT, SPEC
  §6.4.1). The reconciliation this limitation describes is therefore now down to **declaring
  that one indexing origin and confirming the S0..S3 class pattern at B2-M8** — not an open
  research question — but it remains a limitation until M8 confirms it and a physical truing
  session actually makes the declaration. It stays scoped to this donor wheel's observed
  orientation, not stated as a general rule. See `SOLENOID_CAMPAIGN_PLAN.md` Amendment 1 (C1–C2)
  and `SOLENOID_CAMPAIGN.md`'s Convention section.

Carried forward from the 2026-09-04 DSP and build-hygiene pass:

- **The transform is still four times its own compute floor.** 0.79 us per
  butterfly against 0.20 measured cache-resident. Closing it needs a compact
  per-block twiddle table, which changes `truing_fft_plan_t`'s storage contract
  and every caller that sizes it. Not attempted; the estimate is worth roughly
  another third of a pluck.
- **`zero_pad_factor` 8 to 4** remains available and remains the owner's call: it
  changes the bin grid and therefore the golden values, and it is a research
  parameter in the acoustic repository's `config/dsp.yaml`, not a firmware one.
- **Stock Arduino DFU deployment of the Nano is unsupported by decision**, not by
  accident. See the board-profile section above.

Carried forward from 1e:

- **Acoustic layers 2-4 run inline on the control core.** SPEC 4.5 splits the
  cores and capture already honours it, but the analysis does not, which is why a
  single orchestrator step can occupy core 0 for seconds. Phase 2.
- **Capture under an associated client's traffic is unverified.** See the SPEC 9.4
  scope note above — the automated check loads the radio but not a TCP path.
- **No USB CDC fallback yet** (SPEC 12.1). The frames are transport-independent by
  construction, so this is a driver, not a protocol change.
- **Artifact loading over the protocol is not implemented.** SPEC 12.3.1 requires
  artifact-bound changes to go through host-side regeneration and reload; the
  reload half has no wire command yet, and `SET_PARAMETER` correctly refuses those
  fields with `REQUIRES_ARTIFACT_REGENERATION` in the meantime.
