# Bring-up log — ESP32-S3-DevKitC-1

Hardware facts below were measured on the connected development board; nothing
in this file is assumed from a datasheet. Board on `COM4` (CH343 UART bridge),
firmware `0.1.0-phase1a`, ESP-IDF 5.4.0, image 299 KB (7.1 % of the 4 MB app
partition), 22 KB static RAM.

## 2026-09-02 — Phase 1a bring-up

### Platform (SPEC §4.1: "verify on the actual board")

| Property | Measured | Board profile expectation | Result |
|---|---|---|---|
| Chip | ESP32-S3, silicon revision v0.2, 2 cores, features 0x12 (WiFi, BLE) | — | — |
| Flash | 16,777,216 bytes, quad (eFuse) | 16 MB | PASS |
| PSRAM | 8,388,608 bytes, octal (vendor AP, 64 Mbit, generation 3), 80 MHz, memory test OK | 8 MB | PASS |
| Module identification | consistent with **ESP32-S3-WROOM-1-N16R8** | N16R8 | confirmed |

`esptool flash_id` alone cannot see module-level PSRAM (it reports only chip
features); the runtime check is the authoritative one.

### Memory placement (SPEC §9.4.1)

| Check | Measured | Result |
|---|---|---|
| Heap after boot | internal 454,935 total / 366,819 free; SPIRAM 8,388,608 total / 8,386,148 free | — |
| 1 MiB `MALLOC_CAP_SPIRAM` allocation | lands in external RAM | PASS |
| Internal DMA-capable pool | 359,315 free, largest block 237,568; reserve `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL` = 65,536 | — |
| 4092-byte `MALLOC_CAP_INTERNAL \| MALLOC_CAP_DMA` allocation | internal, DMA-capable | PASS |
| Plain `malloc(200 KiB)` with PSRAM enabled | lands in **external** RAM | evidence for invariant 2: never rely on `malloc()` placement for DMA |

### I2S channel-creation probe (SPEC §9.4.1 bring-up check 5)

With PSRAM enabled and the sdkconfig committed in this repository:

| Call | Result |
|---|---|
| `i2s_new_channel(rx, dma_desc_num=6, dma_frame_num=240)` | `ESP_OK` |
| `i2s_channel_init_std_mode(48 kHz, 24-bit data in 32-bit slots, bclk=GPIO4, ws=GPIO5, din=GPIO6)` | `ESP_OK` |
| `i2s_del_channel` | `ESP_OK` |

The known N16R8 failure mode (driver DMA allocations landing in PSRAM) does not
occur with this configuration. The probe's DMA sizing is probe-only; Phase 2
derives the real sizing from the WiFi-load stress test. No audio was captured.

### Core self-test on target

The framework-free core ran on the ESP32 against the same synthetic
implementations the host tests use: row dimensions for 32/36 spokes, fixture
configuration validation including `CALIBRATION_MISSING` for an unestablished
`L_eff`, blob encode/decode and CRC rejection, a 32-spoke synthetic
measurement pass through `WheelState` and the layout rules (FULL, then
PARTIAL_WHEEL_STATE on one rejected row, then TENSION_ABSENT), the SPEC §7.3
stale-confirmation scenario, manual and synthetic wheel navigation (SPEC §10A),
and the session header's non-real-implementation flag. **19 of 19 passed.**

### Configuration persistence (SPEC §11.5)

1. Fresh board: NVS initialised, `0/5 kinds provisioned`, reported as
   "START_TRUING would not be admissible".
2. Provisioning build (`-DTRUING_BRINGUP_PROVISION_FIXTURE_CONFIG=1`): each of
   the five SYNTHETIC fixture blobs saved to NVS and read back byte-identical
   (wheel class 105 B, solver 99 B, chain 52 B, tension model 63 B, machine
   profile 55 B). Loudly logged as fixture data, not a real wheel.
3. Plain image rebuilt from scratch, re-flashed, board rebooted: `5/5 kinds
   provisioned, 5 valid`, CRCs identical to step 2
   (`517fd042`, `5e94498c`, `91bd57e2`, `cb6e6a91`, `5e112e78`).

### Summary

`BRINGUP SUMMARY: passed=29 failed=0 | flash=ok psram=ok i2s_probe=ok nvs=ok core_selftest=ok`
(34 of 34 on the provisioning boot). Heartbeat over 20 s: free internal heap
stable at 370,051 bytes, minimum-ever free 360,603 bytes, SPIRAM free stable.

### Not verified (by design of this phase)

- No audio capture, no WiFi, no stress test (Phase 1f / 2).
- The Arduino Nano ESP32 environment **builds** (same image size) but was not
  flashed: no Nano is connected.
- No real wheel configuration exists; the board currently holds the SYNTHETIC
  fixture configuration from step 2, which must be replaced before any real
  session.

### Capture method

`tools/serial_capture.py COM4 <seconds> <outfile>` resets the board through the
CH343 auto-reset lines and records the console; raw captures are kept out of
source control.

## 2026-09-02 — Phase 1d orchestrator self-play on the DevKitC-1

Firmware `0.1.0-phase1a` + orchestrator (image 316 KB, 36 KB static RAM). After
the bring-up, `orch_demo` runs the complete Stage 1 workflow on core 0 with
the **manual** navigation and **manual** runout implementations answered by the
auto-operator (playing the human), synthetic acoustic estimates and the
synthetic calculation double. A synthetic starting state with two spokes out of
true (lateral +0.4 mm at rim index 3, −0.3 mm at index 10).

| Observation | Value |
|---|---|
| Terminal result | `CONVERGED_GEOMETRIC_ONLY`, reason `TENSION_NOT_VERIFICATION_GRADE` (the honest Stage 1 ceiling, SPEC §8.6.3) |
| Outer cycles run | 1 (both adjustments applied, verification re-measured as cycle 2) |
| Orchestrator steps / state transitions | 660 / 660 |
| Operator waits answered | 197 = 131 positioning confirmations (1 reference + 64 measure + 2 apply + 64 verify) + 64 runout entries + 2 adjustment confirmations |
| Tension / runout records | 64 / 64 |
| Adjustments displayed | spoke 3: −0.400 rev; spoke 10: +0.300 rev; synthetic wheel corrected to 0.000 mm at both |
| Provenance (P6) | session 1, artifact 42 with fingerprint, tension-model profile 1, chain 1, machine 1, layout FULL, 96 active rows (64 valid, 32 suspect), wheel rotation 1.767 rad operator-confirmed, `contains_non_real_implementations = 1` |
| Telemetry ring | 1248 events emitted, 0 dropped (drained every step); no intents rejected |
| Wall time | 6.6 s, dominated by console logging at 115200 baud; no settle delays were needed |
| Heap after the run | internal free 356,027 bytes, minimum-ever free 333,899 bytes |

This proves the state machine, operator-wait correlation, navigation-by-outcome,
admission, apply loop, verification and provenance on the target. It proves
nothing about truing a wheel: every measurement and every solve was synthetic.

## 2026-09-03 — Phase 1c influence artifact and real truing calculation on the DevKitC-1

Firmware `0.1.0-phase1c` (image 369 KB flash, 100 KB static RAM; the expanded
artifact accounts for 57 KB of that). Bring-up: **39 checks passed, 0 failed**
(the 29 of Phase 1a plus the 10 below). Same capture method as before.

### Artifact load and checks (SPEC 8.7, 8.11 R3, 11.4)

| Observation | Value |
|---|---|
| Golden artifact `fixture_sym32` (id 1, fingerprint `25b68917ad02f8b4…`) | loaded from flash: integrity (SHA-256 content hash), compatibility (configured expected fingerprint), shape all pass |
| Compact → expanded | 30,085 B in flash → 56,720 B in internal RAM, 86.6 ms including SHA-256 and the Fourier expansion of 32 × 32 lateral and radial columns |
| Recorded conditioning | FULL rank 32, cond 47.8; TENSION_ABSENT rank 32, cond 47.8; both ≤ max_condition_number |
| Common mode | `n_mt_identified = 0` with `T_target` present (the Phase 1b finding travels with the artifact) |
| One flipped payload bit (PSRAM copy) | rejected: ARTIFACT_INVALID / content_hash |
| Different configured expected fingerprint | rejected: incompatible |
| N_rad + 1 in the solver configuration | rejected as a shape mismatch, nothing reshaped |

### Parity with the host reference and solve cost (SPEC 14.3.5)

| Observation | Value |
|---|---|
| d_ls, 4 cases × FULL + TENSION_ABSENT | worst relative deviation 7.4e-6 against the numpy float64 reference (stated tolerance 1e-5) |
| J (row-count-normalised cost) | worst relative deviation 2.6e-7 |
| Residual + inversion, FULL (32 × 96) | 626 µs per solve |
| Residual + inversion, TENSION_ABSENT (32 × 64) | 411 µs per solve |
| Full contract solve (admission + residual + inversion + plan) | 466 µs |
| Exact linear state (u = Φ_u d, v = Φ_v d) | plan recovers −d to 1.8e-7 rev |
| Policy | unidentified n_mt → TENSION_ABSENT with MEAN_TENSION_MODEL_UNAVAILABLE recorded |

The per-cycle calculation is therefore negligible against the operator-paced
workflow; nothing in Phase 1c needs PSRAM or the second core.

### Workflow self-play on the real calculation

`orch_demo` now runs the same manual-navigation / manual-runout self-play as
Phase 1d but with the artifact-backed calculation, and the simulated wheel
answers each adjustment through the artifact's own influence model
(u += Φ_u d, v += Φ_v d). Starting state: the model's response to +0.30 rev on
spoke 3, −0.25 rev on spoke 10 and +0.15 rev on spoke 21.

| Observation | Value |
|---|---|
| Starting error | max lateral 0.221 mm (tolerance 0.10 mm) |
| Terminal result | `CONVERGED_GEOMETRIC_ONLY`, reason `MEAN_TENSION_MODEL_UNAVAILABLE` (layout TENSION_ABSENT by policy, since the artifact's common mode is unidentified) |
| Outer cycles run | 1; verification re-measured as cycle 2 |
| Plan | spoke 3 −0.300 rev, spoke 10 +0.250 rev, spoke 21 −0.150 rev: the exact inverse of the perturbation |
| Adjustments the operator was asked to apply | 15 = the 3 above plus 12 of −0.000 rev at spokes whose local lateral runout was outside tolerance (the deadband rule skips only when both the turn is below the deadband and the local runout is within tolerance) |
| Final simulated wheel | max lateral 0.0000 mm; lateral at rim indices 3 and 10 exactly 0 |
| Steps / transitions / waits answered | 725 / 725 / 223 (144 positioning, 64 runout entries, 15 adjustment confirmations) |
| Provenance | session 1, artifact 1 with the golden fingerprint, layout TENSION_ABSENT, 64 active rows (64 valid, 0 suspect), `contains_non_real_implementations = 1` |
| Telemetry ring | 1,365 events, 0 dropped; no intents rejected |
| Wall time | 7.3 s, console-bound |
| Heap after the run | internal free 292,723 bytes, minimum-ever 270,363 bytes; SPIRAM untouched by the solve |

The calculation is real and artifact-backed; the acoustic estimate is still
synthetic and the wheel is a simulation of the artifact's own linear model, so
this run proves the solver and the workflow around it, not the truing of a
physical wheel. The twelve −0.000 rev prompts are worth an owner's look: they
are what the SPEC deadband rule prescribes when a neighbour's error puts a rim
index out of tolerance, and a human operator would find them pointless.

## 2026-09-03 — Phase 1f acoustic subsystem on the DevKitC-1

Firmware `0.1.0-phase1f` (image 917 KB flash, 100 KB static RAM; the growth
over 1c is the recorded campaign excerpts compiled in as C arrays). Bring-up:
**52 checks passed, 0 failed.** Same capture method as before.

### I2S front end (SPEC §9.3, §9.4, §9.4.1)

| Observation | Value |
|---|---|
| Channel | 48 kHz, 24-bit in 32-bit slots, mono; 8 descriptors x 240 frames (960 B each, a multiple of 3, under the 4092 B limit) |
| Drain task | core 1, priority 23, separate from the DSP path (SPEC §4.5) |
| Ring | 48,000 words in PSRAM, 9,600-word pre-trigger tail |
| 200 ms capture | OK, 9,600 words returned in 2 ms, entirely from the pre-trigger tail, which is the intended behaviour when the request is no longer than the tail |
| Captured signal | every word zero: **no microphone is connected to GPIO 6, and this run verifies nothing about a transducer** |
| Live `measure_spoke_tension` | rejected / NO_ONSET_DETECTED in 1,430 ms, after commanding a 19,098 µs excitation pulse; a status, never a fabricated value |
| 1 s capture with a compute task at DSP priority on core 1 | no overrun; 20 reads, longest gap between successful reads 95,954 µs |

The longest read gap is close to the 100 ms read timeout, but each read
requests 40 ms of audio and therefore blocks for about that long, so the
figure mostly measures the request size rather than starvation. No overrun
occurred. The mandatory test in SPEC §9.4 is capture integrity under **WiFi**
load, and this build has no WiFi stack: that test is deferred to Phase 1e and
is **not** claimed here.

### Layers 2–4 against the Python reference, on target (SPEC §14.2)

Three recorded plucks from the tension-sweep campaign, compiled in as C arrays
and analysed by the same code the host tests exercise.

| Pluck | f1 (Hz) | reference | SNR (dB) | candidates / strong | T ideal-string (N) | analysis |
|---|---|---|---|---|---|---|
| ts00_e2 | 423.886 | 423.886 | 45.90 | 8 / 12 | 663.7 | 17.96 s |
| ts03_e2 | 379.800 | 379.800 | 75.61 | 5 / 5 | 532.8 | 16.35 s |
| d3_e1 | 432.039 | 432.039 | 57.38 | 7 / 7 | 689.4 | 16.30 s |

Worst deviation across all three: **0.0000 Hz in f1 and 0.000 dB in SNR.**
Onset sample, gate start, window length, transform length, candidate count and
strong-peak count are identical to the reference, and every estimate is
`suspect` / `PROVISIONAL_MODE_ID` / `presumed_fundamental`. The recorded noise
floor (−70.8 dBFS rms) is rejected with `NO_ONSET_DETECTED` and yields no
tension. The DSP scratch is 6,542,832 bytes in PSRAM for a 262,144-point
transform.

Two things this does **not** show. The tensiometer read 1332.8 N for the
ts00 pluck while the ideal-string model on the fixture profile gives 664 N;
the fixture's `L_eff` is the crossing distance, which is synthetic content,
and the effective-length question of SPEC §4.4.1 remains open. And the
analysis takes **16 to 18 seconds per pluck** against roughly 0.6 s on the
host, which is recorded as the phase's open performance item in
docs/IMPLEMENTATION_NOTES.md.
