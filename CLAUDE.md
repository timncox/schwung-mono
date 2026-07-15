---
status: active
last_touched: 2026-07-15
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

Each track owns one monophonic voice, 112 base/effective parameters (seven
pages of eight plus an eight-control Shift bank for every page), three LFOs,
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

SuperWave saw/pulse edges are PolyBLEP band-limited. Pulse follows the
documented base + close-unison-pair + two-sine-sub topology; Ensemble provides
four optional pitched voices and chorus companions. SID uses a 24-bit phase
counter approximation and a 23-bit pitch-clocked noise LFSR. DigiPRO maintains
32 generated 512-sample/12-bit waves. FM+ Static uses the displayed ratio list,
per-block feedback, a combined envelope/volume response, and harmonic Tone
scaling.

## Parameter pages

- 0 SYNTH: eight machine-specific controls.
- Shift + SYNTH: four additional machine-specific controls plus drift, fold,
  bit depth, and noise.
- 1 AMP: attack, hold, decay, release, distortion, volume, pan, portamento.
- Shift + AMP: envelope curves, velocity response, key level, envelope amount,
  pan key tracking, and gain.
- 2 FILTER: base, width, HP resonance, LP resonance, envelope attack,
  envelope decay, envelope base, envelope width.
- Shift + FILTER: key/velocity tracking, envelope amount, pre-drive, HP/LP
  slopes, dry/wet mix, and saturation.
- 3 EFFECT: EQ frequency/gain, sample-rate, delay send/time/feedback/base/width.
- Shift + EFFECT: EQ Q/mix, bit depth, ping-pong, delay duck/drive, and delay
  modulation rate/depth.
- 4-6 LFO 1-3: destination, trigger mode, waveform, multiplier, speed,
  interlace, depth, phase.
- Shift + LFO 1-3: fade, delay, slew, symmetry, steps, polarity, velocity
  response, and key tracking.

FILTER BASE is the high-pass edge and WDTH is the octave distance to the
low-pass edge. Both controls advance one octave per eight steps; full key
tracking places BASE 0 two octaves below the played note. New tracks default
to full key tracking.

Each LFO destination is a direct 0-113 enum: Off, Pitch, then parameter IDs
0-111. Modulation of LFO parameters is fed into the following sample and
clamped, allowing cross- and self-modulation without recursive evaluation.
State v9 stores this direct map, per-track timing and performance state,
machine-specific sound memories, and advanced step behavior while packing
112-bit lock masks and 7-bit lock values compactly. State v2-v8 patches migrate
on load; v2/v3 seven-destination LFO routings are translated to the direct map,
and v2-v8 Pulse panels/locks migrate to the corrected primary layout.

The engine keeps Trigger and Wave in their original 0-127 state slots, split
into five equal bands. Custom Move and browser UIs present those bands as
named, detented choices and write canonical values 0/32/64/96/127. Compact
five-character destination labels prevent values from spilling into adjacent
Move screen columns without changing the saved-state format.

The sound-generator manifest exposes stable `synN`, `ampN`, `fltN`, `fxN`,
and `lfoX_N` keys, including numbered 9-16 keys for every secondary bank.
Dynamic `alt1`...`alt8` aliases expose the selected page's Shift bank. The
overtake UI exposes the selected primary page through dynamic `p1`...`p8`
aliases.

## Sequencer

Patterns contain six tracks x 64 steps. A step stores note, velocity, gate,
independent note/amp/filter/LFO trigger bits, probability, 1–8 retrigs, cycle
condition, slide, and locks for any of the 112 sound parameters. On each trig,
effective parameters reset to the track's base values and then apply that
step's locks. MIDI clock advances at six ticks per 16th; swing offsets
alternating steps, and the engine falls back to `host->get_bpm()` when its own
transport is running without clock.

The `record` performance parameter arms live lock capture: while transport is
running, page and Shift-layer edits write the selected track's current step as
well as its base value. In `mono-voice`, incoming Move clock drives the same
16-step lane while native Move notes remain authoritative. Recorded locks are
serialized with the normal module state, but the record-arm switch itself is
cleared on recall. Render-time smoothing applies a 30 ms one-pole glide to
continuous targets. Waveform/routing/mode enums stay stepped, and smoothing
uses separate targets so LFO cross-modulation never mutates saved parameters.

Full Mono also exposes a saved playback window over the 64 stored steps.
`pattern_start` is zero-based, `pattern_len` is clamped so the window ends by
step 64, and `play_order` selects Forward, Reverse, Pendulum, or Random. Order
changes restart traversal on the following sequencer tick; Pendulum does not
repeat its end points. The Move UI edits these settings in a separate Sequence
Setup view, while `all_steps` gives Remote UI one 64-value overview without
changing `step_page` or causing editor remounts.

Tracks can override the global start and length, rotate the resulting window,
and divide their clock by 1–8. They also carry saved mute/solo state and a
16-parameter memory for each synthesis machine. Step and track clipboards plus
a snapshot swap provide copy/paste and one-level undo/redo without allocating
in the audio render path.

## Fidelity boundary

The public manual documents topology and control intent but not the original
coefficient tables, nonlinearities, FM response curves, or factory waveform
data. Spectral, parameter-routing, restart, source-selection, and filter-octave
tests now protect the documented behaviors, but the algorithms remain original
approximations. Close calibration requires recordings from a reference
Monomachine across controlled MIDI parameter sweeps.

## Verification

- `make test` — native behavioral/audio simulator.
- `make sanitize` — AddressSanitizer + UndefinedBehaviorSanitizer simulator.
- `make arm` — aarch64 shared objects and clean GNU-tar archives in Docker.
- Hardware deployment is intentionally separate and permission-gated.
