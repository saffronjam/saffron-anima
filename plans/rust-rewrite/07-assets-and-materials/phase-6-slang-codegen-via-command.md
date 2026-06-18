# Phase 6 — Slang codegen via `std::process::Command`

**Status:** COMPLETED

**Depends on:** 07-assets-and-materials:phase-5-node-graph-folding

## Goal

Port `find_slangc` (env → slang cache → PATH resolution) and the three runtime compile functions
(`compile_material_graph`, `compile_material_preview_shader`, `compile_material_mesh_shader`), each
writing the generated `.slang` then invoking `slangc` via `std::process::Command` (an argv, not a shell
string) and verifying the resulting `.spv`. The mesh variant splices `emit_graph_surface(mesh=true)`
into the runtime `mesh.slang` übershader between the `// @graph-begin` / `// @graph-end` markers and
passes `-I <shaders dir>`.

## Why this shape (NO LEGACY)

This is the area's one named NO-LEGACY simplification flagged by the feasibility study. The C++ builds a
single shell command string —
`"\"" + slangc + "\" \"" + path + "\" -profile … -o \"" + spv + "\" > /dev/null 2>&1"` — and runs
`std::system`. That is fragile (path-quoting bug surface, shell dependency, a `/dev/null 2>&1`
redirection baked into the string). The Rust port deletes the string entirely and uses
`Command::new(slangc)` with discrete `.arg(...)` calls for the flag set, `.stdout(Stdio::null())` +
`.stderr(Stdio::null())` for the silencing, and inspects `status.success()` + the `.spv` file existence.
No shell, no quoting, no manual redirection — there is one code path and it is the argv. The flag set
(`-profile glsl_450 -target spirv -emit-spirv-directly -fvk-use-entrypoint-name
-matrix-layout-column-major -o <spv>`, plus `-I <shaders dir>` for the mesh variant) matches the static
xtask shader flags for the overlapping flags (this crate's compiles are *runtime* per-material into the
project's `assets/materials/`, distinct from xtask's build-time static set — same tool, different driver,
no shared code path). `find_slangc` keeps the exact resolution order.

## Grounding (real files/symbols)

- `engine-old/source/saffron/assets/assets.cppm`: `findSlangc` (`SAFFRON_SLANGC` →
  `~/.cache/saffron-slang/slang/bin/slangc` → `slangc`), `compileMaterialGraph` (self-contained fragment
  → `materials/<uuid>.spv`), `compileMaterialPreviewShader` (studio-lit sphere → `_preview.spv`,
  `PreviewPush` matching `newPreviewPipeline`), `compileMaterialMeshShader` (read `mesh.slang`, splice
  between `// @graph-begin`/`// @graph-end`, `-I <shadersDir>`, `_mesh.spv`), the
  `"… > /dev/null 2>&1"` + `std::system` invocation being replaced.
- The AGENTS rule: "The mesh-shader path splices the generated surface body into the runtime `mesh.slang`
  between the `// @graph-begin` / `// @graph-end` markers and passes `-I <shaders dir>`… `findSlangc`
  resolves `slangc` from `SAFFRON_SLANGC`, then the slang cache, then `PATH`."

## Acceptance gate

- `cargo build -p saffron-assets` + workspace green; clippy + fmt clean.
- A `#[test]` (no `slangc` invocation) asserts the constructed `Command` argv for each of the three
  compile functions equals the expected `[slangc, <slang_path>, -profile, glsl_450, -target, spirv,
  -emit-spirv-directly, -fvk-use-entrypoint-name, -matrix-layout-column-major, -o, <spv_path>]` (+ `-I
  <dir>` for the mesh variant), and that no element contains shell quotes or redirection tokens (the
  shell-string is gone). `find_slangc` `#[test]` covers the env / cache-path / PATH-fallback order.
- A `#[test]` proving the mesh splice: given a `mesh.slang` stub with the two markers, the spliced source
  keeps the begin-marker line, drops the default body, inserts the emitted surface, and resumes at the
  end marker; a stub missing the markers returns a typed `Err`.
- An integration `#[test]` gated on `slangc` being present (else skipped + logged): compiling a small
  folded graph produces a non-empty `.spv` and a non-zero `slangc` exit returns `Error::SlangcFailed`.
