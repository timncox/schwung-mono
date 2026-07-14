# Mono

Mono is a clean-room, Monomachine-inspired digital instrument for Ableton
Move, built as a [Schwung](https://github.com/charlesvestal/schwung) module.

It ships in two forms:

- **Mono Voice** (`mono-voice`) — one machine voice inside a normal Move
  track, using Move's pads and sequencer.
- **Mono** (`mono`) — a full-surface six-track instrument with an internal
  64-step sequencer and per-step parameter locks.

## Operation manual

Open the self-contained [interactive Mono manual](docs/index.html) for the
quick start, full Move control map, parameter-lock workflow, seven-page editor,
and machine reference.

## Current machines

- SuperWave Saw
- SuperWave Pulse
- SuperWave Ensemble
- SID 6581-style digital oscillator
- DigiPRO-style 512-sample, 12-bit wavetable oscillator
- FM+ Static-style two-block FM oscillator

Every track also has an AHDR envelope, dual-cutoff resonant filter,
distortion, EQ, sample-rate reduction, stereo filtered delay, portamento,
pan, and three assignable LFOs.

On the SYNTH page, hold Shift while turning knobs 1–8 to open the secondary
machine layer. The first four controls are machine-specific; the last four are
Drift, Fold, Bits, and Noise. Shift + jog continues to change machines.

LFO destination is a named seven-choice control: Off, Pitch, Filter Base,
Filter Width, Volume, Pan, or Delay.

## Saving and recall

- **Mono Voice sound preset:** in Schwung's Chain Editor, highlight the synth
  block, hold Shift and click the jog wheel, open **User Presets**, then choose
  **Save current**.
- **Mono Voice sound plus Move sequence:** save the Move Set. Move's native
  clips belong to the Set, not to a component preset. Open Set Overview with
  Shift + Step 1; Move saves Sets automatically.
- **Mono six-track pattern:** inside the full-surface build, hold Shift and
  click the jog wheel. **Save current** captures all six sounds, all 64 steps,
  and every parameter lock; choosing a saved name recalls it without starting
  transport automatically.

## Build and test

```sh
make test       # native host simulator
make sanitize   # ASan + UBSan
make arm        # aarch64 module archives via Docker
```

`make arm` produces:

- `build/mono-voice-module.tar.gz`
- `build/mono-module.tar.gz`

Deployment is separate because it writes to Move hardware:

```sh
scripts/deploy.sh
```

## Overtake controls

- Top-row pads 1-6: select track
- Top-row pad 7: cycle machine; Shift reverses direction
- Top-row pad 8: start/stop internal sequencer
- Lower three pad rows: chromatic performance keyboard
- Step buttons: select/toggle steps; arrows select one of four 16-step pages
- Jog wheel: select one of seven parameter pages
- Knobs 1-8: edit the current page
- Shift + knobs 1-8 on SYNTH: edit the secondary machine layer
- Hold step + turn knob: write a parameter lock
- Hold step + Shift + turn knob: remove that parameter lock

The Move display shows four parameters at a time. Touching knobs 1–4 or 5–8
automatically focuses that bank, while all eight knobs remain active. Schwung's
Remote UI opens a full editor for either build, including a browser keyboard
for quickly checking the note and audio path.

## Clean-room scope

Mono contains no Elektron firmware, factory wavetables, factory samples, or
copied visual assets. The initial synthesis algorithms are original
implementations guided by publicly documented behavior. Exact response-curve
calibration against reference hardware is future work.

## Version 0.1 boundary

This first vertical slice proves the module architecture and is ready for a
hardware smoke test. It does not yet include VO-6, BeatBox, DigiPRO user-wave
loading, FM+ Parallel/Dynamic, FX machines/neighbor routing, arpeggiators,
or song mode. The current factory-independent algorithms also need A/B
calibration against a reference Monomachine before they should be described
as sonically faithful.
