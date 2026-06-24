# Gameplay framework & tooling

**Status:** PENDING IDEA

> Inspiration backlog — not yet implementable as written. Needs a codebase pass (the input pipeline,
> the component-registry serialization for prefabs/save, and how visual scripting transpiles to Luau).

The connective tissue that turns a renderer-with-physics into an engine you build *games* in. Several
pieces are small and self-contained (input mapping, gameplay tags); the heavier ones (prefabs, save/load,
GAS) are blocked on **scene-graph parenting** + **stable entity GUIDs**.

## What it is

Input mapping, hierarchical gameplay tags, prefabs/variants, save/load, visual scripting, and a gameplay
ability system.

- **UE5:** Enhanced Input + Gameplay Tags + Blueprints + the Gameplay Ability System (GAS).
- **Unity:** the Input System + Prefabs/Variants + Visual Scripting (Bolt).

## Core technique

- **Input mapping:** device input → processors (dead zone, invert) → triggers (pressed/held/tap) → a
  typed, named **action**; stackable **mapping contexts** (gameplay vs. menu). Replaces raw-key polling
  (per the no-compat rule — one input path).
- **Gameplay tags:** hierarchical dot-tags (`Weapon.Ranged.Pistol`) in fast containers; feed GAS, input
  contexts, and save profiles.
- **Prefabs/variants:** a serialized entity subtree + sparse **override deltas** + variant chains.
- **Save/load:** reuse the component registry, scoped to runtime-mutable components; needs stable entity
  GUIDs + reference re-linking.
- **Visual scripting — recommendation:** **transpile a React Flow graph → Luau** (mirror the material →
  Slang pipeline) rather than build a second VM. One runtime, one debugger.
- **GAS:** tags → an attribute aggregator → gameplay effects (modifiers) → abilities (with async tasks) →
  cues (cosmetic feedback).

## Build size

- **M** input mapping — self-contained, immediate value.
- **S–M** gameplay tags — standalone-useful, feeds the rest.
- **L** prefab/variant system — **blocked on scene-graph parenting**; pairs with undo/redo (a known gap).
- **M** save/load — reuse the registry; needs stable GUIDs + reference re-linking.
- **L–XL** visual scripting (as Luau codegen).
- **XL** full GAS (tags → attributes → effects → abilities → cues; skip the networking half for now).

## Dependencies (do these first)

- **Scene-graph parenting** (a known gap) — prefab subtrees.
- **Stable entity GUIDs + partial registry (de)serialization** — prefabs + save/load + (later) networking.
- **Undo/redo** (a known gap) pairs naturally with the prefab/override editing.
- Visual scripting rides on the existing **React Flow → codegen** pattern; target Luau, not a new VM.

## What we reuse / what's missing

**Reuse:** Luau scripting (the runtime for both scripts and transpiled visual scripts), the JSON project
format + component registry (prefabs/save), the node-graph editor + codegen (visual scripting, GAS effect
calcs), hecs (ASC components, attributes), animation montages (GAS ability tasks), Jolt casts (GAS cues),
and the control plane.

**Missing:** the input pipeline, the gameplay-tag containers, stable entity GUIDs, scene-graph parenting,
undo/redo, and the GAS attribute/effect/ability machinery.

## Notes & references

- UE5 Enhanced Input + Gameplay Ability System docs (GAS is famously deep — the attribute/effect/ability
  decomposition is the part worth copying).
- Unity Input System + Prefab Variant docs.
- The material → Slang codegen already in-tree is the template for visual-scripting → Luau.
