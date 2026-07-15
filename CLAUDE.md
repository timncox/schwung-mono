---
status: active
last_touched: 2026-07-14
---

# Mono

Mono is a clean-room Schwung instrument inspired by the Elektron Monomachine's
machine-per-track architecture. It does not contain Elektron code or factory
content. Version 0.1 is a playable architectural slice, not a bit-exact clone.

## Builds

- `mono-voice` — ordinary `sound_generator`; one monophonic machine voice,
  played and sequenced by Move.
- `mono` — full-surface `overtake`; six internal tracks, a 64-step pattern,
  per-step locks, and direct surface editing.
- Both wrappers use `src/mono_core.c`. The render contract is 44.1 kHz,
  128-frame, interleaved signed 16-bit stereo.

## Engine

Each track owns one monophonic voice, 64 base/effective parameters (seven
pages of eight plus an eight-control Shift synthesis layer), three LFOs,
amp/filter envelopes, dual state-variable filters,
sample-rate reduction, distortion, a filtered stereo delay, and 64 sequencer
steps. The initial machine set is:

0. SuperWave Saw
1. SuperWave Pulse
2. SuperWave Ensemble
3. SID 6581-style digital oscillator
4. DigiPRO-style 512-sample/12-bit wavetable oscillator
5. FM+ Static-style two-block FM oscillator

The wavetable bank is generated from original mathematical wave recipes at
startup. User waveform import and reference-hardware calibration are later
milestones.

## Parameter pages

- 0 SYNTH: eight machine-specific controls.
- Shift + SYNTH: four additional machine-specific controls plus drift, fold,
  bit depth, and noise.
- 1 AMP: attack, hold, decay, release, distortion, volume, pan, portamento.
- 2 FILTER: base, width, HP resonance, LP resonance, envelope attack,
  envelope decay, envelope base, envelope width.
- 3 EFFECT: EQ frequency/gain, sample-rate, delay send/time/feedback/base/width.
- 4-6 LFO 1-3: destination, trigger mode, waveform, multiplier, speed,
  interlace, depth, phase.

Each LFO destination is a direct 0-65 enum: Off, Pitch, then parameter IDs
0-63. Modulation of LFO parameters is fed into the following sample and
clamped, allowing cross- and self-modulation without recursive evaluation.
State v4 stores this direct map; v2/v3 seven-destination presets migrate on
load.

The engine keeps Trigger and Wave in their original 0-127 state slots, split
into five equal bands. Custom Move and browser UIs present those bands as
named, detented choices and write canonical values 0/32/64/96/127. Compact
five-character destination labels prevent values from spilling into adjacent
Move screen columns without changing the saved-state format.

The sound-generator manifest exposes stable `synN`, `ampN`, `fltN`, `fxN`,
and `lfoX_N` keys. Secondary synthesis controls use stable `syn9`...`syn16`
and `alt1`...`alt8` aliases. The overtake UI exposes the selected page through
dynamic `p1`...`p8` aliases.

## Sequencer

Patterns contain six tracks x 64 steps. A step stores note, velocity, gate,
independent note/amp/filter/LFO trigger bits, and locks for any of the 64 sound
parameters. On each trig, effective parameters reset to the track's base values
and then apply that step's locks. MIDI clock advances at six ticks per 16th;
the engine falls back to `host->get_bpm()` when its own transport is running
without clock.

## Fidelity boundary

The public manual documents topology and control intent but not the original
coefficient tables, nonlinearities, FM response curves, or factory waveform
data. Current algorithms are original approximations with deliberately crisp
digital behavior. Close calibration requires recordings from a reference
Monomachine across controlled MIDI parameter sweeps.

## Verification

- `make test` — native behavioral/audio simulator.
- `make sanitize` — AddressSanitizer + UndefinedBehaviorSanitizer simulator.
- `make arm` — aarch64 shared objects and clean GNU-tar archives in Docker.
- Hardware deployment is intentionally separate and permission-gated.
