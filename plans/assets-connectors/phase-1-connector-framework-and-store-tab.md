# Connector framework + Store tab (keyless vertical slice)

**Status:** IN PROGRESS

> Implementation notes (paused for user changes):
> - Built and gated: engine + `src-tauri` compile and are `clippy -D warnings` clean; editor
>   typechecks, oxfmt-clean, oxlint 0 errors. Protocol regenerated via `xtask gen-protocol`.
> - `import-model` was extended in place with an optional `attribution` (`ImportModelParams` +
>   `AssetAttributionDto`); attribution is stored on `AssetEntry` and persisted by
>   `catalog_to_json`/`catalog_from_json`. (Surfacing it via `list-assets` is deferred to Phase 3.)
> - **Deviation:** the `sa store` CLI stub (Task 10) was intentionally NOT added — connector
>   search/import is editor-local Tauri state with no engine-side state in Phase 1, so a stub would
>   be a non-functional shim. `sa import-model` (with attribution) is already CLI-reachable. The
>   enabled-set becomes engine state in Phase 2 (`stores` block) and gets `sa` visibility there.
> - **Not yet run (deferred per session goal):** the e2e suite and the `just engine` headless smoke.
>   The Phase 1 e2e test is written at `tests/e2e/store-import.test.ts`.

## What this phase delivers

A working, end-to-end Store **without any auth**: enable the keyless Poly Haven connector
(hardcoded on for now), type a query in the existing `AnimaSearchbar`, press Enter, and see merged
results with thumbnails, license badge, and a source badge. Scroll to load more — each source keeps
its own cursor and reports its own exhaustion — then click *Import* and the model lands in the asset
catalog and the scene.

This is the foundational vertical slice that every later phase plugs into:

- The **`auth_kind`-keyed `StoreConnector` trait** + the canonical `StoreResult` shape (Rust struct
  + the ts-rs/serde wire type the webview consumes), per the README's *Canonical normalized result
  schema*.
- The **connector registry** in `src-tauri` and the Tauri commands the webview calls to run a
  global search and to trigger an import.
- The **aggregator**: fan the query out to all enabled connectors, hold **per-source cursors**,
  **round-robin merge**, track **per-source exhaustion**, and feed virtual scroll. No pagination, no
  fake global relevance sort.
- The **Store main tab** (`ViewTab` variant + `openStoreTab` + `App.tsx` dispatch + a Topbar
  button), wired to `AnimaSearchbar` with `provider:` / `type:` chips that fire **only on Enter and
  chip commit**.
- The **first keyless connector — Poly Haven** (`https://api.polyhaven.com`, unique `User-Agent`,
  CC0), as the proof. ambientCG (also `auth_kind: none`) is added in Phase 4 when texture/HDRI
  import lands; this phase mints the framework Poly Haven proves out.
- The **import path**: editor `download()`s the glTF/GLB to a local path, then calls a **new control
  command** that runs the existing `saffron-assets` model importer to produce a `.smodel` catalog
  entry.
- An **`sa` CLI** stub for connector search/import and a **docs** page.

Scope boundary (deferred, per README): no keyring, no `api_key`/`oauth_loopback` connectors, no
`project.json` `stores` block, no first-open onboarding — Poly Haven is simply enabled by default.
Those arrive in Phases 2–3. Material/texture/HDRI mapping to `.smat` is Phase 4; this phase emits
only `kind: model` results from Poly Haven (the *filter-to-importable* mapping step drops the rest).

## Tasks (ordered)

### 1. HTTP + connector dependencies in `src-tauri`

- Add an async HTTP client (`reqwest` with `rustls` TLS, `json` feature) and `serde`/`serde_json` to
  `editor/src-tauri/Cargo.toml`. There is **no outbound HTTP anywhere in the engine** and there must
  not be — connectors are editor-side only, so this dependency lives solely in `src-tauri`, never in
  an `engine/crates/*` crate.
- Pin versions in the editor `src-tauri` manifest (the engine workspace `[workspace.dependencies]`
  does not cover `src-tauri`).

### 2. The `StoreConnector` trait + normalized `StoreResult` (Rust side)

- New module `editor/src-tauri/src/connectors/mod.rs` (registered from
  `editor/src-tauri/src/lib.rs`). Define:
  - `enum AuthKind { None, ApiKey, OauthLoopback }` — the single switch the framework branches on.
    Phase 1 only constructs `None`.
  - The normalized result types mirroring the README schema **exactly**, with
    `#[derive(Serialize, Deserialize)]` and `#[serde(rename_all = "camelCase")]` so the webview
    consumes them verbatim:
    - `enum StoreKind { Model, Hdri, Material, Texture }`
    - `struct StoreLicense { id, requires_attribution, url }`
    - `struct StoreRef { id, display_name }`
    - `struct StoreImportDescriptor { format, ref_ }` (`#[serde(rename = "ref")]` on the handle)
    - `struct StoreResult { id, store, kind, name, author, thumbnail_url, source_url, license,
      import_descriptor, published_at, updated_at, tri_count, file_size, resolution, tags }` with the
      optional fields `#[serde(skip_serializing_if = "Option::is_none")]`.
  - `struct SearchPage { results: Vec<StoreResult>, next_cursor: Option<String>, exhausted: bool }`.
  - `struct StoreCursor(String)` — opaque, connector-defined (offset, page token, etc.).
  - The trait:
    ```
    #[async_trait]
    trait StoreConnector: Send + Sync {
        fn id(&self) -> &'static str;
        fn display_name(&self) -> &'static str;
        fn auth_kind(&self) -> AuthKind;
        async fn search(&self, query: &SearchQuery, cursor: Option<StoreCursor>) -> Result<SearchPage, ConnectorError>;
        async fn download(&self, descriptor: &StoreImportDescriptor) -> Result<PathBuf, ConnectorError>;
    }
    ```
  - `struct SearchQuery { text: String, kind: Option<StoreKind> }` — `text` from `freeText`, `kind`
    parsed from the `type:` chip; `provider:` chips scope *which* connectors run (handled by the
    aggregator, not passed into a connector).
- Add a per-module `thiserror` enum `ConnectorError` (e.g. `Http`, `Decode`, `UnsupportedFormat`,
  `Download`, `NotFound`) — **no stringly `Result<T, String>`**. `download()` writes into a temp dir
  under the editor app-data/cache dir and returns the local path the host will read.

### 3. The Poly Haven connector

- New `editor/src-tauri/src/connectors/polyhaven.rs` implementing `StoreConnector` with
  `id() == "polyhaven"`, `display_name() == "Poly Haven"`, `auth_kind() == AuthKind::None`.
- Every request sends a **unique `User-Agent`** header (e.g. `saffron-anima/<crate-version>
  (+https://...)`) — Poly Haven requires it.
- `search`: query the Poly Haven asset listing (`GET https://api.polyhaven.com/assets?type=models`
  and the per-asset `GET /info/<id>` / `GET /files/<id>` endpoints as needed), filter client-side by
  `query.text`, and **map at the result step to `StoreResult`**:
  - `kind: Model` only (Poly Haven HDRIs/textures are Phase 4) — this is the *filter-to-importable*
    step: results without a glTF deliverable are simply never emitted.
  - `license: { id: "cc0", requires_attribution: false, url: "https://creativecommons.org/publicdomain/zero/1.0/" }`
    (Poly Haven assets are CC0).
  - `import_descriptor: { format: "gltf", ref_: <glTF file url> }` from the asset's exposed glTF
    variant.
  - `thumbnail_url` / `source_url` / `author` / `tags` from the API payload; `published_at` from the
    asset's `date_published` if present, else `None`.
  - The cursor is a simple offset into the (locally cached) listing; set `exhausted` when the slice
    reaches the end.
- `download`: `GET` the `import_descriptor.ref_` glTF/GLB to the temp dir, returning the `.glb`/`.gltf`
  path (glTF with external buffers/images: fetch the sidecar files the manifest references into the
  same dir so the host importer resolves them, or prefer the self-contained `.glb` variant when Poly
  Haven exposes one).

### 4. The registry + aggregator

- `editor/src-tauri/src/connectors/registry.rs`: a `ConnectorRegistry` holding
  `Vec<Arc<dyn StoreConnector>>` (read-shared handles are `Arc<T>`). Phase 1 constructs it with Poly
  Haven enabled by default (Phase 2 makes the enabled set per-project). Expose
  `enabled() -> &[Arc<dyn StoreConnector>]` and `by_id(id) -> Option<Arc<dyn StoreConnector>>`.
- `editor/src-tauri/src/connectors/aggregator.rs`: a `SearchSession` keyed by the search query that
  holds **per-source state** `{ connector: Arc<dyn StoreConnector>, cursor: Option<StoreCursor>,
  exhausted: bool, buffer: VecDeque<StoreResult> }`:
  - `next_batch(n)`: for each non-exhausted source whose buffer is low, `connector.search(query,
    cursor)`, refill its buffer, advance its cursor, set `exhausted` from the page; then **round-robin
    interleave** one result at a time across sources into the returned batch. Per-source errors are
    surfaced but do not abort the batch (one connector failing must not blank the whole Store).
  - `provider:` chips scope the active source set for the session; `type:` maps to
    `SearchQuery.kind`.
  - **No pagination and no global relevance sort** — the only ordering is round-robin interleave; the
    UI scroll position drives how many `next_batch` calls happen.

### 5. Tauri commands the webview calls

- In `editor/src-tauri/src/lib.rs`, register `#[tauri::command]` handlers and add them to the
  `invoke_handler` generate list (these are **editor-local Tauri commands**, distinct from the
  engine control plane — only the *import* crosses to the host in Task 6):
  - `store_search_session(query) -> SessionId` — start/replace the session for a query (called on
    Enter / chip commit). Returns a session handle; the session lives in Tauri managed state
    (`tauri::State`).
  - `store_search_more(session, count) -> { results: StoreResult[], exhausted: bool }` — the
    virtual-scroll driver; pulls the next round-robin batch and reports whether *all* sources are
    exhausted.
  - `store_list_connectors() -> { id, displayName, authKind, enabled }[]` — for the UI.
  - `store_import(result) -> ImportedAsset` — `download()` the descriptor to a local path, then call
    the host control command from Task 6 over the existing control-plane connection in `src-tauri`
    and return the resulting catalog id/name to the webview.
- All Tauri-command errors return a serializable error type (map `ConnectorError` to a string payload
  at this boundary only — the internal APIs stay typed).

### 6. The host-side import control command (control plane)

The engine `import-model` command (`commands_asset.rs`, registered via
`reg.register::<PathParams, ImportModelResult>("import-model", …)`) already takes a local file path,
runs `AssetServer::import_model` (which calls geometry's `translate_model` → `bake_model` in
`engine/crates/assets/src/import.rs`), and returns `ImportModelResult { id, name, type }`. The
editor-side `download()` produces exactly the local path that command wants, and host + editor share
the filesystem.

Per **NO-LEGACY / one-way-to-do-each-thing**, do **not** add a second importer command that
duplicates `import-model`. Instead, **extend the existing import path to capture attribution** so the
README invariant (*attribution captured at import — license/author/sourceUrl stored on the catalog
asset*) holds for connector imports:

- Add an **optional** `attribution` field to the import params DTO in
  `engine/crates/protocol/src/dto.rs` (a new `AssetAttributionDto { license_id, requires_attribution,
  license_url, author, source_url, store_id }`, all `Option`/defaulted so a plain local import omits
  it). Reuse `PathParams` by replacing it for `import-model` with a params struct that carries
  `path` + `Option<AssetAttributionDto>` — and update the single existing `import-model` caller and
  the `sa`/manifest entries together (no parallel command).
- Thread the attribution onto the baked asset's metadata so it travels with the asset: the
  `ContainerMetadata` `import`/`META` chunk in `engine/crates/assets/src/model.rs` and/or the
  `AssetEntry` catalog row in `engine/crates/scene/src/environment.rs`. (Adding a metadata field is
  in-scope; wiring `download()` for material/texture sets is Phase 4.)
- Register stays **one registration** — edit the existing `reg.register::<…>("import-model", …)` in
  `engine/crates/control/src/commands_asset.rs` to read the new param and store the attribution.
- Update the `COMMANDS` table entry / fixture for `import-model` in
  `engine/crates/protocol/src/command.rs` for the changed params type, then regenerate the wire
  artifacts: `cargo run -p xtask -- gen-protocol` (emits `editor/src/protocol/sa-types.ts`,
  `schemas/control/openrpc.generated.json`, the command manifest, and the Luau defs). The editor then
  gets typed access through `editor/src/control/client.ts` via `CommandParamsMap`/`CommandResultMap`.

### 7. The Store main tab (editor React)

- **`ViewTab` union** in `editor/src/state/store.ts`: add `| { id: string; kind: "store"; title:
  string; closable: true }`, and an `openStoreTab()` action mirroring `openFlameTab()` (id
  `"store"`, append if new, set `activeViewTabId`).
- **`App.tsx`**: in the `activeKind` dispatch, add `{activeKind === "store" && <StoreWorkspace />}`.
- New `editor/src/panels/StoreWorkspace.tsx` (or `editor/src/store/StoreWorkspace.tsx`): hosts the
  `AnimaSearchbar`, the results grid, and the import action.
- **`Topbar.tsx`**: add a Store button (e.g. a `Store`/`ShoppingBag` lucide icon, `size="icon-sm"`
  `variant="ghost"`) in the right-hand Tools group — **left of the Tools (Wrench) button**, as the
  user requested — calling `openStoreTab()`.

### 8. Wire `AnimaSearchbar` (Enter-only, chips)

- In `StoreWorkspace`, render `AnimaSearchbar` (`editor/src/components/anima/AnimaSearchbar.tsx`)
  with two `ChipConfig`s:
  - `provider:` — `options()` from `store_list_connectors()` (Phase 1: just Poly Haven), scopes the
    active sources.
  - `type:` — fixed options `model | hdri | material | texture` mapping to `SearchQuery.kind`.
- **Treat the searchbar `SearchState` as local component state.** Dispatch a connector query **only**
  on Enter (`AnimaSearchbar` emits via `onChange` on Enter) and on **chip commit** — never on live
  keystrokes. Set `debounceMs={0}` and do **not** call the network in any `onChange` for free-text
  edits; the handler must distinguish a committed search (Enter/chip) from incidental state changes.
- On a committed search, call `store_search_session(query)`, reset the grid, and load the first
  batch via `store_search_more`.

### 9. Results grid: thumbnails, badges, virtual scroll, import

- New `editor/src/store/StoreResultsGrid.tsx`: a windowed/virtualized grid following the
  `ScriptLogsPanel` (`editor/src/panels/ScriptLogsPanel.tsx`) approach — track `scrollTop`/viewport
  height, compute the visible slice, render only visible cards as `React.memo` rows positioned
  absolutely.
- Each card shows the provider `thumbnailUrl` in an `<img>`, the `name`/`author`, a **license badge**
  (CC0 / CC-BY) from the structured `license`, and a **source badge** from `store.displayName`, plus
  a "view on site" link to `sourceUrl`.
- **Infinite scroll**: when the user nears the end, call `store_search_more(session, n)` to pull the
  next round-robin batch; stop when the command reports all sources `exhausted`. No page controls.
- The card's **Import** button calls `store_import(result)`; on success route through the existing
  catalog refresh so the asset appears in the catalog and can be instantiated into the scene. Surface
  failures via `notifyError(errorText(err))` (`editor/src/lib/flash.ts`), the project's sole
  user-facing error channel.

### 10. `sa` CLI stub

- The connector framework is editor-side, but the **import** is a control command, so the running
  editor stays scriptable. Add `sa` coverage for the connector surface:
  - Since `import-model` now carries optional `attribution`, the existing `sa import-model <path>`
    keeps working (attribution omitted) — confirm the CLI still coerces the changed params via
    `build_params()` in `engine/crates/sa/src/main.rs` (it auto-discovers from `COMMANDS`, so the
    regen in Task 6 is enough).
  - The connector *search/list* surface is editor-local Tauri state, not a control command, so add a
    thin `sa store …` subcommand stub (e.g. `sa store search`, `sa store import`) that documents the
    intent and, for `import`, forwards a downloaded path to `import-model`. Keep it minimal — full
    connector-over-CLI search is out of scope until the framework can run headless.

### 11. Docs

- Add a docs explanation page under `docs/content/explanations/` (a new
  `assets-and-connectors/` section or alongside `geometry-and-assets/`) describing the Store /
  connector concept: `auth_kind`, the normalized `StoreResult` schema, per-source-cursor
  round-robin aggregation, filter-to-importable, and the download → `import-model` path. Add the row
  to the section hub `_index.md`. Follow the docs house style (sentence-case noun-phrase title equal
  to the body `# H1`, a slim `What | File | Symbols` pointer table, humanizer pass).

## Done when

- [ ] Poly Haven is enabled by default; opening the Store tab from the new Topbar button (left of the
      Wrench) shows the `AnimaSearchbar`.
- [ ] Typing a query and pressing **Enter** (or committing a `provider:` / `type:` chip) runs the
      search; **no** network call fires on keystroke/`onChange`.
- [ ] Results render in the virtualized grid with thumbnails, a structured-license badge, and a
      source (store) badge.
- [ ] Scrolling to the end pulls more results per source (round-robin), each source advancing its own
      cursor and reporting its own exhaustion; the grid stops only when all sources are exhausted —
      no pagination, no global relevance sort.
- [ ] Clicking **Import** downloads the glTF/GLB locally and calls the (one) `import-model` control
      command, which bakes a `.smodel` and a catalog entry via `AssetServer::import_model` /
      `bake_model`; the model appears in the catalog and can be placed in the scene, with its CC0
      license/author/source recorded on the asset metadata.
- [ ] The connector trait, registry, aggregator, Poly Haven connector, and `ConnectorError` are
      idiomatic Rust (per-crate `thiserror` enum, `Arc<dyn StoreConnector>`, `?` propagation) with no
      stringly errors and no compat shims; `import-model` was *extended in place* (no duplicate
      command).
- [ ] `cargo run -p xtask -- gen-protocol` was run; `editor/src/protocol/sa-types.ts`,
      `schemas/control/openrpc.generated.json`, and the manifest are regenerated and the editor calls
      the changed command through `client.ts` with `CommandParamsMap`/`CommandResultMap` types.
- [ ] The `sa store` subcommand stub and the changed `sa import-model` work against a running host.
- [ ] An e2e test in `tests/e2e` covers the changed `import-model` command (baking from a local path,
      with and without attribution), asserting a validation-clean host log.
- [ ] A docs page + hub `_index.md` row describe the connector concept.
- [ ] **Milestone gate:** `just engine` then `just prepare-for-commit` (format + lint) pass with every
      warning this change raised fixed.
- [ ] **Git left unstaged** — do not commit, stage, or push; leave the work for the user to review.
