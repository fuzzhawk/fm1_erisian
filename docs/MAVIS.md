# MAVIS — design

MAVIS is a second firmware for the M-VAVE FM-1, built from this repository
(`mavis/`, sharing `platform/`, `shared/` and the tools with ERISIAN). It is
a dedicated keyboard synth with three engines, spending as much of the FM-1
as possible on making them realistic and textural:

1. **KOTO**: a modelled plucked string (waveguide).
2. **MALLET**: a modal model that morphs continuously from marimba-style bars
   to a hammered-string "mallet piano".
3. **PAD**: an original additive engine in the spirit of the harmonic
   oscillator in Mutable Instruments' Plaits (written from scratch in fixed
   point, not ported).

Everything in `CLAUDE.md` applies to MAVIS as well (one core by default,
rescue key, torn-write-safe storage, no GPL code, both flash IDs, no
overclocking). ERISIAN is on hold; nothing here may break its build.

Decisions below were made with Charles on 2026-11-24.

---

## 1. Platform facts that shape MAVIS

| Fact | Consequence |
|---|---|
| Keys are on/off only (no velocity, no aftertouch) | Expression comes from knobs, envelopes, per-note humanize and morph |
| One core by default | Voice count and model detail are set by CPU measured on hardware |
| ~230 KB RAM free without ERISIAN's drum machine | Long delay lines, mode tables and a real reverb fit |
| Every flash write drops ≥1 audio block | Saves wait for silence (see §8) |
| Update identity still `FM-1_8000014` | Phase 4: ERISIAN and MAVIS must get distinct identities before release |

---

## 2. Playing model

- **One engine per patch.** A patch is KOTO, MALLET or PAD with all voices
  for that engine.
- **Adaptive quality.** Voices share a fixed CPU budget: with few notes each
  voice runs at full detail; as more notes sound, detail steps down
  gracefully (fewer pad partials, fewer mallet modes, sympathetic strings
  and sawari thinned). Built for MALLET (M2): a resonator budget (16..192,
  start 96) steered every block by the CPU load measured with the tick
  counter (shrinks above 55 %, grows below 35 %); each new note gets
  budget / sounding voices; above 80 % sounding voices drop their top modes.
  Maximum voices: 8 (the real number is set on hardware).
- **Reverb**: one warm room-to-hall stereo reverb (size, damping, pre-delay,
  per-patch send).

---

## 3. KOTO — modelled pluck

Digital waveguide string (fractional delay with good interpolation, loop
filter, stiffness allpass), with:

- **Pluck & tone core**: pluck position (comb on the excitation), nail
  (tsume, bright and sharp) ↔ finger (soft), strength, string damping and
  decay, a small pitch drop at the attack (tension), a wooden body (a few
  modes in the 150–600 Hz region, paulownia-like) and radiation filter.
- **Oshide bends**: pressing behind the bridge. A knob bends held notes;
  patches can also carry automatic shapes: bend-in from below, ato-oshi
  (bend up after the pluck), yuri (slow vibrato after the attack).
- **Sawari & strings**: amplitude-dependent bridge buzz, and a shared bank
  of sympathetic strings tuned to the current koto tuning, excited quietly by
  the output (one bank for all voices, not one per voice).
- **Tremolo & gliss helpers**: sukuigaki-style re-plucks while a key is held
  (rate in Hz or tempo-free), and a quick glissando across the scale.
- Release: finger damping with a little release noise.

## 4. MALLET — bars to mallet piano

Modal synthesis: up to 12 two-pole resonators per voice, excited by a mallet
or hammer contact pulse.

- **MATERIAL** continuum through mode-ratio sets: free bar → marimba (tuned
  1 : 4 : 10…) → vibraphone → xylophone → glockenspiel → kalimba tine →
  stiff string (harmonic with inharmonicity). Ratios interpolate, so morph
  can travel between instruments.
- **Mallet**: hardness (pulse width and brightness), strike position (mode
  weights), strength; a soft felt option for the piano end.
- **Resonator tube**: tuned tube under the bar (body, boom, longer sustain);
  vibraphone motor tremolo.
- **String layer** (the piano end): 2–3 near-unison strings per note
  (beating), two-stage decay (prompt sound and aftersound), stretched
  partials, a shared soundboard body.
- Frequency-dependent decay (high modes die faster), damper behaviour on
  release, HOLD as a sustain pedal.

Built (M2, 2026-11-24): 12 modes per note (doubled with UNISON for the
piano end's second string, which also gives the two-stage decay through
SUSTAIN); MATERIAL interpolates the mode sets kalimba → glockenspiel →
xylophone → vibraphone → marimba → string (STRETCH sets the string's
inharmonicity); per-material fundamental decay (vibes ring ~9× longer than
xylophone at the same DECAY); HARD/FORCE set the contact time, POS the
strike point, NOISE the knock; TUBE resonator, BOARD (shared soundboard),
TREM/T.RAT (one motor for all bars), RELS damper (127 = no damper). Modes
above ~13.8 kHz are skipped. Resonator feedback truncates toward zero so
tails decay to true silence (rounding left limit cycles).

## 5. PAD — additive harmonics

An original fixed-point additive engine (Plaits' harmonic oscillator is the
inspiration; no Mutable code is used):

- **Core**: up to 32 sine partials per voice; spectral peak (**centroid**),
  **spread** (bump width), bump shape, **odd/even** balance, tilt, stretch
  (inharmonicity), partial count.
- **Partial drift**: each partial does slow random walks in pitch and level,
  so held chords breathe.
- **Spectral envelope**: centroid and spread follow their own slow envelope
  (bloom open, settle).
- **Unison / detune**: up to 3 copies per voice, sharing the partial budget.
- **Formant & breath**: a vowel-like formant bump at a fixed frequency that
  can sweep, plus a filtered noise layer for air.
- Partial amplitudes are computed per block (128 frames) and ramped per
  sample.

---

## 6. Morph between patches

- Each patch can morph towards a **target patch of the same engine**.
- **SELECT** is the morph knob (0–100 %). **SEL + PRESETS** picks the target.
- Continuous parameters interpolate; discrete ones (tuning, material set
  where it is a choice) switch at 50 %.
- The morph position is a performance control; it is saved with the patch.

## 7. Keyboard and performance

- 27 keys, OCT−/OCT+ (±3 octaves).
- **Scales & tunings**: equal temperament, just intonation and koto tunings
  (hirajoshi, kumoi, kokinjoshi, iwato, akebono…), root key; option for the
  white keys to play the scale.
- **Strum**: keys pressed together are spread over a strum time (up/down).
- **Hold / latch**: notes keep sounding until HOLD is pressed again.
- **Humanize**: tiny random tuning, timing and level per note (stands in for
  velocity).

## 8. Patches and storage

- **64 user slots** plus **32 factory presets** in firmware (read-only),
  spread across the three engines, from traditional to extreme and
  experimental.
- **Names**: Markov-chain names with a MAVIS word list (wood, water, silk,
  shrine, rain, moss…), plus one-character editing.
- Stored in the user partition with the same log-structured store as ERISIAN
  (`shared/storage`), under MAVIS's own keys and magic, so both firmwares'
  data can live side by side.
- Saves wait for silence (no voice sounding) because every flash write drops
  audio; "SAVE PENDING: RELEASE KEYS" until then.

## 9. Controls (proposal; confirm on hardware)

| Control | Function |
|---|---|
| KNOB1–4 | The four parameters on screen |
| PRESETS | Browse patches (factory + user); until M5: switch engine |
| SELECT | Morph towards the target patch |
| ALGORITHM | Page within the edit mode |
| EDIT | Next edit mode |
| SEL + PRESETS | Choose the morph target |
| SAVE | Save screen (slot, name) |
| OCT− / OCT+ | Octave (OCT− held at power-on = rescue) |
| ARP | Hold / latch |
| SEQ | Strum on/off |
| REC | Koto tremolo (re-pluck while held) |
| FX | Reverb page |
| GLO | Global: tuning/scale, root, quality, voices, dual core (off by default) |
| ENV / LFO | Envelope / motion pages |
| HOME | Status (CPU, overruns, RAM, flash) |

## 10. Screens

**Live visualiser**, minimal text: the koto string's shape, the mallet's mode
amplitudes, the pad's partial spectrum, drawn from the sounding voice. One
accent colour per engine. Boot: plain "MAVIS" + version.

---

## 11. Phases

| Phase | Content |
|---|---|
| M0 | Skeleton: boot, rescue, store mount, voice pool, keys + octave, status page (CPU/overruns), CI build + emulator boot |
| M1 | KOTO core (string, pluck, body), tunings/scales |
| M2 | MALLET (modes, material continuum, tube, string layer) |
| M3 | PAD (partials, drift, spectral envelope, unison, formant/breath) |
| M4 | Reverb |
| M5 | Patches: 64 slots, factory set, names, morph |
| M6 | Performance: strum, hold, humanize, oshide shapes, tremolo/gliss, sympathetic strings, visualiser |

Each phase ends with a CI build, host tests and a hardware check by Charles
(logged in `docs/HARDWARE.md`).
