# Hardware log

Facts verified on real hardware. Each entry: date, what, and
"verified on Charles's unit (0x856014)".

## Unit

| Item | Value | Status |
|---|---|---|
| Device | M-VAVE FM-1 (M-VAVE logo under screen) | — |
| SoC | JieLi AC791N / WL82, 2× pi32v2 | from public sources |
| Flash | Puya P25Q80H, JEDEC `0x856014` | read in M-Flasher |
| Stock firmware | V15 | confirmed by Charles |
| Full flash backup | taken before first custom flash | done |
| Bootloader | M-Flasher bootloader installed (2026-11-17); **hold REC at power-on → download mode** | verified |

## Unverified platform facts (from public sources / fm1-core)

- Screen 240×240; audio 44.1 kHz, 128-frame blocks (per `include/fm1_block.h`), 24-bit stereo in int32.
- 1 MB external SPI flash. `tools/package.py` layout: app image, VM
  (settings) area, BT-info reserve, user reserve at the top of flash.
- Stock firmware runs one core.
- Newer units (autumn 2026+) use UCUN UC25HQ80, `0xB36014`.
- fm1-core: builds/links/packages, **never run on hardware** (its README says so).
- fm1-core platform sources contain no JEDEC ID handling; flash access is
  inside the binary SDK / boot resources. Whether they accept `0xB36014` is
  unknown — to test on a newer unit or by inspection.

## Log

### 2026-11-17 — sine example (CI run 3, `fm1.fwsc` sha256 221c5f87…) — FAILED
Verified on Charles's unit (0x856014).
- Installed with M-Flasher's firmware-update function.
- Result: screen flickers quickly and dimly; no sound, no LEDs, nothing on USB.
  Most likely a boot loop (early crash or watchdog reset) before USB/audio start.
- M-Flasher then offered its own bootloader for recovery; installed it.
  Holding REC at power-on now shows the M-Flasher download-mode screen.
- Status: recovery to V15 / full backup pending.

Note: with the M-Flasher bootloader, REC (not OCT−) at power-on is the
bootloader's download mode. ERISIAN's own rescue combo (OCT−) is separate and
runs inside the firmware.
