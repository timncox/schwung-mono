---
status: active
last_touched: 2026-07-20
deploy: scripts/deploy.sh
---

# Mono

Clean-room, Monomachine-inspired Schwung instrument for Ableton Move. It
ships as the `mono-voice` sound generator and the `mono` six-track overtake
tool, both backed by `src/mono_core.c`.

## Work safely

- Run `make test` after DSP, sequencing, parameter, or state changes.
- Run `make sanitize` before release.
- Keep the render path non-allocating; allocate delay and wavetable memory in
  `mono_create()` only.
- Keep the DSP implementation clean-room. Do not add Elektron firmware,
  factory wavetables, factory samples, or copied visual assets.
- `make arm` stages and cross-compiles both module archives with Docker.
- One tag publishes both archives. Keep the top-level `release.json` fields as
  the Custom GitHub fallback for `mono`, and keep its `modules` entries aligned
  with the `mono` and `mono-voice` catalog IDs.
- `scripts/deploy.sh` writes to Move hardware. Do not run it without explicit
  deployment authorization.

See `CLAUDE.md` for the engine layout, parameter pages, sequencer model, and
fidelity boundary.
