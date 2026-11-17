# Changelog

## Unreleased
- Repository groundwork: imported fm1-core with history, CI build of the
  unmodified sine example, toolchain fetch script, host test harness, docs.
- First green CI build of the sine example (`fm1.fwsc`, image 287 808 B).
- Fixed `tools/build.py`: `app.bin` now includes `.data`, `.ram0_data` and
  `.cache_ram_data` after `.text`; the sine build boots in the emulator.
- CI boots every package in fm1-emu: fails on a halt, checks a 440 Hz tone
  with a key held, reports resets and a text render of the screen.
- Discordian Markov name generator.
- Log-structured preset store (`app/storage/`) with power-cut, bit-flip,
  full-flash and wear host tests.
