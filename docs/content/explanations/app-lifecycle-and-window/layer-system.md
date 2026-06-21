+++
title = 'Layers'
weight = 2
+++

# Layers

A layer is a unit of program behavior the app runs at each phase of the frame. It is a trait
with provided (default-empty) methods, not a class with required overrides. The app keeps a
list of boxed layers and invokes each hook on every one; the hooks a layer does not implement
fall through to the empty defaults and cost nothing.

This is the runtime-interface-as-trait pattern: the "interface" is the set of hooks, and an
implementation is a type that overrides only the hooks it needs. The shape mirrors a
conventional `App`/`Layer` lifecycle while dropping the inheritance that usually comes with it.

```rust
pub trait Layer {
    fn name(&self) -> &str { "Layer" }
    fn on_attach(&mut self, _app: &mut App) {}
    fn on_update(&mut self, _app: &mut App, _dt: TimeSpan) {}
    fn on_render(&mut self, _app: &mut App) {}                          // submit GPU work
    fn on_ui(&mut self, _app: &mut App) {}
    fn on_render_graph(&mut self, _app: &mut App, _graph: &mut RenderGraph) {}  // add passes
    fn on_detach(&mut self, _app: &mut App) {}
}
```

## How dispatch works

A client builds a type that implements `Layer`, overrides the hooks it needs, and pushes it
with `attach_layer`. `App` owns the layers in a flat `Vec<Box<dyn Layer>>` in attach order. At
each phase of the frame, the loop walks the vec and calls the hook on every layer through
`run_hook`:

```rust
run_hook(app, |layer, app| layer.on_update(app, dt));
```

`run_hook` moves the layer vec out of `App` for the duration of the pass (a `mem::take`), so a
hook can borrow `&mut App` — the window and the frame host — without aliasing the list being
iterated. After the pass it restores the vec, appending any layers a hook attached mid-pass.
Each hook takes `&mut App` rather than capturing it, so a layer never aliases the app it runs
inside.

## The callback set

Each hook maps to a fixed point in the loop (see [the main loop](../main-loop-and-run/)):

| Hook | When it runs | Typical use |
|---|---|---|
| `on_attach` | once, after `on_create`, before the loop | allocate resources, subscribe to window signals |
| `on_update(dt)` | every iteration, first | game logic, camera, animation; `dt` is a wall-clock `TimeSpan` |
| `on_render` | inside the frame, after `begin_frame` | record GPU work through the [submit seam](../the-submit-and-rendergraph-seams/) |
| `on_ui` | inside the frame, after `on_render` | per-frame UI-phase work (the host renders the scene here) |
| `on_render_graph(graph)` | inside the frame, after the engine's passes are added | add passes to the live frame graph |
| `on_detach` | once, during teardown, before `on_exit` | drop GPU resources |

The two rendering hooks are separate on purpose. `on_render` records commands into the
current frame; `on_render_graph` is handed the live graph so the layer can add whole passes.
That split is the subject of [the submit and render-graph seams](../the-submit-and-rendergraph-seams/).

## Why a trait of hooks, not a required base

A trait with default-empty methods offers two advantages. Every hook is independently
optional, with no empty-override boilerplate. And a layer carries its own state in the
implementing type's fields, so the same `Layer` contract works for a small probe struct or the
full editor host. The cost is one dynamic dispatch per hook per layer per phase, negligible
next to a frame's GPU work.

The editor host is built this way. It is a single `Layer` whose fields hold the scene-edit
state and which wires the scene render into `on_ui` and the editor camera into `on_update`.

## In the code

| What | File | Symbols |
|---|---|---|
| Layer trait | `app/src/lib.rs` | `Layer` |
| Attaching | `app/src/lib.rs` | `attach_layer`, `App::layers` |
| Dispatch | `app/src/lib.rs` | `run_hook`, `step_frame`, `run_frame` |
| The host layer | `host/src/layer.rs` | the host's `Layer` impl (`on_update`, `on_ui`) |

> [!TIP]
> Layers run in attach order at every phase, and there is no priority or removal API. If one
> layer's `on_update` must see the result of another's, attach it second. The order you call
> `attach_layer` in `on_create` is the order everything runs for the life of the program. A
> layer attached from inside a hook joins on the *next* pass, not the current one.

## Related

- [Main loop](../main-loop-and-run/) — where the hooks are invoked
- [Render seams](../the-submit-and-rendergraph-seams/) — what `on_render` vs `on_render_graph` do
- [Window and events](../window-and-events/) — the signals a layer subscribes to in `on_attach`
