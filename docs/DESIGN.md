# ERISIAN — design

Status: agreed design from the brainstorming session (2026-11-17). Nothing
here has run on hardware. Numbers marked *(est.)* are estimates to be replaced
by measurements from Charles's unit (Phase 3 onward).

---

## 1. Platform limits that shape the design

| Resource | Value | Source |
|---|---|---|
| Keys | 27 keys F3–G5 = 16 white + 11 black | `include/fm1.h` |
| Buttons | 14: OCT−, OCT+, and 12 labelled buttons (see §6) | panel |
| Encoders | 7, plus 1 knob | `include/fm1.h` |
| LEDs | every button and key | `include/fm1.h` |
| Screen | 240×240 RGB565, DMA bands of 240×16 px (no full framebuffer) | `include/fm1.h` |
| Audio | 44.1 kHz stereo, **128-frame blocks** (~2.9 ms), Q23 in int32 | `include/fm1_block.h` |
| CPU | 1× pi32v2 core used, ~320 MHz *(unverified)* → ~7 000 cycles/sample | public spec |
| RAM | ~512 KB internal SRAM shared with the runtime; ~150–250 KB free for the app *(est.)* | `platform/link.ld` |
| Firmware flash | ~460 KB for runtime + binary engine + our code + embedded data | `tools/package.py` |
| User flash | 524 KB (presets) | `tools/package.py` |
| Settings flash (VM) | ≥ 32 KB | `tools/package.py` |
| Codec | SEA decoder in the runtime (320-frame chunks) | `include/fm1_sea.h` |
| MIDI | USB (device only *(est.)*), BLE incl. central role, DIN/TRS if present | `include/fm1.h` |

Rules that follow:
- All audio code runs from RAM (`FM1_AUDIO_CODE`). Flash writes silence audio,
  so nothing streams to flash while playing.
- Long audio recording is impossible → the looper records notes.
- Delay/reverb buffers run at reduced rate and mono.
- Per-voice effects are cheap inserts + sends to shared effects.

---

## 2. Priority tiers

1. **Core** — rescue check at boot, text boot screen (name + version), drum
   sequencer with all step features, 11 voices with choke/mute groups, FM drum,
   Karplus-Strong drum, wav kick, presets with torn-write-safe storage.
2. **Sound depth** — shaker, klunk-bonk-crunch, hats/cymbal (+ reverse), noise
   riser/fall/impact, lo-fi & transient layer, per-voice filter/drive/sends,
   tape delay, plate reverb, 1176 bus comp, tape bus.
3. **Keys** — 808 bass, sine lead, koto, flute, vibraphone, resonator pad,
   3 layers, MIDI-out layer, arpeggiator, note looper.
4. **Character** — glitch FX, note repeat, ERIS randomize, Kallisti morph,
   Markov names.
5. **Stretch** — golden apple intro, git-out granular synth, BLE MIDI,
   MIDI clock sync, 4-op FM upgrade, richer physical models.

---

## 3. Drum side

### Voices
- 11 voices, selected with the 11 black keys.
- Each voice = **one engine** (Machinedrum-style, any engine on any voice)
  **+ lo-fi/transient layer**.
- **4 choke groups** (or none). Default: closed hat, open hat and reverse
  cymbal choke each other. Set on the voice's Amp page.
- Mute mode (SHIFT+ENV) for live muting.

### Engines
| Engine | Scope |
|---|---|
| FM drum | 2-op (carrier + modulator, pitch/amp envs, feedback) + filtered noise. Kick/snare/tom/hat/clank. Fallback if flash runs out: drive the built-in M-VAVE FM engine. |
| Karplus-Strong | Plucked toms/basses, noise-excited snare/clap, metallic/bell (stretched tuning + allpass dispersion), selectable exciter (noise, click, wav kick, mallet). |
| Shaker | Particle model: bead count, shell resonance, shake energy. |
| Klunk-bonk-crunch | Struck objects (modal: wood/plastic/tin/bottle, mallet hardness, damping), crunch textures (dense granular noise), rattle/buzz add-on. |
| Hat/cymbal | Modal inharmonic partials, closed/open decay, tightness. Reverse mode with swell synced to land on the next beat/bar. |
| Wav kick | Charles's own sample, ~30 KB, 16-bit mono 44.1 kHz (~0.34 s), **uncompressed**. Stored in user flash (uploaded once with tools/uploader), loaded into RAM at boot, played from RAM. |
| Noise | Tempo-synced riser/fall (1/2/4 bars) with filter sweep and pitch; impact/boom (thump + sub drop + tail). |

### Lo-fi & transient layer (per voice)
Transient generator (click/snap/thump), vinyl/tape texture (crackle, hiss,
wow/flutter), bitcrush + sample-rate reduction.

---

## 4. Sequencer

- Master pattern length **1–64 steps** (4 pages × 16). Each voice has its own
  length inside it; all voices reset when the master length ends.
- Per-voice speed: 2×, 1×, ¾×, ½×, ¼×. Master length counts 1/16 steps.
- Per step: trig, velocity, probability, ratchet, micro-timing, parameter
  locks (pool-capped per pattern).
- Swing per voice.
- Live chains of patterns (SHIFT+SEL), saved with the project.
- Note repeat: hold ARP + pad/step in drum mode.
- Live pads mode (ENV): black keys play voices.

---

## 5. Keys side

- **3 layers**, each stacked or assigned to a key zone, sharing a pool of
  ~6–8 voices with voice stealing *(tuned on hardware)*.
- Any layer can be **External**: sends notes on a chosen MIDI channel (USB out
  first). Only live keys (and arp) go out; looper and sequencer stay internal.
- Engines:
  | Engine | Scope |
  |---|---|
  | Koto | KS string, pluck position, damping, body. Cheap first. |
  | Flute | Waveguide, breath noise, jet, vibrato. Cheap first. |
  | Vibraphone | Resonator bank, motor tremolo, damper. Cheap first. |
  | 808 bass | Glide/legato, pitch envelope, drive stages (soft clip → fold → LP), sub/click layer. |
  | Sine lead | 2-op FM, mono/poly, glide, vibrato (shares FM drum code). |
  | Pad | Resonator bank excited by noise/keys (shares code with vibraphone and metallic KS). |
  | Git-out granular | **Stretch.** Resampled to 22 kHz, SEA ~3 bit (~25–40 KB flash), decoded to RAM at boot as 8-bit (~37 KB). |
- **Arpeggiator** (ARP): up, down, up/down, random, as-played, chord.
  Tempo-synced.
- OCT−/OCT+ change octave.

### Note looper (REC)
- Records notes, not audio (~4 bytes/note).
- **4 loop slots**; each can be muted, cleared, and be synced or drone.
- Synced: REC arms → starts on next bar or first note → REC again rounds the
  length to 1/2/4/8 bars.
- **Overdub**: REC on a playing loop; SHIFT+LFO undoes the last pass.
- **Drone**: SHIFT+REC records a free-length loop that ignores the sequencer
  and keeps playing when it stops.
- Loops are **saved with the pattern**. Cap ~1 000 notes per pattern total.

---

## 6. Effects

Signal flow: `voice inserts → sends (delay, reverb) → drum bus → master`
(keys layers join at the master, with their own sends).

| Stage | Contents |
|---|---|
| Per drum voice / keys layer | SVF filter (HP/LP/BP) with envelope + pre-drive, drive, lo-fi layer (drums), send levels |
| Tape delay (send) | Tempo sync, wow/flutter + degrade per repeat, freeze, self-oscillation (with limiter). Mono, reduced rate. |
| Plate reverb (send) | Size, damping, pre-delay. Half rate, mono, stereo spread on output. |
| Drum bus | 1176-style comp with all-buttons mode; tape block (saturation, soft comp, hiss) |
| Master | Gentle tape block, compressor/limiter. Sidechain duck (any voice ducks keys and/or reverb). |
| Glitch (FX bus) | Stutter/retrigger with slide, pitch/tape stop, granular smear over the capture buffer (~1–2 s). |

Tape blocks have an **age** macro linking saturation, hiss and high-cut.

---

## 7. Controls

HOME tap = home screen, **hold = SHIFT**. The screen shows a legend of what
each button does in the current mode.

| Button | Drum mode | Keys mode | SHIFT + |
|---|---|---|---|
| OCT− / OCT+ | Pattern page 1–4 | Octave | Copy / Paste |
| FX | Cycle effect buses | same | — |
| SEL | + white key = pattern | Layer 1/2/3 | Chain / layer stack-split setup |
| ENV | Live pads on/off | Envelope page | Mute mode |
| LFO | ERIS randomize | LFO page | Undo |
| EDIT | Next parameter page | same | Previous page |
| GLO | Tap tempo / hold + encoder = BPM | same | Swing, global settings, MIDI |
| HOME | Home / hold = SHIFT | same | — |
| SAVE | Preset library | same | — |
| ARP | Note repeat | Arpeggiator | Kallisti on/off |
| SEQ | Drum ↔ keys | same | — |
| PLAY/STOP | Sequencer start/stop | same | — |
| REC | Note looper | same | Drone loop |

Chords:
- SEL + white key → select pattern.
- SEL + black voice key + white key → that voice's length.
- SEL + FX + white key → master length.
- While holding a chord, OCT−/OCT+ reach pages 2–4.
- Hold step key + turn encoder → parameter lock.
- SHIFT + step key → clear step; SHIFT + voice key → clear voice's track.
- **OCT− held at power-on → rescue mode**, checked before anything else.

Encoders: pages of 7 parameters, each lined up with an encoder; EDIT steps
through pages (Engine / Filter / Amp / Sends / Lo-fi …).

---

### Panel bit map (from fm1-emu's FM-1 panel model; to verify on hardware)
`fm1_controls.buttons` bits: 0 OCT−, 1 OCT+, 2 FX, 3 SCL ("SEL"), 4 ENV,
5 LFO, 6 EDIT, 7 GLO, 8 HOME, 9 SAVE, 10 ARP, 11 SEQ, 12 PLAY, 13 REC.
`keys` bits 0..26 = F3..G5. Encoders 0 SELECT, 1 ALGO, 2–5 K1–K4, 6 PRESET.
LEDs: 0..13 buttons, 14..40 keys.

## 8. Screens

Clean minimal, dark background, **per-mode accent**: gold in drum mode, cyan
in keys mode.

- **Drum:** all-voices grid (11 rows × 16 steps of the current page),
  selected voice highlighted, steps past the length dimmed, each voice row
  shows its own playhead. Header: page indicator, BPM, kit, transport.
  Footer: 7-parameter strip.
- **Keys:** layer stack (engine, zone/stack, level, MIDI channel), keyboard
  zone map, looper status, 7-parameter strip.
- **FX:** signal-flow strip with the current bus marked, bus meters,
  7-parameter strip.
- **Boot:** plain text name + version (~0.5 s). Golden apple intro is stretch.

---

## 9. Character features

- **ERIS randomize** (LFO in drum mode): sound params, pattern steps, engine
  choice (high amounts), keyboard patches. Amount control, one-level undo
  (~5 KB RAM).
- **Kallisti morph** (SHIFT+ARP, both modes): one encoder blends A↔B
  (kits in drum mode, keyboard patches in keys mode). Continuous params
  interpolate; discrete params (engine, choke group, sample) switch at 50 %.
  Capture B from the Kallisti screen.
- **Markov names**: order-2 character Markov chain trained on an original
  Discordian word list (Eris, Kallisti, fnord, Pentabarf, Greyface, Chao,
  Hodge, Podge, Malaclypse, Discordia, apple, hotdog …), occasional 5/23
  suffix. Auto-named on create; one encoder rerolls. Manual editing one
  character at a time (cursor encoder + character encoder). Max 12 chars.

---

## 10. Presets and storage

- Saving a pattern stores a **full copy of its kit** and its 4 loops. Load
  options: whole pattern, kit only, or notes only onto the current kit.
- Kits and keyboard patches also save independently.
- Slots (balanced): **32 patterns, 32 kits, 32 keyboard patches, 8 chains.**
- **Log-structured store** in the 524 KB user partition: every save writes a
  new record (sequence number + CRC) into free space; the old copy is released
  only after the new one verifies; on boot the newest valid record wins.
  Shared spare area ~32 KB. Spreads wear. SAVE screen shows free space.
  Budget *(est.)*: 32 × 12 KB patterns + kits + patches + chains + spare
  ≈ 470 KB.
- Host tests: power cut at every write offset, bit flips, full flash, wrap.
- Flash JEDEC ID must be `0x856014` or `0xB36014`; otherwise saves are refused
  with a message.

---

## 11. Global settings (GLO, SHIFT+GLO)

- Tempo, swing.
- MIDI out: port (USB now, BLE later), channel per external layer.
- CPU/RAM meter on/off.
- Dual core: **off by default**; opt-in with automatic fallback to one core if
  core 1 faults or stalls.

---

## 12. Budgets *(est.; replace with measurements)*

| RAM | KB |
|---|---|
| Wav kick (whole sample) | ~30 |
| Tape delay (mono, reduced rate) | ~30–40 |
| Plate reverb (half rate) | ~30–45 |
| Glitch capture buffer | ~44–88 |
| Note loops pool | ~8 |
| ERIS undo + Kallisti A/B | ~7 |
| Voices, UI, display bands | TBD |
| Git-out (stretch) | 37 |

| Firmware flash | KB |
|---|---|
| **Limit**: image area must end ≥ 32 KB before the BT/user area (0x73000) | **460** |
| Runtime + M-VAVE engine + fm1-core platform + sine, incl. RAM sections (CI run 10, 2026-11-17) | **376** (`app.bin` 366 300 B; image area 0x5E000) |
| **Left for everything ERISIAN adds** | **~84** |
| Wav kick | 0 (lives in user flash, not the firmware) |

Of the image, 78.5 KB is `.ram0_data` (code and data copied to SRAM at boot),
so it also uses that much RAM.

Flash is tighter than RAM probably is. Keep engines small (shared
resonator/KS/FM cores), avoid large tables, and track image size in CI.

---

## 13. Open questions

- Kick: decided 2026-11-17 — user partition, uploaded once over USB MIDI.
- Free RAM and CPU per voice (Phase 3 on hardware).
- Whether flash reads from the audio interrupt are safe (decides wav kick
  playback).
- Whether the FM-1 can be a USB MIDI host (assumed no) and whether it has a
  DIN/TRS MIDI jack.
- Whether the keys report velocity.
- Whether the binary SDK accepts flash ID `0xB36014`.
- MIDI clock sync (stretch).
