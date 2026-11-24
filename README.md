# ERISIAN

Custom firmware for the M-VAVE FM-1 pocket synth (JieLi AC791N / WL82, PI32v2).

ERISIAN is built on [fm1-core](https://github.com/rgrvmusic/fm1-core) by
rgrvmusic, an MIT-licensed FM-1 hardware platform derived from M-VAVE's
FM1-open. fm1-core's git history is preserved in this repository. Its original
README is kept at [docs/FM1_CORE_README.md](docs/FM1_CORE_README.md).

This repository builds two firmwares: **ERISIAN** (drum machine and keys,
`app/`, [docs/DESIGN.md](docs/DESIGN.md); on hold) and **MAVIS** (a dedicated
three-engine keyboard synth, `mavis/`, [docs/MAVIS.md](docs/MAVIS.md)). Code
both use is in `shared/`.

> **Status:** see [docs/HARDWARE.md](docs/HARDWARE.md) for what has been
> verified on hardware. Read
> [docs/RECOVERY.md](docs/RECOVERY.md) and take a full flash backup before
> flashing anything.

## How it works

1. Source lives here; Claude Code edits it.
2. GitHub Actions (`.github/workflows/build.yml`) fetches the JieLi toolchain,
   builds the firmware, packages it as `.fwsc`, runs host tests and uploads
   the artifacts.
3. You download the `.fwsc` from the Actions run and flash it yourself over USB.

## Building locally

Needs Linux x86-64 (or Docker `linux/amd64`), Python ≥ 3.10 and the JieLi
PI32v2 toolchain 2.5.3 (clang 4.0.1, `-mcpu=r3`).

```sh
sh tools/fetch_toolchain.sh             # into ~/.jieli, never into the repo
export FM1_PI32_TOOLCHAIN=/path/containing/bin/clang
python3 tools/build.py --clean --package # build/fm1.fwsc
sh tests/run_tests.sh                    # host tests, no toolchain needed
```

## Layout

- `include/`, `platform/`, `sdk/`, `examples/`, `tools/build.py`,
  `tools/package.py`: from fm1-core.
- `tools/fetch_toolchain.sh`, `tools/toolchain.sha256`: toolchain download with pinned hash.
- `tests/`: host tests (run in CI).
- `docs/`: hardware log, recovery, changelog.
- `app/`: ERISIAN's firmware. `mavis/`: MAVIS's firmware.
- `shared/`: storage and drawing code used by both (`tools/build.py --shared shared`).

## License

ERISIAN's own code and fm1-core's `platform/`, `include/`, `tools/` and
`examples/` are MIT ([LICENSE](LICENSE)). The binary SDK and boot resources in
`sdk/` keep their own terms (M-VAVE engine permission, Apache-2.0 boot/update
resources, newlib, compiler-rt, …), listed in [THIRD_PARTY.md](THIRD_PARTY.md).
`.fwsc` packages contain Apache-2.0 JieLi/M-VAVE resources, so releases ship
[LICENSES/Apache-2.0.txt](LICENSES/Apache-2.0.txt) and `THIRD_PARTY.md`.
The JieLi toolchain is never committed or redistributed.
