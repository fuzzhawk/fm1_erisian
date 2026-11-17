# Hardware log

Facts verified on real hardware. Each entry: date, what, and
"verified on Charles's unit (0x856014)".

## Unit

| Item | Value | Status |
|---|---|---|
| Device | M-VAVE FM-1 (M-VAVE logo under screen) | — |
| SoC | JieLi AC791N / WL82, 2× pi32v2 | from public sources |
| Flash | Puya P25Q80H, JEDEC `0x856014` | read in M-Flasher |
| Stock firmware | V15 or V16? | **to check** |
| Full flash backup | — | **to do (two copies, off-machine)** |

## Unverified platform facts (from public sources / fm1-core)

- Screen 240×240; audio 44.1 kHz, 256-frame blocks, 24-bit stereo in int32.
- 1 MB external SPI flash. `tools/package.py` layout: app image, VM
  (settings) area, BT-info reserve, user reserve at the top of flash.
- Stock firmware runs one core.
- Newer units (autumn 2026+) use UCUN UC25HQ80, `0xB36014`.
- fm1-core: builds/links/packages, **never run on hardware** (its README says so).
- fm1-core platform sources contain no JEDEC ID handling; flash access is
  inside the binary SDK / boot resources. Whether they accept `0xB36014` is
  unknown — to test on a newer unit or by inspection.

## Log

_(nothing verified yet)_
