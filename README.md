# Mono

Mono is a clean-room, Monomachine-inspired digital instrument for Ableton
Move, built as a [Schwung](https://github.com/charlesvestal/schwung) module.

It ships in two forms:

- **Mono Voice** (`mono-voice`) — one machine voice inside a normal Move
  track, using Move's pads and sequencer.
- **Mono** (`mono`) — a full-surface six-track instrument with an internal
  64-step sequencer and per-step parameter locks.

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
- Hold step + turn knob: write a parameter lock
- Hold step + Shift + turn knob: remove that parameter lock

## Clean-room scope

Mono contains no Elektron firmware, factory wavetables, factory samples, or
copied visual assets. The initial synthesis algorithms are original
implementations guided by publicly documented behavior. Exact response-curve
calibration against reference hardware is future work.
