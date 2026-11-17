#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Print flash and RAM use of an FM-1 build from its linker map."""
import re
import sys
from pathlib import Path

RAM0_END = 0x1C7FE00 - 52 - 128          # platform/link.ld RAM0_SIZE end
IMAGE_LIMIT = 503808 - 32768             # package.py: VM end - minimum VM


def main(map_path: str, image_path: str) -> None:
    text = Path(map_path).read_text(errors="replace")
    sections = {}
    for m in re.finditer(r"^(\.[\w.]+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s*$", text, re.M):
        sections.setdefault(m.group(1), (int(m.group(2), 16), int(m.group(3), 16)))
    symbols = {}
    for m in re.finditer(r"^\s+0x([0-9a-f]+)\s+(\w+) = ", text, re.M):
        symbols.setdefault(m.group(2), int(m.group(1), 16))

    image = Path(image_path).stat().st_size
    area = (0x4000 + 0x120 + image + 383 + 64 + 4095) // 4096 * 4096
    print(f"flash: app.bin {image} B; image area {area} B of {IMAGE_LIMIT} B; "
          f"{(IMAGE_LIMIT - area) // 1024} KB left")
    for name in (".text", ".ram0_data", ".ram0_bss", ".cache_ram_data", ".cache_ram_bss"):
        if name in sections:
            addr, size = sections[name]
            print(f"  {name:16} at 0x{addr:08x}  {size:7} B ({size / 1024:.1f} KB)")
    begin = symbols.get("_HEAP_BEGIN")
    end = symbols.get("_HEAP_END")
    if begin and end:
        print(f"ram0: static data/code/bss end at 0x{begin:08x}; "
              f"runtime heap {end - begin} B ({(end - begin) / 1024:.1f} KB) "
              f"up to 0x{end:08x}")
        print("      every static buffer ERISIAN adds comes out of that heap "
              "(FreeRTOS task stacks and runtime allocations live there).")


if __name__ == "__main__":
    main(*sys.argv[1:3])
