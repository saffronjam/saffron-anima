# Credentials + per-project enablement + API-key connector

**Status:** IN PROGRESS

> Implementation notes:
> - Built and gated: engine + `src-tauri` `clippy -D warnings` clean; editor typechecks +
>   oxfmt + oxlint 0 errors; `saffron-assets` tests pass (incl. the `stores` round-trip).
> - `stores` block added to `project.json` via `ProjectSidecar.stores` (save/load/reload), mirroring
>   the `debug_overlays` sidecar precedent; held in `SceneEditContext.stores`; read/written over the
>   control plane by new `get-stores` / `set-stores` commands (`ProjectStoresDto { enabled }`).
> - Credentials: `editor/src-tauri/src/connectors/credentials.rs` (keyring, service `saffron-anima`)
>   with the headless fallback (`SAFFRON_NO_KEYRING`, `SAFFRON_SECRET_<ID>`, in-memory). Tauri
>   commands `connector_set_secret` / `_clear_secret` / `_secret_status` (status returns a bool only).
> - Poly Pizza `api_key` connector + shared `ApiKeyField` (onboarding + Settings "API & Secrets").
> - **Design choice:** the enabled-set is host state (per-project), credentials are machine-local;
>   the editor passes the enabled set as the search `providers` scope (the bridge holds no per-project
>   state). `store_list_connectors` lists *available* connectors; enablement comes from `get-stores`.
> - **Deviation:** no dedicated `sa connectors` command — `sa get-stores` is auto-exposed and reports
>   the enabled set; `auth_kind` is editor-side only, so it is not surfaced over the control plane.
> - **Not yet run (deferred per session goal):** the e2e suite. The Phase 2 e2e test is written at
>   `tests/e2e/store-config.test.ts`.

## What this phase delivers

Phase 1 stood up the connector framework, the aggregator, the first `none` connector (Poly Haven),
the Store `ViewTab`, and the host-side `import-model` control command. Everything so far
is keyless and ephemeral: nothing is remembered about which connectors a project uses, and there is
no place to hold a credential. This phase adds the two persistence layers the design calls for and
the first connector that needs them:

1. **A credential layer in `editor/src-tauri`** using the `keyring` Rust crate (Secret Service on
   Linux), keyed by connector id — **machine/user-global, never per-project** — with a headless / CI
   fallback so the `just e2e` host boot never breaks.
2. **A per-project `stores` block in `project.json`** — a new top-level block alongside
   `renderSettings` / `editorCamera` / `debugOverlays` — holding the **enabled connector ids** plus
   non-secret per-project config. This is the shared, committed part: a teammate who opens the
   project sees the same enabled stores.
3. **First-open onboarding** — opening the Store the first time for a project prompts the user to
   choose which connectors to enable for *this* project; each enable runs that connector's add-flow
   (`none` → instant; `api_key` → paste-key field).
4. **The `api_key` connector: Poly Pizza** — a paste-and-store key in the keyring and a connector
   that reads it at search / download time, giving the user a third live source (low-poly models,
   all of Quaternius, CC0 / CC-BY GLB).

The split is the whole point: **enabled-set travels in `project.json` (shared, committed); secrets
live in the keyring (machine-local, never committed)**. A teammate re-enters their own Poly Pizza
key; the enabled-set is already there.

This phase depends on Phase 1's connector trait + `auth_kind` switch and the `import-model` command.
It does not touch the OAuth machinery — that is Phase 3's `oauth_loopback` capability, and the
`none` / `api_key` connectors must never reach into it.

## Tasks

### 1. Add the `keyring` crate + a credential module in `src-tauri`

The editor's `src-tauri` currently configures only `tauri-plugin-dialog` (see
`editor/src-tauri/Cargo.toml`). Add the `keyring` crate as a direct dependency (not the Tauri
plugin — we want the Rust API directly in the bridge, keyed by our own ids).

- Add `keyring` to `editor/src-tauri/Cargo.toml` (Secret Service backend on Linux; declare the
  Linux build dependency `dbus-devel pkgconf-pkg-config` in the docs/build note for this phase).
- Create `editor/src-tauri/src/credentials.rs` (a new module wired into
  `editor/src-tauri/src/lib.rs`) exposing a small façade over the keyring:
  - A `Credentials` accessor that maps a connector id to a keyring entry under a fixed service name,
    e.g. service `"saffron-anima"`, account `<connector-id>` (so the stored key is conceptually
    `saffron-anima/poly-pizza`, matching the README's `saffron-anima/sketchfab` example).
  - `set_secret(connector_id, secret)`, `get_secret(connector_id) -> Option<String>`, and
    `delete_secret(connector_id)`.
  - A per-module `thiserror` error enum (`CredentialError`) — **no `Result<T, String>`** — wrapping
    `keyring::Error` and the backend-selection outcome; propagate with `?`. Map it to the Tauri
    command error type at the bridge edge.

### 2. Headless / no-Secret-Service fallback (the gotcha to plan for)

**Gotcha:** the `saffron-build` toolbox and CI have no D-Bus / Secret Service daemon, so a keyring
call throws. The `just e2e` suite boots a headless host (and the editor bridge alongside in some
flows); a hard keyring dependency at startup would break that boot. Design the fallback so the
keyring layer degrades cleanly instead of panicking.

- In `credentials.rs`, select the backend **once at module init**:
  1. If `SAFFRON_NO_KEYRING=1` (or no Secret Service is reachable on a probe), use an **in-memory
     backend** — a `Mutex<HashMap<String, String>>` behind the same `Credentials` façade.
  2. Otherwise, allow **env-var injection** for tests: a secret for connector `<id>` may be supplied
     via `SAFFRON_SECRET_<ID>` (uppercased, `-`→`_`), read transparently by `get_secret` and taking
     precedence so e2e can inject a fake Poly Pizza key with no daemon.
  3. Otherwise use the real `keyring` Secret Service entry.
- The probe must **never panic** — a failed Secret Service connection downgrades to the in-memory
  backend and logs once (idiomatic `tracing`/log at the bridge), it does not propagate a startup
  error. This is what keeps `just e2e` green with no daemon present.
- Document this contract in `editor/AGENTS.md`'s debugging notes (one line: how to inject a test
  secret and how to force the in-memory backend).

### 3. Tauri bridge commands for credential set/clear/status

The Store settings UI must set and clear a key without the secret ever leaving `src-tauri`. Add
Tauri `#[tauri::command]` handlers in `editor/src-tauri/src/lib.rs` (registered in the existing
`invoke_handler` list) — these are **Tauri-local commands**, not engine control-plane commands, so
they do not go through `saffron-protocol`:

- `connector_set_secret(connector_id, secret)` → calls `Credentials::set_secret`.
- `connector_clear_secret(connector_id)` → `delete_secret`.
- `connector_secret_status(connector_id) -> bool` → whether a secret is present (return only the
  boolean; **never** return the secret value to the webview).

Expose thin wrappers from the editor side near `editor/src/control/client.ts` (or a small
`editor/src/control/credentials.ts` if it reads cleaner) calling `invoke(...)` directly — these are
Tauri commands, distinct from the typed `call<C>` control-plane path.

### 4. The `project.json` `stores` block (engine side)

Add a new top-level `stores` block to the project file, following exactly the `editorCamera` /
`debugOverlays` pattern in `engine/crates/assets/src/project.rs`.

- **Save** — in `AssetServer::save_project`, after `debugOverlays` is written, insert a `stores`
  key into the `serde_json::Map` (the `doc`), written via the same `dump_json(Value::Object(doc))`
  call. Write it conditionally when present, matching the `is_object()` guards used for the sidecar
  fields.
- **Load** — in `AssetServer::load_project`, retrieve `stores` via
  `doc.get("stores").cloned().unwrap_or(Value::Null)` next to where `editorCamera` / `debugOverlays`
  are pulled, and carry it back to the caller.
- **`ProjectSidecar`** — extend the `ProjectSidecar` struct (`project.rs`) to carry the `stores`
  value alongside `editor_camera` and `debug_overlays`, so the save/load I/O signatures stay tight
  and `saffron-sceneedit` applies it the same way it applies the camera/overlay sidecar blocks. This
  is the **one** home for per-project enablement — **do not** add a separate per-project
  `settings.json` (NO-LEGACY: one way to do each thing).
- Define the block's shape as a typed serde struct (per-crate, with a `thiserror` parse error if it
  ever needs validation) rather than poking at a raw `Value` from many call sites: enabled connector
  ids (`Vec<String>` of connector ids matching the Phase 1 connector registry) plus a non-secret
  per-connector config map (`BTreeMap<String, serde_json::Value>` for things like Poly Haven
  resolution preference — **never** an API key).

### 5. Wire the `stores` block onto the control plane DTO + regenerate protocol

The editor must read/write the enabled-set through the existing save-project / open-project path —
no new persistence command. The block rides the existing project DTO surface.

- Add the `stores` shape to the relevant DTO(s) in `engine/crates/protocol/src/dto.rs` (the
  project-info / open-project result surface that the editor consumes via `ProjectInfoDto` and the
  open/load flow). Keep field order deliberate (load-bearing for OpenRPC + CLI). Reuse the README's
  canonical connector-id terms; secrets never appear in any DTO.
- If the new shape introduces a struct/enum, add it to the `COMMANDS`-reachable graph and give any
  new command a fixture-or-skip entry in `engine/crates/protocol/src/command.rs` (only if a new
  command is actually added — the preference is to reuse save-project / open-project).
- Update the command handlers in `engine/crates/control/src/commands_asset.rs`
  (`save-project` / `open-project` / `reload-project`) so the `stores` block round-trips through
  `ProjectSidecar` and `project_dto()`.
- Regenerate the editor + CLI + schema artifacts: `cargo run -p xtask -- gen-protocol`. This refreshes
  `editor/src/protocol/sa-types.ts`, `schemas/control/openrpc.generated.json`,
  `command-manifest.generated.json`, and `sa.generated.luau`. Do not hand-edit generated files.

### 6. First-open onboarding flow (editor)

When the Store tab is opened for a project whose `stores` block is empty / absent, prompt the user
to choose which connectors to enable for *this* project.

- In the Store workspace component (the `StoreWorkspace` introduced in Phase 1, dispatched from
  `editor/src/app/App.tsx`), detect the empty enabled-set on first open and show an onboarding
  panel listing the available connectors from the connector registry (each with its `auth_kind`).
- Enabling a connector runs its add-flow keyed off `auth_kind`:
  - `none` (Poly Haven, ambientCG) → instant enable; just add the id to the enabled-set.
  - `api_key` (Poly Pizza) → show the paste-key field (Task 7), store via
    `connector_set_secret`, then add the id.
  - `oauth_loopback` is **not handled here** — Phase 3 wires it; show it as present-but-disabled
    until then (no dead code path that pretends to handle it).
- On enable/disable, write the new enabled-set into the project via the existing save-project flow
  so it persists in the `stores` block. Use the Zustand store patterns in
  `editor/src/state/store.ts` (optimistic write + reconcile, `notifyError(errorText(err))` on
  failure) — do not invent a second persistence route.
- Make the shared-vs-secret distinction visible in the UI copy: enabling a store is shared with the
  project; the key is stored only on this machine and a teammate re-enters their own.

### 7. The Poly Pizza `api_key` connector + settings paste-key field

Implement the third connector and the UI to hold its key.

- **Connector** (`editor/src-tauri`, alongside the Phase 1 connectors): a Poly Pizza connector with
  `auth_kind = api_key`, implementing the Phase 1 connector trait — `search(query, perSourceCursor)`
  and `download(importDescriptor)`. At search/download time it reads its key via
  `Credentials::get_secret("poly-pizza")`; if absent, it returns a typed "not configured" connector
  error (a variant on the Phase 1 connector error enum) rather than emitting empty results silently.
  - API: docs at `https://poly.pizza/docs/api/v1.1`; results are GLB on `static.poly.pizza`.
  - Map each response item onto the canonical **`StoreResult`** (README schema): `kind: "model"`,
    structured `license` (CC0 → `requiresAttribution: false`; CC-BY → `true` with the canonical
    url), `store: { id: "poly-pizza", displayName: "Poly Pizza" }`, `importDescriptor`
    `{ format: "glb", ref: <glb url> }`, `thumbnailUrl`, `sourceUrl` (asset page), `author`, and the
    optionals it provides (`triCount`, `tags`). **Filter-to-importable at the mapping step** — only
    emit items that resolve to a GLB.
  - Keep its **own cursor/offset** and report `exhausted` so the Phase 1 aggregator interleaves it
    round-robin with Poly Haven (and ambientCG once Phase 4 lands it).
- **Settings field**: add an "API & Secrets" section to `editor/src/app/SettingsModal.tsx` (extend
  the `SECTIONS` list and add the dispatch branch), with a new `ApiSecretsSection` component. Use a
  shadcn `Input type="password"` for the Poly Pizza key; on save call `connector_set_secret`, on
  clear call `connector_clear_secret`, and reflect presence via `connector_secret_status` (show
  "key set", never the value). The same paste field is reused by the onboarding `api_key` add-flow
  (Task 6) — one component, not two.

### 8. `sa` CLI command for connector config

Phase 1 added an `sa` connector/search/import command. Extend the connector listing so the
enabled-set and credential *presence* are inspectable from a shell (so a shared project's store
config is debuggable). Because secrets live in the editor's keyring and the `sa` CLI talks only to
the engine control plane, the CLI surfaces the **enabled-set from the `stores` block** (via the
project info it already reads) — it must **not** print or read secrets.

- Ensure `sa connectors` (or the Phase 1 equivalent) reports each connector's `auth_kind` and
  whether it is enabled for the current project (from the loaded `stores` block).
- This is one registration on the existing connector command surface — no duplicate command.

### 9. Docs

Update the Asset Connectors explanation page added in Phase 1 (under
`docs/content/explanations/`), in the same change:

- The credential model: keyring service `saffron-anima`, account = connector id, machine-local;
  the headless / CI fallback (`SAFFRON_NO_KEYRING`, `SAFFRON_SECRET_<ID>`, in-memory backend) and
  why it exists (no Secret Service in the toolbox / e2e).
- The per-project `stores` block in `project.json` (enabled ids + non-secret config), and the
  shared-enabled-set / per-user-secret split.
- The first-open onboarding flow and the Poly Pizza `api_key` add-flow.
- Update the hub `_index.md` row for the page if its summary changes.

### 10. e2e coverage (keep the host boot clean)

- Add a `tests/e2e` (bun/TypeScript) test that the `stores` block round-trips: open a project, set
  an enabled-set + non-secret config via save-project, reload, and assert it persists and that the
  log stays validation-clean.
- Confirm the host boot stays clean with **no Secret Service present** — the credential fallback
  (Task 2) must keep `just e2e` green. Inject a fake Poly Pizza key via `SAFFRON_SECRET_POLY_PIZZA`
  (or force `SAFFRON_NO_KEYRING=1`) in the test harness rather than touching a real keyring.

## Done when

- [ ] Opening the Store for the first time on a project prompts a connector choice; `none` enables
      instantly, `api_key` (Poly Pizza) shows a paste-key field.
- [ ] The enabled-set persists in `project.json`'s new top-level `stores` block (saved/loaded via
      `AssetServer::save_project` / `load_project` and `ProjectSidecar`) and survives a project
      reload.
- [ ] A pasted Poly Pizza key is stored in the OS keyring (service `saffron-anima`, account
      `poly-pizza`); the webview never receives the secret value, only a presence boolean.
- [ ] Search runs globally across Poly Haven **and** Poly Pizza (round-robin interleave,
      per-source cursor/exhaustion), and importing a Poly Pizza GLB produces a catalog
      entry via the host-side `import-model` command.
- [ ] No secret is ever written to `project.json` or any DTO; a teammate opening the shared project
      sees the enabled stores and re-enters their own key.
- [ ] `just e2e` boots a headless host clean with **no Secret Service present** (the in-memory /
      env-var fallback engages; no panic, no validation errors).
- [ ] Protocol regenerated via `cargo run -p xtask -- gen-protocol`; generated files
      (`sa-types.ts`, OpenRPC, manifest, luau) are not hand-edited.
- [ ] The `sa` connector command reports per-connector `auth_kind` and per-project enabled state
      (never secrets).
- [ ] The Asset Connectors docs page + its hub `_index.md` row are updated in the same change.
- [ ] **Milestone gate:** `just engine` then `just prepare-for-commit` (format + lint) both pass,
      with every warning this change raises fixed.
- [ ] **Git left unstaged** — do not commit, stage, or push; leave the work for the user to review.
