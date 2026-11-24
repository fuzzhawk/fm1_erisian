#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build an FM-1 firmware image with the JieLi PI32v2 toolchain.

The platform layer in platform/ and the application passed with --app are
compiled, linked against the fixed runtime in sdk/lib/libfm1_engine.a, and
optionally packaged as a .fwsc update image.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import shutil
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build"
PRODUCT_RE = re.compile(r"FM-1_8[0-9]{6}")
ENTRY_POINT = 0x02000120
USER_BYTES = 536576
UPDATE_FAMILY = 7

COMMON_FLAGS = [
    "-target", "pi32v2",
    "-integrated-as",
    "-mcpu=r3",
    "-mfprev1",
    "-Wuninitialized",
    "-Wno-invalid-noreturn",
    "-fno-common",
    "-fallow-pointer-null",
    "-fprefer-gnu-section",
    "-femulated-tls",
    "-Wno-shift-negative-value",
    "-Wframe-larger-than=2560",
    "-fms-extensions",
    "-fno-unwind-tables",
    "-ffunction-sections",
    "-fdata-sections",
    "-fmessage-length=0",
    "-cl-single-precision-constant",
    "-mllvm", "-pi32v2-large-program=true",
    "-D__GCC_PI32V2__",
    "-O2",
    "-fwrapv",
    "-fno-fast-math",
    "-ffp-contract=off",
]

# The scanner and the display driver address registers directly and run from
# interrupt/control context; keep them out of the PIC register machinery.
EXTRA_FLAGS = {
    "platform/panel.c": ["-fno-pic"],
    "platform/display.c": ["-fno-pic"],
}

LD_UNDEFINED = [
    "connectivity_wl82_bluetooth_start",
    "audio_app_bluetooth_open",
    "bt_get_local_name",
    "bt_get_mac_addr",
]


class BuildError(RuntimeError):
    pass


def run(command: list[str], *, capture: bool = False) -> subprocess.CompletedProcess:
    printable = " ".join(str(part) for part in command)
    if not capture:
        print(printable)
    result = subprocess.run(command, capture_output=capture, text=True)
    if result.returncode != 0:
        if capture:
            sys.stderr.write(result.stdout)
            sys.stderr.write(result.stderr)
        raise BuildError(f"command failed ({result.returncode}): {command[0]}")
    return result


def _search_roots(explicit: str | None) -> list[Path]:
    roots: list[Path] = []
    for value in (explicit, os.environ.get("FM1_PI32_TOOLCHAIN"),
                  os.environ.get("JIELI_TOOLCHAIN")):
        if value:
            roots.append(Path(value).expanduser())
    roots += [Path.home() / ".jieli" / "toolchain", Path.home() / ".jieli"]
    return roots


def _find_tool(names: tuple[str, ...], roots: list[Path],
               override: str) -> Path | None:
    value = os.environ.get(override)
    if value:
        path = Path(value).expanduser()
        return path if path.is_file() else None
    for root in roots:
        if not root.is_dir():
            continue
        # Prefer the PI32v2-specific directory over shared ones.
        candidates = [root / "bin", root / "pi32v2" / "bin", root]
        candidates += sorted(root.glob("*/pi32v2/bin"))
        candidates += sorted(root.glob("*/bin")) + sorted(root.glob("*/*/bin"))
        for directory in candidates:
            for name in names:
                if (directory / name).is_file():
                    return directory / name
    return None


def find_toolchain(explicit: str | None) -> tuple[Path, Path, Path]:
    """Return (clang, ld, objcopy). The JieLi archive may keep clang and the
    PI32v2 binutils in different bin directories, so each is found on its own.
    FM1_PI32_CLANG / FM1_PI32_LD / FM1_PI32_OBJCOPY override the search."""
    roots = _search_roots(explicit)
    clang = _find_tool(("clang",), roots, "FM1_PI32_CLANG")
    linker = _find_tool(("ld", "pi32v2-ld", "lto-wrapper-ld"), roots,
                        "FM1_PI32_LD")
    objcopy = _find_tool(("objcopy", "pi32v2-objcopy", "llvm-objcopy"),
                         roots, "FM1_PI32_OBJCOPY")
    missing = [name for name, path in
               (("clang", clang), ("ld", linker), ("objcopy", objcopy))
               if path is None]
    if missing:
        raise BuildError(
            "cannot find the JieLi PI32v2 toolchain (" + ", ".join(missing)
            + "); set FM1_PI32_TOOLCHAIN to its install directory or "
            "FM1_PI32_CLANG / FM1_PI32_LD / FM1_PI32_OBJCOPY")
    return clang, linker, objcopy


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 16), b""):
            digest.update(chunk)
    return digest.hexdigest()


def collect_sources(app_directory: Path, shared: list[Path]) -> list[Path]:
    sources = sorted((ROOT / "platform").glob("*.c"))
    # The whole application tree (engine, storage and UI code live in
    # subdirectories), plus any shared trees (code used by several firmwares).
    for directory in [*shared, app_directory]:
        sources += sorted(directory.rglob("*.c"))
        sources += sorted(directory.rglob("*.S"))
    return sources


def object_name(source: Path) -> str:
    relative = source.relative_to(ROOT) if source.is_relative_to(ROOT) else source
    return re.sub(r"[^A-Za-z0-9._-]", "_", str(relative)) + ".o"


def identity_source(app_directory: Path, product: str,
                    shared: list[Path] = ()) -> tuple[str, str]:
    entries: list[str] = []
    directories = [ROOT / "platform", ROOT / "include", *shared, app_directory]
    for directory in directories:
        if not directory.is_dir():
            continue
        for path in sorted(p for p in directory.rglob("*") if p.is_file()):
            relative = path.relative_to(ROOT) if path.is_relative_to(ROOT) else path
            entries.append(f"{relative}:{sha256(path)}")
    identity = hashlib.sha256("\n".join(entries).encode()).hexdigest()
    raw = hashlib.sha256("\n".join(entries).encode()).digest()

    byte_list = ", ".join(f"0x{byte:02x}" for byte in raw)
    text = (
        '#include "fm1_layout.h"\n'
        f"const struct fm1_firmware_layout fm1_layout = "
        f"{{{USER_BYTES}U, {UPDATE_FAMILY}U}};\n"
        f"const unsigned char fm1_build_id[32] = {{{byte_list}}};\n"
        f'const unsigned char fm1_update_identity[27] = "{product}";\n'
    )
    return identity, text


def check_entry(elf: Path) -> None:
    data = elf.read_bytes()
    if len(data) < 52 or data[:4] != b"\x7fELF":
        raise BuildError("linked output is not an ELF file")
    entry = struct.unpack_from("<I", data, 24)[0]
    if entry != ENTRY_POINT:
        raise BuildError(f"entry point is 0x{entry:08x}, expected 0x{ENTRY_POINT:08x}")


# Sections the startup code copies from flash into SRAM. platform/link.ld
# defines their load addresses as consecutive offsets after .text
# (data_lma = text + |text|, _ram0_data_lma = data_lma + |data|,
# cache_ram_data_lma = _ram0_data_lma + |ram0_data|), so app.bin must be these
# sections' contents back to back, in this order. Extracting .text alone leaves
# the RAM code and initialised data out of the image: the startup then copies
# erased flash into SRAM and jumps into it.
IMAGE_SECTIONS = (".text", ".data", ".ram0_data", ".cache_ram_data")


def write_image(objcopy: Path, elf: Path, output: Path, image: Path) -> None:
    parts = output / "sections"
    parts.mkdir(parents=True, exist_ok=True)
    blob = bytearray()
    for section in IMAGE_SECTIONS:
        part = parts / (section.strip(".") + ".bin")
        part.unlink(missing_ok=True)
        run([str(objcopy), "-O", "binary", "-j", section, str(elf), str(part)])
        data = part.read_bytes() if part.exists() else b""
        print(f"section {section:16} {len(data):8} bytes")
        blob += data
    if not blob:
        raise BuildError("empty firmware image")
    image.write_bytes(bytes(blob))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", type=Path, default=ROOT / "examples",
                        help="directory with application sources")
    parser.add_argument("--shared", type=Path, action="append", default=[],
                        help="extra source tree shared between firmwares (repeatable)")
    parser.add_argument("--toolchain", default=None)
    parser.add_argument("--output", type=Path, default=BUILD)
    parser.add_argument("--product", default="FM-1_8000014",
                        help="update identity, FM-1_8 followed by six digits")
    parser.add_argument("--package", action="store_true",
                        help="also create a .fwsc update image")
    parser.add_argument("--clean", action="store_true")
    parser.add_argument("--version-string", default=None,
                        help="defines FW_VERSION (and ERISIAN_VERSION) for the application")
    arguments = parser.parse_args()

    try:
        if not PRODUCT_RE.fullmatch(arguments.product):
            raise BuildError("invalid --product value")
        output = arguments.output.resolve()
        if arguments.clean and output.exists():
            shutil.rmtree(output)

        app = arguments.app.resolve()
        shared = [directory.resolve() for directory in arguments.shared]
        for directory in shared:
            if not directory.is_dir():
                raise BuildError(f"no shared directory {directory}")
        if not any(app.glob("*.c")):
            raise BuildError(f"no C sources in application directory {app}")

        clang, linker, objcopy = find_toolchain(arguments.toolchain)
        print(f"toolchain: clang={clang} ld={linker} objcopy={objcopy}")

        objects_directory = output / "obj"
        generated = output / "generated"
        objects_directory.mkdir(parents=True, exist_ok=True)
        generated.mkdir(parents=True, exist_ok=True)

        includes = [
            ROOT / "include",
            ROOT / "sdk" / "include",
            ROOT / "sdk" / "include" / "libc",
            ROOT / "platform",
            *shared,
            app,
        ]
        include_flags = [f"-I{directory}" for directory in includes]

        version_flags = []
        if arguments.version_string:
            if not re.fullmatch(r"[A-Za-z0-9._+-]{1,24}", arguments.version_string):
                raise BuildError("invalid --version-string")
            version_flags = [f'-DERISIAN_VERSION="{arguments.version_string}"',
                             f'-DFW_VERSION="{arguments.version_string}"']

        sources = collect_sources(app, shared)
        objects: list[Path] = []
        for source in sources:
            relative = source.relative_to(ROOT) if source.is_relative_to(ROOT) \
                else source.name
            flags = COMMON_FLAGS + include_flags + version_flags
            flags += EXTRA_FLAGS.get(str(relative), [])
            target = objects_directory / object_name(source)
            print(f"compile {relative}")
            run([str(clang), *flags, "-MMD", "-MF", f"{target}.d",
                 "-c", str(source), "-o", str(target)])
            objects.append(target)

        identity, source_text = identity_source(app, arguments.product, shared)
        identity_file = generated / "identity.c"
        identity_file.write_text(source_text)
        identity_object = objects_directory / "identity.o"
        print("compile generated/identity.c")
        run([str(clang), *COMMON_FLAGS, *include_flags, "-c",
             str(identity_file), "-o", str(identity_object)])
        objects.append(identity_object)

        elf = output / "sdk.elf"
        map_file = output / "sdk.map"
        link = [
            str(linker), "-o", str(elf), "--gc-sections",
            "-T", str(ROOT / "platform" / "link.ld"),
            *[str(path) for path in objects],
            "--start-group",
            "--whole-archive", str(ROOT / "sdk" / "lib" / "libfm1_engine.a"),
            "--no-whole-archive",
            str(ROOT / "sdk" / "lib" / "libc.a"),
            str(ROOT / "sdk" / "lib" / "libcompiler_rt.a"),
            "--end-group",
            "--sort-common",
            f"-M={map_file}",
            *[f"--undefined={name}" for name in LD_UNDEFINED],
        ]
        print(f"link {elf}")
        run(link)
        check_entry(elf)

        image = output / "app.bin"
        write_image(objcopy, elf, output, image)
        print(f"identity: {identity}")
        print(f"firmware: {elf}")
        print(f"image:    {image} ({image.stat().st_size} bytes)")

        if arguments.package:
            package = output / "fm1.fwsc"
            script = ROOT / "tools" / "package.py"
            run([sys.executable, "-I", str(script), str(image),
                 "--output", str(package), "--product", arguments.product])
            print(f"package:  {package}")
    except (BuildError, OSError) as error:
        print(f"build: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
