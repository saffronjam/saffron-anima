+++
title = 'Author a scene'
weight = 3
math = false
+++

# Author a scene

Create entities, add lights and a camera, and save the project from the CLI.

Start with an active project. In the editor, use the startup modal. From a shell,
`SAFFRON_PROJECT=<project-name>` is the simplest test path.

## Steps

1. Create an entity:
   ```sh
   se create-entity Floor
   ```
2. Give it a mesh from the catalog (`se list-assets` lists ids and names), then place it:
   ```sh
   se assign-asset Floor mesh cube
   se set-transform Floor --scale '{"x":10,"y":0.2,"z":10}'
   ```
   `set-transform` merges the passed fields over the current value. Rotation is Euler radians; every field is an `{x,y,z}` object.
3. Add a directional light:
   ```sh
   se create-entity Sun
   se add-component Sun DirectionalLight
   se set-light Sun --direction '{"x":-0.5,"y":-1,"z":-0.3}' --intensity 3
   ```
   For dynamic lights, use `add-component <entity> PointLight` or `SpotLight`.
4. Add a camera:
   ```sh
   se create-entity Camera
   se add-component Camera Camera
   ```
5. Tint a surface via its material:
   ```sh
   se set-material Floor --baseColor '{"x":0.8,"y":0.8,"z":0.8,"w":1}' --roughness 0.9
   ```
6. Save the active project (catalog + scene):
   ```sh
   se save-project
   ```

The editor offers the same operations: the **Create** menu, the in-viewport gizmo (W/E/R cycle translate/rotate/scale), and the Inspector.

## Verify

- Confirm the tree: `se list-entities`.
- Dump one entity: `se inspect Floor`.
- Screenshot it: `se screenshot viewport /tmp/scene.png`.
- Reload to confirm round-trip: `se open-project <project-name>` or `se load-project project.json`.

## In the code

| What | File | Symbols |
|---|---|---|
| Entities + components + transform | `control_commands_scene.cpp` | `create-entity`, `add-component`, `set-transform` |
| Lights + material | `control_commands_scene.cpp` | `set-light`, `set-material` |
| Assign catalog assets | `control_commands_asset.cpp` | `assign-asset` |
| Save / load project | `control_commands_asset.cpp` | `save-project`, `load-project` |

## Related

- [Built-in components](../../explanations/scene-and-ecs/built-in-components/)
- [Project serialization](../../explanations/geometry-and-assets/project-serialization/)
- [Picking](../../explanations/scene-and-ecs/picking/)
