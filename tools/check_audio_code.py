#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Fail if ERISIAN's audio-interrupt functions live in flash or call into it.

The FM-1 programs its flash while audio runs, so every function the audio
interrupt reaches must be in SRAM (FM1_AUDIO_CODE) and must not call code in
flash: compiler helpers (64-bit division, memset/memcpy from libc) included.
Usage: check_audio_code.py OBJDUMP ELF
"""
import re
import subprocess
import sys

AUDIO = re.compile(r"^(fm1_app_render|fm1_output_mix|drums_render|drums_trigger|render_voice|"
                   r"sine|decay|mix_master|seq_render|on_pulse|next_random|seq_speed_period|"
                   r"seq_voice_step|erisian_audio_\w+)$")
RAM_LO, RAM_HI = 0x01C00000, 0x01C80000
FLASH_LO = 0x02000000

objdump, elf = sys.argv[1], sys.argv[2]
text = subprocess.run([objdump, "-d", elf], capture_output=True, text=True).stdout
# Function headers: "<hex address> <name>:"; references inside instructions
# are annotated by this objdump as "<symbol : hexaddress >".
header = re.compile(r"^([0-9a-f]+) <([^>]+)>:\s*$")
target = re.compile(r"<([^<>:]+?)\s*:\s*([0-9a-f]+)\s*>")

functions, current = {}, None
for line in text.splitlines():
    m = header.match(line)
    if m:
        current = m.group(2)
        functions[current] = {"addr": int(m.group(1), 16), "lines": []}
    elif current:
        functions[current]["lines"].append(line)

problems, checked = [], []
for name, f in functions.items():
    if not AUDIO.match(name):
        continue
    checked.append(name)
    if not RAM_LO <= f["addr"] < RAM_HI:
        problems.append(f"{name} is at 0x{f['addr']:08x}, not in SRAM (missing FM1_AUDIO_CODE?)")
    for line in f["lines"]:
        for sym, addr in target.findall(line):
            if int(addr, 16) >= FLASH_LO:
                problems.append(f"{name} references flash ({sym.strip()} at 0x{int(addr, 16):08x}): "
                                f"{line.strip()}")

print(f"audio functions checked: {', '.join(sorted(checked)) or 'none found'}")
if not checked:
    sample = [l for l in text.splitlines() if l.endswith(">:")][:5]
    print("objdump call syntax sample:", *sample, sep="\n  ")
for p in problems:
    print("ERROR:", p)
sys.exit(1 if problems or not checked else 0)
