# Material & texture connectors (ambientCG + Poly Haven) → `.smat`

**Status:** IN PROGRESS

> Implementation notes:
> - Built and gated: engine + `src-tauri` `clippy -D warnings` clean; editor typechecks + oxfmt +
>   oxlint 0 errors; protocol tests (300) + assets tests (170) pass.
> - **ambientCG** connector (materials + HDRIs via the `full_json` API): material → a single zip of
>   PBR maps extracted (`extract_zip`) into a role-named folder; HDRI → a single file. A default
>   resolution is chosen per asset.
> - `kind` drives the host importer in `store_import` (bridge): `model` → `import-model`,
>   `material`/`texture` → `material-import`, `hdri` → `import-texture`.
> - **NO-LEGACY:** rather than a new `import-material-set` command, the existing `material-import` was
>   extended in place with optional `attribution` (mirrors the `import-model` decision) — one
>   material-folder import path, not two.
> - **Deviations / deferred:**
>   - Poly Haven texture/HDRI result mapping is deferred (its per-map files endpoint needs a
>     multi-map folder assembler); ambientCG covers the material + HDRI capability. Poly Haven still
>     contributes models.
>   - No interactive resolution picker yet — the connector imports at a default resolution (1K→2K→
>     first). The `StoreResult.resolution` is surfaced; the picker is a follow-up.
>   - No one-click "use as environment" for HDRIs (imported as a texture; wiring `environment.skyTexture`
>     is a follow-up).
> - **e2e run (final, per session goal):** the full suite was run to completion — **328 pass / 3 fail
>   across 88 files**. All connector tests pass (`store-import`, `store-config`, `store-material` — 13/13,
>   covering get/set-stores round-trip + reload, import-model ± attribution, material-import +
>   attribution, and `list-assets` surfacing attribution). The **3 failures are unrelated** — Vulkan
>   `VUID-VkWriteDescriptorSet-descriptorType-00331` validation errors in `morph.test.ts` from the
>   in-flight `better-animations` GPU morph-deform work (a buffer bound as `STORAGE_BUFFER` without the
>   usage flag); not touched by this plan.
> - **e2e sandbox note (reusable):** under the software GPU (llvmpipe) the host's swapchain present
>   stalls against headless weston, freezing the control loop (a raw `bun test` times out on *every*
>   render-dependent test, existing ones included). Running with `SAFFRON_EDITOR_NATIVE_VIEWPORT=1` +
>   `SAFFRON_VIEWPORT_SHM_SCENE`/`_ASSET` makes the host publish frames to shm instead of presenting,
>   which avoids the stall and lets the suite run headless. Worth baking into the e2e harness/`just e2e`.

## What this delivers and why

Phases 1–3 build the connector framework, credentials, and OAuth around the `model` import
path — a single deliverable file (`.glb`/`.gltf`) that runs straight through `bake_model`. This
phase finishes the other half of the [provider matrix](README.md): the `material`, `texture`, and
`hdri` kinds. These exercise a **different import path** than models, because the deliverable is not
one mesh file but a *set* of PBR maps (basecolor / normal / roughness / metallic / AO / height),
usually shipped as a multi-file zip at several resolutions, or a single HDRI environment map.

After this phase a user can search a CC0 material on **ambientCG**, pick a resolution, and import it
as a `.smat` with its maps wired onto the PBR slots, and import a **Poly Haven** HDRI as an
environment map — both immediately usable in the scene. The two keyless `none` connectors (Poly
Haven, ambientCG) gain their texture/HDRI result mapping here; their model results were already
covered in Phase 1.

The engine side is mostly reuse: `import_material_folder`
(`engine/crates/assets/src/manage.rs`) already scans a folder of maps, detects each role by filename
suffix (`detect_material_role`), registers each texture with the correct sRGB/linear colorspace
(`AssetServer::register_texture_bytes`), and writes a `.smat` via `save_material_asset`. The
`MaterialImportResult` it returns is the catalog handle. HDRIs reuse the existing `import-texture`
control command path (`AssetServer::import_texture`, which already recognises `.hdr` bytes), so an
HDRI is just a texture the scene's `environment.sky_texture` can reference.

The new work is: a **second host-side import command** for map sets that the `StoreResult.kind`
discriminator selects (model → the Phase-1 `import-model`/`bake_model` path; material/texture →
this one), connector result-mapping for the texture/HDRI deliverables, resolution selection in the
Store UI, and zip extraction for the multi-file material downloads.

## How `kind` selects the import path

The canonical `StoreResult.kind` (`model | hdri | material | texture`, defined in the
[README schema](README.md#canonical-normalized-result-schema)) is the single discriminator. The
editor downloads the deliverable and dispatches:

| `kind` | Deliverable | Host command | Engine path |
|--------|-------------|--------------|-------------|
| `model` | one `.glb`/`.gltf` (Phase 1) | `import-model` | `bake_model` → `.smodel` |
| `material`, `texture` | `texture-zip` of PBR maps | **`import-material-set`** (new) | `import_material_folder` → `.smat` |
| `hdri` | one `.hdr` | existing `import-texture` | `import_texture` → texture catalog entry |

There is exactly **one** command per deliverable shape; do not add a per-provider import command.

## Tasks (ordered)

### 1. Connector result-mapping for textures & HDRIs (`editor/src-tauri`, Rust)

Extend the two `none` connectors added in Phase 1 to also emit `material` / `texture` / `hdri`
`StoreResult`s, filtering to importable deliverables at the mapping step (per the README
*filter-to-importable* rule — never a generic post-filter).

- **ambientCG** — `https://ambientcg.com/api/v2/full_json`, keyless GET with the connector's unique
  `User-Agent`. Map each asset's downloadable map zips to a `StoreResult` with `kind: "material"`
  (PBR materials) or `kind: "hdri"` (HDRI assets), `license` = `{ id: "cc0", requiresAttribution:
  false, url: <CC0 url> }`, `resolution` populated from the zip variant, and `importDescriptor`
  `{ format: "texture-zip", ref: <download url for the chosen resolution> }`. Emit one result per
  asset; the *available resolutions* travel as an extra connector-defined field on the descriptor so
  the UI (task 3) can offer them without a second search round-trip.
- **Poly Haven** — `https://api.polyhaven.com`, keyless GET with the connector's unique `User-Agent`.
  Map PBR-texture assets to `kind: "texture"` (map sets, `texture-zip` descriptor) and HDRI assets to
  `kind: "hdri"` (a single `.hdr`/`.exr` deliverable). All Poly Haven assets are CC0
  (`requiresAttribution: false`). Use the Poly Haven `files` endpoint to resolve the per-resolution
  download URL for the descriptor.
- Both connectors expose available resolutions in the result so the UI can pick before
  `download()`. The connector's `download()` (per the Phase-1 trait) fetches the chosen-resolution
  artifact to a local file and, for a `texture-zip`, **extracts it into a temp folder** of role-named
  map files, returning that folder path (the host importer consumes a *folder*, matching
  `import_material_folder`). Use a maintained zip crate added to the `src-tauri` manifest
  (`editor/src-tauri/Cargo.toml`); errors flow through the connector's `thiserror` error enum
  introduced in Phase 1 (no stringly `Result<T, String>`).

### 2. Host-side `import-material-set` control command (`saffron-control` + `saffron-protocol`)

Add **one** new control command that maps a downloaded map-set folder onto a `.smat`, distinct from
the model `import-model` command.

- **DTOs** in `engine/crates/protocol/src/dto.rs`: `ImportMaterialSetParams { path: String, name:
  String, license: StoreLicense, source_url: String, author: String }` and a result reusing the
  existing material catalog handle shape (mirror `MaterialImportResult` →
  the existing material-import result DTO; do not invent a parallel handle type). Derive
  `serde(rename_all = "camelCase")` + `JsonSchema` + `TS` like every DTO, preserving field order.
  Add a `StoreLicense` DTO (`id`, `requiresAttribution`, `url`) matching the README schema — shared
  by the Phase-1 `import-model` command too, so define it once and reuse.
- **`COMMANDS` table** entry in `engine/crates/protocol/src/command.rs` (`import-material-set`,
  kebab-case, one-line summary, param/result type-name strings) plus a matching
  `COMMAND_FIXTURES`/`COMMAND_SKIPS` row (a fixture, since this is a real importable command).
- **Handler** registered once in `engine/crates/control/src/commands_asset.rs` via the
  `register::<P, R>` pattern (alongside the existing `material-import` registration). It calls
  `ctx.renderer.with_gpu_uploader(...)` then `import_material_folder(assets, gpu, &path, &name)`,
  and writes the structured `license` + `source_url` + `author` onto the catalog asset's metadata
  so attribution follows the asset (per the README *attribution captured at import* rule). This is
  the single seam where CC-BY/Sketchfab-style attribution is persisted for material sets.
- **NO LEGACY:** the existing `material-import` command imports from a *local folder a user picked*.
  `import-material-set` is the connector path (downloaded folder + structured license). If the
  attribution metadata write means `material-import` should carry the same fields, fold it in and
  update its one caller — do **not** leave two divergent material-folder import paths. Decide and
  document which command survives; there is exactly one way to import a material set.
- Regenerate the wire artifacts: `cargo run -p xtask -- gen-protocol` (updates
  `editor/src/protocol/sa-types.ts`, `schemas/control/openrpc.generated.json`, the manifest, and the
  Luau defs). The editor then gets typed access through `CommandParamsMap` / `CommandResultMap`.

### 3. Resolution selection + import wiring in the Store UI (`editor/src`)

- In the Store result card / detail view (the `StoreWorkspace` added in Phase 1), when a result's
  `kind` is `material` / `texture` / `hdri` and it carries multiple resolutions, render a resolution
  picker (a shadcn/ui `Select`) defaulting to a sensible mid resolution. The picked resolution
  selects which `importDescriptor.ref` the import uses.
- On import, dispatch by `kind`: `material` / `texture` → `client.importMaterialSet({ path, name,
  license, sourceUrl, author })` after the connector `download()` returns the extracted map folder;
  `hdri` → the existing `import-texture` client method after downloading the `.hdr`. Route every
  control-call failure through `notifyError(errorText(err))` (`editor/src/lib/flash.ts`), the sole
  user-facing error surface.
- Add a typed wrapper method on the `client` object in `editor/src/control/client.ts`
  (`importMaterialSet(params): Promise<...> { return call("import-material-set", params); }`),
  matching the generated `CommandParamsMap`/`CommandResultMap` entries.
- After an HDRI import, surface a one-click "Use as environment" affordance that sets
  `environment.skyTexture` to the new texture (reuse the existing environment-texture wiring referenced
  in `engine/crates/control/src/commands_asset.rs`), so the HDRI is immediately usable in the scene.

### 4. `sa` CLI: import a material set / HDRI

The connector list/search/import `sa` commands are introduced in Phase 1. This phase only needs the
new command to be reachable from the shell — which it is automatically, because `sa` discovers
commands from `saffron_protocol::COMMANDS` (`engine/crates/sa/src/main.rs`). Verify
`sa import-material-set --path <dir> --name <n>` and the existing `sa import-texture <hdr>` both work
end to end, and that `sa store import <result-id>` (Phase 1) dispatches material/texture results to
`import-material-set` by `kind`. No second registration — one `COMMANDS` entry covers CLI + editor.

### 5. e2e coverage (`tests/e2e`, bun/TypeScript)

Add a behaviour test driving the headless host over the control plane (typed via
`@saffron/protocol`): place a small fixture folder of role-named PBR maps (basecolor/normal/
roughness — reuse or extend the material fixtures already in the tree), call `import-material-set`,
and assert the returned material handle, that `list-assets` shows the `.smat`, that the structured
license/attribution landed on the catalog entry, and a validation-clean log. Add an HDRI case via
`import-texture` with a tiny `.hdr` fixture. The connector HTTP itself lives editor-side and is not
exercised here; the test targets the host command contract.

### 6. Docs: asset-connectors explanation page + hub row

Per the AGENTS.md keep-docs-current rule, this phase lands the `docs/` explanation page for the
**asset-connectors** concept (the connector model, `auth_kind`, the canonical `StoreResult` schema,
and the `kind`-driven import split: `model` → `.smodel` via `bake_model`, `material`/`texture` →
`.smat` via `import_material_folder`, `hdri` → texture/environment). Add the matching row to the
section hub `_index.md`. Use the house style: TOML front matter, `title` equal to the body `# H1`, a
slim `What | File | Symbols` code-pointer table (cite `import_material_folder`, `bake_model`,
`save_material_asset`, the `import-material-set` command — symbols, not line numbers), lead with the
concept and why, and run the prose through the humanizer pass. If Phases 1–3 already created the
page, extend it with the material/texture/HDRI section rather than duplicating it (one page per
concept).

## Done when

- [ ] ambientCG and Poly Haven connectors emit `material` / `texture` / `hdri` `StoreResult`s
      conforming to the canonical README schema (structured `license`, `kind`, available resolutions
      on the descriptor), filtered to importable deliverables at the mapping step.
- [ ] The Store UI offers a resolution picker for multi-resolution assets and dispatches import by
      `kind`; texture-zip downloads are unzipped into a role-named map folder before handoff.
- [ ] `import-material-set` exists as exactly one new control command (DTOs in
      `saffron-protocol`, `COMMANDS` entry with a fixture, single `register` in `saffron-control`),
      runs `import_material_folder` → `.smat`, and writes structured license + source + author onto
      the catalog asset; `cargo run -p xtask -- gen-protocol` regenerated and committed-to-tree
      artifacts updated. No duplicate/legacy material-folder import path remains.
- [ ] Searching a CC0 material on ambientCG, picking a resolution, and importing yields a `.smat`
      with maps wired onto the PBR slots, usable in the scene; importing a Poly Haven HDRI yields an
      environment-usable texture (`environment.skyTexture`).
- [ ] `sa import-material-set` and `sa store import` (material/texture by `kind`) work from a shell;
      no second registration was added.
- [ ] e2e test in `tests/e2e` imports a fixture map set + HDRI over the control plane, asserts the
      handles, the persisted attribution metadata, and a validation-clean log.
- [ ] The `docs/` asset-connectors explanation page and its hub `_index.md` row cover the
      `kind`-driven material/texture/HDRI import path.
- [ ] **Milestone gate:** `just engine` then `just prepare-for-commit` (format + lint) both pass;
      every warning the change raises is fixed.
- [ ] **Git stays read-only:** the work is left **unstaged** for the user — do not commit, stage, or
      push.
