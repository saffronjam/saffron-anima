# Audio system

**Status:** PENDING IDEA

> Inspiration backlog — not yet implementable as written. Needs a codebase pass (a new `saffron-audio`
> crate, the SDL3 audio backend already in the toolbox, and a lock-free parameter ring mirroring the
> existing contact-event ring).

The engine currently has **no audio at all** — this is a greenfield subsystem. It is near-orthogonal to
the renderer, and it reuses several existing patterns: Jolt raycast/sensors, the node editor, the
lock-free ring, and Luau. Occlusion is the standout effort/impact win.

## What it is

A full game-audio stack: a mixer, 3D spatialization with attenuation, occlusion, reverb zones, and
adaptive music.

- **UE5:** the built-in audio engine + MetaSounds (node-based procedural DSP).
- **Unity:** the built-in audio mixer, usually augmented with FMOD or Wwise.

## Core technique

A **voice → submix DAG → DSP-chain** graph mixes active sounds. **Spatialization** pans by listener
relative direction and attenuates by distance curve; HRTF convolution gives true binaural placement.
**Occlusion** raycasts listener→source through Jolt and applies a low-pass biquad + attenuation when
blocked. **Reverb zones** are trigger volumes (reuse Jolt sensors + the contact ring) that crossfade
reverb DSP. **Adaptive music** is a sample-accurate clock driving a state machine that crossfades stems.
**MetaSounds-style** authoring layers a node graph over a DSP node list — the same React-Flow-→-codegen
shape as the material editor, with a DSP target instead of Slang.

## Build size

- **L** core audio engine/mixer (new `saffron-audio` crate; voice → submix DAG → DSP chains).
- **M** spatialization + attenuation (panning) / **L** with HRTF convolution (`rustfft`).
- **S–M** occlusion (Jolt raycast + low-pass biquad) — **best effort/impact ratio.**
- **M** reverb zones + DSP (zones reuse Jolt sensors/triggers + the contact ring).
- **M** adaptive music (sample-accurate clock + state machine, Luau-driven).
- **L–XL** MetaSounds-style node DSP (reuse React Flow + codegen; target = a DSP node list).

## Dependencies (do these first)

- **None hard** — it is a greenfield crate. The SDL3 audio backend is already in the toolbox.
- A **lock-free SPSC parameter ring** mirroring the contact-event ring (game thread → audio thread).
- *Spatial gameplay coupling* reuses **Jolt raycast/sensors** (occlusion + reverb zones).

## What we reuse / what's missing

**Reuse:** SDL3 (output backend), Jolt raycast (occlusion) + sensors/triggers (reverb zones), the
contact-event-ring pattern (lock-free parameter passing), the node editor + codegen (MetaSounds), hecs
(audio-source/listener components), Luau (music state), and the control plane.

**Missing:** the entire DSP stack, audio decode crates (ogg/wav), and an HRTF dataset (binaural only).

## Notes & references

- UE5 MetaSounds docs — the node-DSP authoring model (and why it maps onto our codegen pipeline).
- `rustfft` / `cpal` / `symphonia` — candidate Rust crates for FFT, output, and decoding.
- HRTF datasets (e.g. MIT KEMAR / SADIE) for binaural spatialization.
