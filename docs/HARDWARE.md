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
| fm1-core platform | boots, screen, knob, keys, buttons, audio out | verified 2026-11-17 |
| Panel map | buttons 0 OCT− 1 OCT+ 2 FX 3 SEL 4 ENV 5 LFO 6 EDIT 7 GLO 8 HOME 9 SAVE 10 ARP 11 SEQ 12 PLAY 13 REC; keys 0..26 = F3..G5; encoders 0 SELECT 1 ALGORITHM 2–5 KNOB1–4 6 PRESETS; master knob = `knob` | verified 2026-11-17 |
| Free heap (largest block, ~3 s after boot) | ≥ 256 KB (probe was capped at 256 KB) | verified 2026-11-17 |
| User flash via `fm1_flash_*` | mounts, writes, survives power-cycle; ERISIAN share 440 KB | verified 2026-11-17 |
| Rescue combo | OCT− at power-on → ERISIAN rescue mode (no flash access) | verified 2026-11-17 |
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
- Recovered: restored V15 (as shipped) with M-Flasher, keeping the M-Flasher
  bootloader. Everything works. Verified on Charles's unit (0x856014).
- Same package in the FM-1 web emulator: blank screen, no sound. So the fault
  is in the build/package, not the unit.

Note: with the M-Flasher bootloader, REC (not OCT−) at power-on is the
bootloader's download mode. ERISIAN's own rescue combo (OCT−) is separate and
runs inside the firmware.

### 2026-11-17 — sine example with RAM sections (CI run 10, `fm1.fwsc` sha256 b727d42c…) — WORKS
Verified on Charles's unit (0x856014).
- Installed with M-Flasher's firmware-update function.
- Boots. Screen: blue band and green level bar, as drawn by `examples/sine.c`.
- Master volume knob moves the green bar (knob ADC works; `fm1_controls.knob`).
- Every button/key gives a tone while held (key matrix, buttons and audio output work).
- The 7 encoders do nothing and the blue band never changes: expected, the
  example does not use them.
- First confirmed boot of fm1-core on hardware, after the `build.py` image fix.
- Not yet checked: LEDs under pressed keys, exact pitch (440 Hz expected).

### 2026-11-17 — first ERISIAN build (CI run 18, `erisian.fwsc` sha256 5c8c8f28…) — WORKS
Verified on Charles's unit (0x856014).
- Boot screen with ERISIAN name and version.
- All 14 buttons report the expected bit and name (the label is SEL, not
  "SCL" as fm1-emu calls it; fixed in the firmware).
- Encoders: SELECT 0, ALGORITHM 1, KNOB1 2, KNOB2 3, KNOB3 4, KNOB4 5,
  PRESETS 6. Master knob = `fm1_controls.knob`.
- RAM: largest free block 256 KB, the probe's cap, so at least that much.
- FLASH OK 412K free after the kick upload (440 KB share).
- Kick uploaded over USB with tools/uploader (Chrome/Edge Web MIDI):
  "USB KICK SAVED", "KICK 13553 FR" (0.31 s, 16-bit mono 44.1 kHz).
  First real use of the store, the SysEx upload and USB MIDI replies.
- Kick playback from RAM on key press; master knob sets the level.
- Power-cycle: kick still present (KICK 13553 FR): the store survives a reboot.
- OCT− held at power-on boots into RESCUE MODE.

### 2026-11-17 — first drum machine (CI run 26, `erisian.fwsc` sha256 50c63a5a…) — WORKS
Verified on Charles's unit (0x856014).
- Grid, starter beat, PLAY, voices, steps and parameter pages look and sound good.
- On-device FM voices sound close to the host demo render (`demo.wav`).
- Uploaded kick plays on voice 1.
- **The beat stutters during SAVE**: the runtime silences audio during flash
  erase/write, and a save erased a sector and wrote ~3.4 KB in one go.
  Next build: pre-erase free sectors while stopped, write saves in 256-byte
  chunks.
