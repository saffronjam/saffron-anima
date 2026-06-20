# Phase 8 — `saffron-sceneedit` crate + the edit context

**Status:** COMPLETED

**Depends on:** 03-ecs-and-scene:phase-7-scene-document-and-migrations, 00-foundations:phase-3-signal-crate

## Goal

Stand up the `saffron-sceneedit` crate and the `SceneEditContext` session struct — the editor's mutable
state core: the authored scene, the component registry, selection, the project path fields, the version
stamps, the gizmo op/space source-of-truth, the overlay options, the smoothing queues, and the
asset-preview block. Implement `set_selection`, the `active_scene`/`previewing` accessors,
`register_builtin_components` (the one canonical registration site for all 24 components), the
`ScriptInputState` + `derive_script_input_edges`, and the debug-overlay serde. Play mode, fly-camera, and
gizmo math are the next three phases; this phase delivers the container and its invariants.

## Why this shape (NO LEGACY)

- **`saffron-sceneedit` depends on `{core, signal, scene, json}` — no Rendering, no SDL.** The C++ module
  is deliberately backend-neutral: input arrives as plain structs the host fills (`sceneedit/AGENTS.md`).
  The Rust crate keeps that — the gizmo *geometry* (`buildNativeGizmo`) stays in the host; only the
  hit-test/drag *math* lives here (phase-11). This is what lets control drive editor state without pulling
  in the renderer.
- **The heap-ownership dance disappears.** The C++ heap-owns `SceneEditContext`
  (`newSceneEditContext`/`destroySceneEditContext`, `scene_edit_context.cpp:94`) only to keep its heavy
  entt/json destructor out of the client TU. Rust has no header/TU split, so the context is just an owned
  struct with `Default`/`new` and automatic `Drop` — the `new*`/`destroy*` pair is deleted (a PP-3
  subtraction). The seeding (`new_scene_edit_context` seeds a Camera + Sun and selects the camera) becomes
  `SceneEditContext::new()`.
- **`active_scene` is the single sanctioned accessor, returning `&mut Scene`.** The three-way branch
  (preview-view → play duplicate → authored scene) is one `match`/`if` chain
  (`scene_edit_context.cppm:292`); nothing else may branch on `play_state`/`preview_scene` to pick a
  scene. `playScene`/`previewScene` are `Option<Scene>` (the idiomatic `std::optional<Scene>` port);
  `active_scene` returns `&mut *play_scene` / `&mut *preview_scene` / `&mut scene`. `previewing` reads
  `preview_scene.is_some() && preview_active_view`.
  - *Added substrate (09-control-plane:phase-3):* `registry_and_active_scene(&mut self) ->
    (&ComponentRegistry, &mut Scene)` — the same three-way scene branch, but borrowing the `registry`
    field and the active scene **disjointly** in one place. The scene command handlers
    (`add-component`/`set-component-order`/`inspect`/`copy-entity`/…) call a registry method that also
    takes the active scene (`component_order`, `set_component_order`, `append_component_order`,
    `remove_component_order`), which `self.registry.method(self.active_scene(), …)` cannot express
    (`active_scene` borrows all of `self`). The C++ held both at once (`ctx.sceneEdit.registry` +
    `activeScene(ctx.sceneEdit)`); this accessor is the Rust split for that pair, keeping the
    scene-routing branch the single sanctioned one.
- **Version stamps are load-bearing and preserved exactly.** `scene_version`, `selection_version`,
  `play_version`, `animation_version` are the control-plane reconcile-poll keys (`sceneedit/AGENTS.md`):
  the editor desyncs if the wrong one is bumped. They stay `u64` counters on the context, bumped at the
  same sites (e.g. `set_selection` bumps `selection_version` and publishes `on_selection_changed`).
- **Signals come from saffron-signal.** `on_selection_changed: SubscriberList<Entity>` and
  `on_play_state_changed: SubscriberList<PlayState>` are the hand-rolled `SubscriberList` from
  00-foundations (snapshot dispatch, bool-stop, !Send `FnMut`). They are not Refs.
- **`ScriptInputState` + `derive_script_input_edges` live here as a shared POD.** The C++ keeps this on
  `Scene`-adjacent shared ground so Script and SceneEdit (both importing Scene) avoid a cross-module edge
  (`scene.cppm:1235`). In Rust, `ScriptInputState` is a plain struct on the context (held/mouse raw sets
  + derived pressed/released edges + deltas + the prev-tick memory); `derive_script_input_edges` is a
  free function the host calls once per tick before `tick_scripts`. The sets are `HashSet<String>`.

## Grounding (real files / symbols)

- `engine-old/source/saffron/sceneedit/scene_edit_context.cppm`: `SceneEditContext` (213), `activeScene`
  (292), `previewing` (310), the version-stamp fields (226–247), `MaterialSmoothTarget`/
  `TransformSmoothTarget` (126/138), `NativeGizmoState` (102), `SkeletonOverlayOptions`/
  `DebugOverlayOptions` (192/202), `PlayState` (148), `ScriptError`/`ScriptLog` rings (161/176), the
  asset-preview block (265–280), `setSelection` decl (322), `registerBuiltinComponents` decl (352),
  `AssetDragPayload` (316).
- `engine-old/source/saffron/scene/scene.cppm`: `ScriptInputState` (1235), `deriveScriptInputEdges` (1256).
- `engine-old/source/saffron/sceneedit/scene_edit_context.cpp`: `setSelection` (20), `debugOverlaysToJson`/
  `debugOverlaysFromJson` (27/36), `newSceneEditContext`/`destroySceneEditContext` (94/116), the
  gizmo op/space name↔enum helpers (49–92).
- `engine-old/source/saffron/sceneedit/scene_edit_components.cpp`: `registerBuiltinComponents` (18) —
  the 24-call canonical site.

## Acceptance gate

- Cargo workspace compiles; `saffron-sceneedit` builds with `#![deny(unsafe_code)]` and depends only on
  `{core, signal, scene, json}`.
- `cargo test -p saffron-sceneedit`:
  - `SceneEditContext::new()` seeds a Camera + Sun and selects the camera; `register_builtin_components`
    registers all 24 components (assert count + a sample by name).
  - `set_selection` bumps `selection_version` and publishes `on_selection_changed`.
  - `active_scene` returns the authored scene in Edit, the play duplicate when a `play_scene` is set, and
    the preview scene when `preview_scene` + `preview_active_view`; `previewing` reflects the same.
  - `derive_script_input_edges` produces pressed/released edges and mouse deltas against the prev tick,
    then rolls the memory forward (port the C++ semantics).
  - `debug_overlays`/`gizmo_op`/`gizmo_space` name↔enum round-trips, debug-overlay serde round-trip.
- Workspace build green; prior phases still pass.
