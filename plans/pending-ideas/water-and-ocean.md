# Water & ocean

**Status:** PENDING IDEA

> Inspiration backlog — not yet implementable as written. Needs a codebase pass (a reusable GPU-FFT
> utility, a clipmap water mesh, single-layer water shading via the node-graph, and Jolt buoyancy forces).

The **Gerstner evaluator + buoyancy** is the cheap, high-value gameplay win (deterministic floating via
Jolt with no GPU readback). The **FFT spectral ocean** is the visual showcase but needs a reusable
GPU-FFT building block first.

## What it is

Oceans, lakes, and rivers with waves, buoyancy/floating physics, refraction/underwater, and shoreline
interaction.

- **UE5:** the Water system (Gerstner waves, water bodies as splines, single-layer water shading) +
  Water Buoyancy.
- **Unity:** HDRP Water (multi-band FFT).

## Core technique

**FFT spectral ocean (Tessendorf):** seed a Phillips/JONSWAP **spectrum**, phase-advance it in frequency
space, **inverse-FFT** (Stockham FFT on the GPU) to get displacement + normal + a **Jacobian** (which
detects wave folding for foam), summed across several **cascades** (wavelength bands) for detail.

**Gerstner evaluator + buoyancy:** a closed-form sum of trochoidal waves evaluated **identically on CPU
and GPU**, so physics gets exact surface height with no GPU readback. Buoyancy applies hydrostatic +
hydrodynamic forces at pontoon points or per submerged triangle via Jolt `AddForce`.

**Shading:** single-layer water = Fresnel BRDF + Beer–Lambert volume absorption + screen-space refraction
(reuses IBL/SSR/the node-graph). **Foam** + an interactive **ripple height-field** (a compute height
field fed by the contact ring / foot IK) react to objects. The surface mesh is a **clipmap** centered on
the camera.

## Build size

- **L** FFT spectral ocean — the reusable **GPU-FFT** building block is the hard part (also useful for
  bloom/convolution).
- **S** Gerstner evaluator; **S/M** buoyancy — **top water pick for gameplay value.**
- **M** clipmap water mesh; **M** single-layer water shading; **M** underwater post.
- **M** foam + interactive ripple field.
- **L** rivers/lakes (partly blocked on terrain + a spline primitive).

## Dependencies (do these first)

- **GPU-FFT utility** (shared enabler) — gates the FFT ocean; Gerstner needs none of it.
- **Jolt buoyancy** rides on existing `AddForce` + the deterministic world.
- *Rivers/lakes* want **heightfield-terrain** (shorelines/carving) + a **spline primitive**.
- *Ripples* reuse the **contact-event ring** / foot IK as the disturbance source.

## What we reuse / what's missing

**Reuse:** compute + render graph (FFT + height fields), bindless, Jolt (deterministic buoyancy — the
payoff), IBL/SSR + the material node-graph (shading), and the contact ring (ripple sources). A Luau
`sa.water_height` query fits the scripting model.

**Missing:** a reusable GPU-FFT utility, a clipmap water-mesh primitive, and a spline primitive (rivers).

## Notes & references

- Tessendorf, "Simulating Ocean Water" — the FFT spectrum method.
- Gerstner/trochoidal waves — the closed-form evaluator for CPU/GPU-shared height.
- UE5 Water + Water Buoyancy docs; Unity HDRP Water (multi-band FFT) for the cascade model.
