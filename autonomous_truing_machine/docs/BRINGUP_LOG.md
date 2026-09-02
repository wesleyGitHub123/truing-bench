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
