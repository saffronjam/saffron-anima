# Restructure plans — split the remaining monoliths

This is the continuation of the codebase-restructure effort that began with the renderer.
**Part 1 (the renderer) is DONE and committed** — `rendering/renderer.cppm` went from a
single 3669-line monolith to 11 focused files, and the `Renderer` god-struct was
decomposed into composed sub-structs. The three remaining parts each have their own file:

- **[part2-editor-app.md](part2-editor-app.md)** — `editor/source/main.cpp` → a thin stub + a new `Saffron.EditorApp` module.
- **[part3-control-split.md](part3-control-split.md)** — `control/control.cppm` (1071 lines) → an interface partition + impl units.
- **[part4-editor-split.md](part4-editor-split.md)** — `editor/editor.cppm` (911 lines) → an interface partition + impl units.

## The validated mechanism (READ FIRST — it differs from the original plan)
Splitting a module uses **two file kinds**, proven on the renderer:

1. **One interface partition** per module for the **shared types + ALL public function
   declarations** — `export module Saffron.<M>:<Name>;` (e.g. `:Types`, `:Context`,
   `:Command`). The primary unit `export import`s it. Goes in `FILE_SET CXX_MODULES`,
   listed **before** the primary.
2. **`.cpp` implementation units** for the function **definitions** — `module
   Saffron.<M>;` (no `export`, no partition colon, **`.cpp` extension**). These are
   regular `target_sources(<lib> PRIVATE …)`, **NOT** in the file set. They implicitly
   import the primary interface (so they see the partition's re-exported types) and
   explicitly `import` whatever else they call (Core for `Result`/`Ref`, sibling
   modules, etc.). Cross-unit calls between impl units resolve via the public decls in
   the interface partition. A purely-internal helper (used by one function) is
   **co-located** in the same `.cpp` as its caller (module linkage, defined before use).

> **Why not an interface partition per feature?** It triggers a flaky clang-21 + libc++
> `import std` **BMI-serialization ICE** (`ASTWriter::GenerateNameLookupTable`, `SIGBUS`).
> Impl units produce no BMI, so they can't trip it. Keep interface partitions to ~1–2 per
> module (the shared-types one, plus `:Detail` for shared internal helpers if needed).

See the `renderer-module-split` memory and the existing renderer files
(`engine/source/saffron/rendering/renderer_types.cppm` = the interface-partition example,
`renderer_drawlist.cpp` = the impl-unit example) for the exact, working syntax.

## Build + gate rules (non-negotiable)
- **Build `-j1`** in the toolbox — parallel builds intermittently `SIGBUS` on the ICE:
  `toolbox run -c saffron-build bash -lc 'cmake --preset debug && cmake --build build/debug -j1'`
  (run `cmake --preset debug` after every `CMakeLists.txt` change — the file set is explicit.)
- These are **pure source reorganizations — no behavior change.** The gate is: green
  build + the editor still runs. For renderer-affecting work use the byte-deterministic
  **cube pixel-gate** (move `project.json` aside → editor spawns the bundled cube;
  `SAFFRON_EXIT_AFTER_FRAMES=16 SAFFRON_CAPTURE=/tmp/x.png ./build/debug/bin/SaffronEditor`;
  `cmp` against a baseline). For editor/control work, build-green + a bounded headless run
  (`SAFFRON_EXIT_AFTER_FRAMES=5`) + drive the running editor with `se` (ping, render-stats,
  list-entities, screenshot) — and a manual UI smoke (hierarchy/inspector/gizmo/billboards/
  drag-drop/save-load) for the editor parts.
- **NEVER `rm -rf build/debug`** — it holds runtime-imported assets (baked `.smesh`/
  textures under `bin/assets/`) that are not in git. (A `rm -rf` already cost the imported
  "UE4" model; `project.json` still references it. Re-import from source, then repoint.)
- Conventions still bind (`CONVENTIONS.md`): `Result<T>`/`Err`, trailing-return for value
  fns, no ternary/inheritance/virtual, **minimal comments, no banner dividers, no
  change-journey notes** (don't add "moved from / was in" comments to relocated code),
  `///` doc only on exported decls. Commit style: conventional, **no AI attribution / no
  Co-Authored-By**. Build `-j1` after each step; one commit per part (or per impl unit).

## Suggested order
**Part 4 (editor)** and **Part 3 (control)** are independent source-only reorgs that keep
each module's public surface byte-identical — do them first, in either order. **Part 2
(EditorApp)** adds a new module + new DAG edges and depends on Editor/Control being stable,
so do it **last**. Each part is independent enough to be its own session.
