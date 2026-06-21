+++
title = 'Physics panel'
weight = 8
+++

# Physics panel

The Physics panel is the play-mode window into the live Jolt world. It sits in the diagnostics group beside [Stats and the Profiler](../metrics-dashboard/) (open it from the Tools → Diagnostics menu), and while the scene is Playing it shows the world's body counts and a contact/trigger event feed. In Edit it shows an empty state — the Jolt world only exists during play — and, crucially, polls nothing: a closed panel or a panel in Edit makes zero control round-trips.

It also hosts the per-selection **ragdoll** and **character** controls. These are the substitute for tooling Saffron deliberately does not have: there is no interactive per-bone PhAT editor (no 3D bone handles), so a rig's ragdoll is driven by a blend slider and an enable button; and a character is nudged with direction buttons rather than play-testing with the keyboard.

## Telemetry, gated to matter

The reconcile poll fetches physics state only when the panel is **open and play is active**:

```ts
if (isPanelOpen(state, "physics") && state.playState !== "edit") {
  setPhysicsState(await client.physicsState());
  const drained = await client.drainContacts(contactsSince);
  appendContactEvents(drained.events, drained.overflowed);
}
```

`physics-state` returns the body and dynamic-body counts; `physics-bodies` lists every live body (owner entity, motion type, awake/sleeping, world position) via `World::list_bodies`, read-only getters that never perturb the deterministic sim; `drain-contacts` returns the contact and trigger transitions since a cursor, drained into a bounded newest-first ring (the cursor and ring reset on each fresh play session). All three are **Edit-safe** — they return inactive/empty rather than erroring when the world is null — so the panel can mount in Edit and simply show *"Enter Play to inspect the physics world."* The body table shows each body's motion and awake state; the contact feed marks trigger overlaps (a sensor collider) distinctly from solid touches and surfaces the engine's *events-dropped* flag when its ring wrapped.

## Ragdoll controls (a rig with Bone Physics)

When the selected entity carries a `BonePhysicsComponent`, a Ragdoll section appears:


- **Go limp** builds the Jolt `Ragdoll` from the rig's auto-fit bone bodies and lets it collapse (passive).
- **Active (motors)** turns the pose-driving motors on, so the ragdoll blends *toward* the animation rather than going slack — UE5's powered-ragdoll model.
- **Physics blend** is the 0–1 weight mixing physics against animation per the rig's blend layer: 0 is pure animation, 1 is pure physics, and a partial value is a hit-react that eases back.

The read-only readout shows the live `present` / `active` / mean weight / bone count from `get-ragdoll`. Every control errors when the world is null, so the whole section is disabled (with a tooltip) in Edit.

## Character move test (a Character Controller)

When the selection carries a `CharacterController` component, a Character section gives four direction buttons, a Jump, and a speed slider seeded from the controller's authored `maxSpeed`. A button calls `move-character` with `velocity = direction × speed` (the engine ignores the vertical component); the last move's on-ground state shows beside the heading. `move-character` does not error in Edit, but the write is inert until a physics step consumes it, so the section is disabled in Edit for the same reason as the ragdoll.

## Code

| What | File | Symbols |
|---|---|---|
| The panel | `editor/src/panels/PhysicsPanel.tsx` | `PhysicsPanel`, the World / Contacts / Ragdoll / Character sections, `PlayGate` |
| Registration | `editor/src/components/dock/panelRegistry.tsx` · `editor/src/state/dockLayout.ts` | the `physics` `SCENE_PANEL_REGISTRY` entry, `SCENE_PANEL_IDS`, `DEFAULT_LEAF` |
| Store state + poll | `editor/src/state/store.ts` | `physicsState`, `physicsBodies`, `contactLog`, `appendContactEvents`, the open-AND-playing poll block + `contactsSince` |
| Typed wrappers | `editor/src/control/client.ts` | `physicsState`, `physicsBodies`, `drainContacts`, `enableRagdoll`, `setRagdoll`, `getRagdoll`, `moveCharacter` |
| Backing commands | `engine/crates/control/src/commands_physics.rs` · `engine/crates/physics/src/world.rs` | `physics-state`, `physics-bodies` (+ `World::list_bodies`), `drain-contacts`, `enable-ragdoll`, `set-ragdoll`, `get-ragdoll`, `move-character` |
