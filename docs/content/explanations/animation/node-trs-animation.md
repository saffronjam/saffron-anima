+++
title = 'Node-TRS animation'
weight = 7
+++

# Node-TRS animation

Not every animated thing is a skinned skeleton. A glTF that swings a lamp, opens a door, or orbits a
moon (`BoxAnimated` is the canonical example) animates plain scene-graph *nodes* — their translation,
rotation, and scale — with no skin in sight. Node-TRS animation drives a live entity forest from those
channels, reusing the same clip, sampler, and pose-override seam skeletal playback already uses.

## One track model, two targets

A clip mixes bone tracks, node-TRS tracks, and morph-weight tracks side by side. What a track drives is
its [`AnimTarget`]({{< relref "animation-data-model" >}}):

```rust
pub enum AnimTarget {
    Bone = 0,   // a skinned-mesh joint, resolved to a bone index by name at import
    Node = 1,   // a plain scene-graph node, bound by durable name at runtime
}
```

A `Bone` track writes through a resolved bone index; a `Node` track binds by the glTF node name. The
sampler is identical — translation/scale lerp, rotation slerp, the same Step/Linear/CubicSpline math —
the only difference is *where* the sampled local transform lands.

## A live entity forest

glTF import unconditionally decodes a node forest, and a mesh-bearing node carries its mesh node-locally
(`ImportedNode.mesh`), so there is one mesh-ownership shape for skinned meshes, OBJ imports, and animated
nodes alike. Spawn instantiates that forest as live `Transform` + `Relationship` entities under a
container root holding one `AnimationPlayer`. A single identity-transform root still collapses to one
entity; any non-identity or animated node keeps its container so it never loses a drivable local
transform.

The runtime binds each node track by **durable name → `Uuid` → `Entity`**, cached and re-resolved on a
stale handle through a first-match pre-order walk scoped to the player's forest — never the global
`find_entity_by_uuid`, which would cross instances and mis-bind a repeated forest name. A resolved node
track writes a `PoseOverride` on its entity, exactly as a bone track does on a joint; `update_world_transforms`
already prefers `PoseOverride` for any entity, so node playback reuses the skeletal write seam wholesale.

> [!NOTE]
> Node players get the **full** transition / cross-fade machinery, the same path skeletal playback uses
> — there is no reduced "nodes hard-sample only" mode.

## One playback surface

Because the node forest's player is an ordinary `AnimationPlayer`, the existing transport commands drive
it unchanged: `play-animation`, `seek-animation`, `set-animation-loop`, `get-animation-state`. There is
no second playback verb for nodes. `list-clip-bindings` resolves a clip's channels against the live
forest, so a node channel's label is the bound entity's current name — and an unresolved one falls back
to the raw glTF node name, which doubles as the broken-binding signal in the editor.

| What | File | Symbols |
|---|---|---|
| Track target + path | `geometry/src/types.rs` | `AnimTarget`, `AnimPath` |
| Node forest import | `geometry/src/gltf_import.rs` | `import_gltf_model`, `build_node_forest` |
| Forest spawn + collapse | `assets/src/spawn.rs` | `spawn_node_forest`, `is_single_identity_root` |
| Name binding + write seam | `animation/src/runtime.rs` | `find_named_descendant`, `resolve_node_targets`, `tick_node_rig` |
| Binding inspection | `control/src/commands_animation.rs` | `list-clip-bindings` |
