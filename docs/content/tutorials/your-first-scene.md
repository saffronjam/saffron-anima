+++
title = 'Your first scene'
weight = 1
+++

# Your first scene

Start from an empty editor, add a cube, light it, give it a camera, and save the result to a
project file you can reload. Every step is a real `sa` command with a menu equivalent, so you
can follow either path. You end with a `project.json` that draws a lit cube through a scene
camera.

## Start the editor and the control CLI

The engine builds and runs inside the `saffron-build` toolbox (see [the build
environment](../../explanations/architecture-and-conventions/) and `AGENTS.md`). The
`cmd/sa` wrapper in the repo root launches the editor and a CLI that talks to it:

```sh
./cmd/sa start --build      # builds, launches SaffronAnima, waits for its socket
./cmd/sa ping               # pong  engine=SaffronAnima  version=...  pid=...
```

`ping` confirms the CLI is connected to the live editor over its unix socket. Every command
below drives that same editor. To work in the window instead, use the **Hierarchy** panel
(top-left), the **Inspector** (below it), the **Assets** browser (bottom), and the 3D
**Viewport** in the center.

> [!NOTE]
> `cmd/sa` runs the built `sa` binary through `toolbox run`. If you launched the editor
> another way, plain `sa <command>` from inside the toolbox reaches the same socket.

## Add a cube

The engine ships a `cube` model under `engine/assets/models/`, copied next to the
host executable at build time. Import it:

```sh
./cmd/sa import-model models/cube.gltf
```

This bakes the model to a `.smesh`, registers it in the project asset catalog, and spawns an
entity carrying it, already selected. The reply gives you the entity id and the mesh asset
id. List what you have so far:

```sh
./cmd/sa list-entities      # one entity named "Mesh"
./cmd/sa list-assets        # the cube mesh in the catalog
```

In the editor this is **File ▸ Import...** (or the **Import...** button in the Assets
panel), then dragging the catalog tile onto an entity's Mesh field.

Position the cube so the camera you add next has something to frame. `set-transform` merges
only the fields you pass, leaving scale and rotation alone:

```sh
./cmd/sa set-transform Mesh --translation '{"x":0,"y":0,"z":0}'
```

> [!NOTE]
> Entity selectors accept the name or the numeric id. Rotation is Euler XYZ in radians on
> the wire (the inspector shows degrees). See
> [transform and matrices](../../explanations/scene-and-ecs/transform-and-matrices/).

## Light it

An unlit cube renders flat. Add a directional light; the engine shades through the first one
in the scene:

```sh
./cmd/sa create-entity Sun
./cmd/sa add-component Sun DirectionalLight
./cmd/sa set-light Sun --direction '{"x":-0.5,"y":-1,"z":-0.3}' --intensity 3 --color '{"x":1,"y":0.95,"z":0.9}'
```

`set-light` targets the entity you name, or the first directional light if you omit it. The
`ambient` field fills shadowed faces so nothing goes fully black:

```sh
./cmd/sa set-light Sun --ambient 0.2
```

In the editor this is **Create ▸ Directional Light**, then editing Direction / Color /
Intensity / Ambient in the Inspector. For a local look, **Create ▸ Point Light** drops a
`PointLightComponent`; point and spot lights are dynamic and get
[clustered](../../explanations/lighting-and-brdf/clustered-forward/) automatically.

## Give it a camera

The viewport draws through a fly-camera by default. A scene needs its own camera so the
render is reproducible. Add one back from the cube, looking down -Z:

```sh
./cmd/sa create-entity Main Camera
./cmd/sa add-component "Main Camera" Camera
./cmd/sa set-transform "Main Camera" --translation '{"x":0,"y":1,"z":5}'
```

The scene renders through the first camera whose `primary` flag is set, which a new
`CameraComponent` is by default. Confirm it:

```sh
./cmd/sa inspect "Main Camera"     # dumps every component as JSON
```

In the editor: **Create ▸ Camera**, then aim it with the gizmo. In edit mode, a camera shows
as a small camera model plus its frustum; both helpers are hidden while playing.

## Check it's drawing

Ask the renderer what it drew, then screenshot the viewport:

```sh
./cmd/sa render-stats               # draws=1  batches=1  instances=1  clustered=on
./cmd/sa screenshot viewport /tmp/first-scene.png
```

One draw call, one batch, and one instance is your cube. If `render-stats` shows
`instances=0`, the camera lost its primary flag or the entity lost its Mesh; re-run `inspect`
on each.

## Save the project

A project file bundles the asset catalog and the scene into one document, so reopening it
restores the cube, light, camera, and imported mesh:

```sh
./cmd/sa save-project /tmp/first-scene/project.json
```

In the editor this is **File ▸ Save Project**. Reload it any time:

```sh
./cmd/sa load-project /tmp/first-scene/project.json
```

`load-project` re-imports the catalog (so the mesh resolves to GPU buffers again) and
rebuilds every entity from JSON. It reads the same format the editor's menu writes.

## Next

- [Set a PBR material](../a-pbr-material/) — make the cube metallic, add a point light, tune exposure.
- [Write a custom Slang shader](../a-custom-slang-shader/) — draw the scene with your own shader.
- [Built-in components](../../explanations/scene-and-ecs/built-in-components/) — every component and what its fields mean.
- [sa CLI](../../explanations/tooling-and-control/sa-cli-protocol/) — how these commands reach the editor.
- [Project serialization](../../explanations/geometry-and-assets/project-serialization/) — what `save-project` writes.
