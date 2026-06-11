+++
title = 'Author a material graph'
weight = 8
math = false
+++

# Author a material graph

Build a material's surface from a node graph — the way Unreal's material editor works — and watch a
preview sphere update as you wire it. For *how* a graph becomes a shader, see
[node-graph codegen](../../explanations/materials-and-pipelines/node-graph-codegen/).

## In the editor

1. Open the **Material** tool in the right sidebar. Pick a material from the dropdown, or click **New**.
2. Click **Graph**. A full-screen node canvas opens over a live preview sphere.
3. Add nodes from the left palette, grouped **input** (`Constant`, `Texture Slot`, `UV`), **math**
   (`Multiply`, `Add`, `Lerp`, `Saturate`, `Frac`, `Sin`, …), and **output** (`Material Output`).
4. Drag from a node's right (output) handle to another node's left (input) handle to wire them. Edit a
   `Constant`'s four values or a `Texture Slot`'s slot inline on the node.
5. Wire your result into **Material Output**'s `baseColor` (or `metallic`/`roughness`/`emissive`). Edits
   **auto-apply** (debounced) and re-render the preview — the surface morphs as you work.
6. **Compile** forces shader codegen for a procedural graph; **Close** returns to the panel.

A graph that is only constants and textures feeding the output **folds to params** and draws on the
shared übershader — no compile. A graph with math or procedural nodes **codegens** a per-material shader
that renders on the preview *and* on any entity the material is assigned to.

## From the CLI

The same operations are scriptable over the [`se` CLI](../drive-the-editor-from-the-cli/):

```sh
se material-create --name Rock
se material-set-graph --material Rock \
  --graph '{"nodes":[{"id":"c","type":"constant","props":{"value":[0.6,0.3,0.1,1]}},
                     {"id":"t","type":"textureSlot","props":{"slot":"albedo"}},
                     {"id":"m","type":"multiply"},{"id":"out","type":"materialOutput"}],
            "edges":[{"from":["c","rgba"],"to":["m","a"]},
                     {"from":["t","rgba"],"to":["m","b"]},
                     {"from":["m","rgba"],"to":["out","baseColor"]}]}'
se material-compile-graph --material Rock   # force codegen; { ok: true }
se material-assign --entity 12345 --material Rock
se material-cook                            # bake every codegen variant to disk
```

`material-set-graph` reports `foldable` (whether it avoided codegen). `material-cook` precompiles every
codegen material's übershader variant — run it after editing `mesh.slang` or before shipping.

## Related

- [Node-graph codegen](../../explanations/materials-and-pipelines/node-graph-codegen/) — fold vs codegen, the emitter, slangc
- [Native materials](../../explanations/materials-and-pipelines/native-materials/) — the `.smat` asset the graph lives on
- [Drive the editor from the CLI](../drive-the-editor-from-the-cli/) — the `se` command basics
