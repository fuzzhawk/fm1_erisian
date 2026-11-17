#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Summarise an emulator run for CI logs: audio level of a WAV, and a coarse
text rendering of a 24-bit BMP screen snapshot."""
import struct
import sys
import wave
from pathlib import Path


def audio(path: Path) -> tuple[int, float]:
    with wave.open(str(path)) as w:
        frames = w.readframes(w.getnframes())
        width, channels, rate = w.getsampwidth(), w.getnchannels(), w.getframerate()
    if width != 2 or not frames:
        print(f"audio: {path.name}: {len(frames)} bytes, sample width {width}")
        return 0, 0.0
    samples = struct.unpack(f"<{len(frames) // 2}h", frames)
    peak = max(abs(s) for s in samples)
    rms = (sum(s * s for s in samples) / len(samples)) ** 0.5
    # Rough pitch: count upward swings of the left channel through a band of
    # +-peak/4 (hysteresis), so low-level noise near zero adds no crossings.
    left = samples[::channels]
    band = max(peak // 4, 1)
    state, cycles = 0, 0
    for v in left:
        if state <= 0 and v > band:
            if state < 0:
                cycles += 1
            state = 1
        elif state >= 0 and v < -band:
            state = -1
    seconds = len(left) / rate
    hz = cycles / seconds
    print(f"audio: {seconds:.2f} s, peak {peak}, rms {rms:.0f}, "
          f"~{hz:.0f} Hz")
    return peak, hz


def screen(path: Path, cols: int = 60, rows: int = 30) -> None:
    data = path.read_bytes()
    offset, = struct.unpack_from("<I", data, 10)
    width, height = struct.unpack_from("<ii", data, 18)
    bpp, = struct.unpack_from("<H", data, 28)
    if bpp != 24:
        print(f"screen: {path.name}: {bpp} bpp not supported")
        return
    stride = (width * 3 + 3) & ~3
    shades = " .:-=+*#%@"
    lit = 0
    print(f"screen: {path.name} {width}x{abs(height)}")
    for row in range(rows):
        line = ""
        for col in range(cols):
            x = col * width // cols
            y = row * abs(height) // rows
            y = abs(height) - 1 - y if height > 0 else y
            b, g, r = data[offset + y * stride + x * 3: offset + y * stride + x * 3 + 3]
            level = (r * 3 + g * 6 + b) // 10
            lit += level > 16
            line += shades[min(level * len(shades) // 256, len(shades) - 1)]
        print("  |" + line + "|")
    print(f"screen: {lit} of {cols * rows} sampled points lit")


# --expect-tone HZ: fail unless every WAV holds a tone near HZ (+-10 %).
arguments = sys.argv[1:]
expect = None
expect_sound = False
if arguments[:1] == ["--expect-tone"]:
    expect = float(arguments[1])
    arguments = arguments[2:]
elif arguments[:1] == ["--expect-sound"]:
    expect_sound = True
    arguments = arguments[1:]
status = 0
for name in arguments:
    p = Path(name)
    if not p.exists():
        print(f"{name}: missing")
        status = 1
    elif p.suffix == ".wav":
        peak, hz = audio(p)
        if expect and (peak == 0 or abs(hz - expect) > expect * 0.1):
            print(f"audio: expected a tone near {expect:.0f} Hz")
            status = 1
        if expect_sound and peak < 1000:
            print("audio: expected sound (peak >= 1000)")
            status = 1
    elif p.suffix == ".bmp":
        screen(p)
sys.exit(status)
