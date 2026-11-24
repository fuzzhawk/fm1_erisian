# ERISIAN (FM-1 custom firmware) — rules for Claude Code

## Never
- Never commit the JieLi toolchain, or any file from it.
- Never flash, erase or write to a device. Charles flashes by hand.
- Never write code that erases or writes the bootloader / update-loader
  region of flash. App code may write only its own data region.
- Never enable overclocking or change core voltage.
- Never reuse another firmware's package identity or USB device name.
- Never copy GPL-3.0 code (FM1-open app code, Felucca, sloop, NoteSorcery,
  haunted-fm1…) into this repo: it would make the project GPL. Ideas only.

## Always
- Default to ONE core. Dual core is opt-in, behind a setting, with a
  fallback to one core if core 1 faults or stalls.
- Accept both flash JEDEC IDs: 0x856014 (Puya P25Q80H) and
  0xB36014 (UCUN UC25HQ80). Unknown ID → refuse to write, show a message.
- Storage writes must be torn-write safe: every record carries a sequence
  number + CRC, the newest valid copy wins, and the only good copy of
  anything is never overwritten (see docs/DESIGN.md §10).
- Keep a rescue path at boot: check the rescue key combo
  (OCT− held at power-on) before anything else runs.
- Every change must build in CI. Read failures from the Actions job logs.
- Add or update host tests for engine, storage and UI logic.
- Log anything verified on hardware in docs/HARDWARE.md, marked with the
  date and "verified on Charles's unit (0x856014)".

- Follow docs/DESIGN.md for features, controls and priorities.
- MAVIS (mavis/, docs/MAVIS.md) is a second firmware built from this repo;
  every rule here applies to it too. Code both firmwares use lives in
  shared/ (build with `--shared shared`). Keep ERISIAN building.

## Workflow
1. Edit → push to a branch → CI builds the .fwsc and runs host tests.
2. Tell Charles which artifact to download and what to test on the device.
3. Charles reports results; record them in docs/HARDWARE.md.

## Notes
- The JieLi package server is not reachable from the Claude Code sandbox
  (403 via proxy); firmware builds happen only in Actions.
- `tools/build.py --product` sets the update identity (`FM-1_8` + 6 digits,
  default `FM-1_8000014`). Changing it is Phase 4 — confirm what the stock
  updater accepts before picking one.
