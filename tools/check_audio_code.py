#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Fail if ERISIAN's audio-interrupt functions live in flash or reference it.

The FM-1 programs its flash while audio runs, so every function the audio
interrupt reaches must be in SRAM (FM1_AUDIO_CODE) and must not call or read
anything in flash: compiler helpers (64-bit division), libc (memset, memcpy)
or const tables included.
Usage: check_audio_code.py OBJDUMP ELF
"""
import re
import subprocess
import sys

AUDIO = re.compile(r"^(fm1_app_render|fm1_output_mix|drums_render|drums_trigger|render_voice|"
                   r"sine|decay|mix_master|seq_render|on_pulse|next_random|seq_speed_period|"
                   r"seq_voice_step|seq_stop|engine_sample|transient|svf2x|soft15|rnd|white|clamp15|"
                   r"micro_pulses|swing_pulses|passes|start_hits|"
                   r"synth_note|synth_render|synth_all_off|synth_voice_render|synth_engine_sample|"
                   r"isqrt32|note_inc|resonator_set|resonate|engine_start|alloc_voice|voice_note|"
                   r"layer_note|find_layer_voice|fold15|vibrato|"
                   r"mv_note|mv_render|mv_all_off|koto_block|koto_render|voice_start|"
                   r"pitch_period|sine1024|xorshift|clamp16|ticks_since|master_clip)$")
RAM_LO, RAM_HI = 0x01C00000, 0x01C80000
FLASH_LO, FLASH_HI = 0x02000000, 0x02100000

objdump, elf = sys.argv[1], sys.argv[2]
run = lambda *a: subprocess.run([objdump, *a, elf], capture_output=True, text=True).stdout

# 1. Where each audio function is (symbol table). Static helpers shared
# through a header (dsp.h) exist once per file: check every copy.
symbols = {}
for line in run("-t").splitlines():
    parts = line.split()
    if len(parts) >= 2 and re.fullmatch(r"[0-9a-f]{8,16}", parts[0]):
        symbols.setdefault(parts[-1], set()).add(int(parts[0], 16))
audio = {n: a for n, a in symbols.items() if AUDIO.match(n)}

problems = []
for name, addrs in sorted(audio.items()):
    for addr in sorted(addrs):
        if not RAM_LO <= addr < RAM_HI:
            problems.append(f"{name} is at 0x{addr:08x}, not in SRAM (missing FM1_AUDIO_CODE?)")

# 2. What each audio function references: disassemble the RAM code section
# (it is a data section to the linker, so -D, not -d).
text = run("-D", "-j", ".ram0_data")
# This objdump prints a function as a bare "name:" line, and annotates
# references as "<symbol[+0xoff] : hexaddress >".
header = re.compile(r"^([A-Za-z_.$][\w.$]*):\s*$")
ref = re.compile(r"<([^<>:]+?)\s*:\s*([0-9a-f]+)\s*>")
current, seen = None, set()
for line in text.splitlines():
    m = header.match(line)
    if m:
        current = m.group(1) if AUDIO.match(m.group(1)) else None
        if current: seen.add(current)
        continue
    if not current:
        continue
    for name, a_addr in ref.findall(line):
        addr = int(a_addr, 16)
        if FLASH_LO <= addr < FLASH_HI:
            problems.append(f"{current} references flash ({name} at 0x{addr:08x}): {line.strip()}")

print(f"audio functions in the symbol table: "
      + ", ".join(f"{n}@" + "/".join(f"0x{a:08x}" for a in sorted(v))
                  for n, v in sorted(audio.items())) or "none")
print(f"audio functions disassembled: {', '.join(sorted(seen)) or 'none'}")
if not audio or not seen:
    print("objdump -D sample (first function headers):")
    print("\n".join([l for l in text.splitlines() if l.strip()][:25]))
for p in problems:
    print("ERROR:", p)
sys.exit(1 if problems or not audio or not seen else 0)
