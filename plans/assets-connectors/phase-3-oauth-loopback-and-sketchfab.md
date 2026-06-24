# OAuth loopback capability + Sketchfab connector + attribution

**Status:** IN PROGRESS

> Implementation notes:
> - Built and gated: engine + `src-tauri` `clippy -D warnings` clean; editor typechecks + oxfmt +
>   oxlint 0 errors; protocol tests pass.
> - Reusable `oauth_loopback.rs`: ephemeral `127.0.0.1` listener, random CSRF `state`, the
>   Anima-styled self-contained fragment-bridge landing page, single-callback shutdown + 5-min
>   timeout, token → keyring. Driven by `OAuthLoopbackConfig` (provider-agnostic).
> - Sketchfab connector (`sketchfab.rs`): Data API v3 search (`downloadable=true`, cursor paging) +
>   Download API (GLB→glTF→USDZ); 401 → `NotConfigured` (re-login). `connector_login` Tauri command
>   runs the flow on a worker thread.
> - Attribution surfaced: `AssetEntryDto.attribution` added + populated by `asset_list_dto`; a
>   **Credits** view (`StoreCredits.tsx`) lists assets whose license requires attribution. Sketchfab
>   onboarding now does Connect (login) → enable.
> - **Deviations:**
>   - Reused the existing `open_external` (flatpak-spawn → xdg-open) as `open_url_in_browser` instead
>     of adding `tauri-plugin-opener` — it is the URL opener that actually works in the toolbox, and
>     keeps one URL-opening path.
>   - Sketchfab `client_id` is read from `SAFFRON_SKETCHFAB_CLIENT_ID` (no secret shipped); login
>     errors clearly when unset.
>   - Attribution is recorded on the catalog row / `project.json` and surfaced via `list-assets`;
>     writing it into the `.smodel` `ContainerMetadata` (durable across a from-disk rescan) is a noted
>     follow-up — the catalog/project is the source of truth the Credits view reads.
>   - Crediting uses the "Sketchfab" store-badge wordmark on result cards; bundling the official logo
>     asset is a follow-up (need the brand asset file).
>   - No `sa connector-login` — the login is an interactive browser/loopback flow that lives in the
>     editor bridge and cannot run from the headless CLI.
> - **Not yet run (deferred per session goal):** the e2e suite. The attribution e2e assertion is in
>   `tests/e2e/store-import.test.ts` (list-assets surfaces attribution).

## What this phase delivers

The third `auth_kind` — **`oauth_loopback`** — comes online, and with it the **Sketchfab**
connector, the largest catalog in the provider matrix (700k+ downloadable CC models). This is the
only connector that needs a full OAuth login, so the work is split cleanly:

1. A **reusable `oauth_loopback` capability** in `editor/src-tauri`: an ephemeral
   `127.0.0.1:<free-port>` HTTP listener (loopback only — **never** `0.0.0.0`), a system-browser
   open to the provider authorize URL, an Anima-styled self-contained success/error landing page,
   CSRF `state` validation, single-callback-then-shutdown, and a keyring token write. It is keyed
   off `auth_kind = oauth_loopback` and is generalized so a future OAuth store only declares its
   endpoints — it never special-cases Sketchfab.
2. The **Sketchfab connector**: Data API v3 search/browse → `StoreResult[]`, Download API →
   glTF/GLB/USDZ, per-user token from the loopback capability.
3. **Attribution capture**: for CC-BY and Sketchfab the structured `license` plus `author` and
   `sourceUrl` are written onto the catalog asset's metadata at import, so attribution follows the
   asset everywhere, and a **credits view** surfaces it. The Sketchfab logo-crediting requirement is
   honoured in the results UI.

### Why OAuth loopback / implicit flow, not Authorization Code

Sketchfab's OAuth docs offer **Authorization Code** (requires a client *secret* — unusable in an
open-source desktop app, since the secret cannot be kept secret, and Sketchfab does **not** offer
PKCE), **Implicit** (no secret), and **Username/Password**. We therefore use the **Implicit flow +
loopback redirect**. Sketchfab explicitly allows loopback redirect URIs — `http://127.0.0.1:port`
or `localhost`, and "Multiple redirect URIs are supported" — which is the RFC 8252 native-app
pattern (the same one `gh` / `gcloud` / `aws sso` use).

The implicit flow returns the token in the URL **fragment** (`#access_token=...`), which a browser
never sends to the server. So the landing page has a **functional** job, not a cosmetic one: its
inline JS reads `window.location.hash` and `POST`s the token back to the local listener. Implicit
tokens last **1 month with no refresh token**, so the UX plans for a monthly re-login (acceptable;
the connector detects 401 and re-prompts).

Endpoints (declared per the generalized capability, not hard-wired): authorize
`https://sketchfab.com/oauth2/authorize/`, token `https://sketchfab.com/oauth2/token/` (the token
endpoint is unused in implicit flow but recorded so a future store using Authorization Code can
reuse the same descriptor). Sketchfab is now an Epic/Fab property folding into Fab — **flag
standalone-API longevity as a risk** in the connector docstring and the docs page.

This phase depends on Phase 1 (the `auth_kind`-keyed connector trait + aggregator + host-side
`import-model` command) and Phase 2 (the keyring integration with its headless/no-Secret-Service
fallback, and the `project.json` `stores` block).

## Tasks

### 1. Add the Tauri opener plugin

- Add `tauri-plugin-opener` to `editor/src-tauri/Cargo.toml` (Rust) and
  `@tauri-apps/plugin-opener` to `editor/package.json` (JS), alongside the existing
  `tauri-plugin-dialog`. Register the plugin in the builder in `editor/src-tauri/src/lib.rs`.
- The capability opens the authorize URL with the opener's `open_url` (system default browser). Do
  **not** reintroduce any legacy `shell.open` path — `opener` is the one way to open URLs.
- Grant the minimal opener permission in the Tauri capabilities config so `open_url` is callable.

### 2. The reusable `oauth_loopback` capability in `src-tauri`

Add a module `editor/src-tauri/src/oauth_loopback.rs` (declared from `lib.rs`). It is provider-
agnostic and driven entirely by a descriptor the connector supplies — no Sketchfab strings here.

- An `OAuthLoopbackConfig` struct (the connector declares it): `authorize_url`, `token_url`
  (recorded for future Authorization-Code stores; unused by implicit), `client_id`, `scope`,
  `response_type` (`"token"` for implicit), and the keyring key (e.g. `saffron-anima/sketchfab`,
  matching the connector id from Phase 1/2).
- A `run_loopback_login(config) -> Result<AccessToken, OAuthError>` flow:
  1. Bind a `TcpListener` to `127.0.0.1:0` (OS-assigned free port) — assert loopback, **never**
     `0.0.0.0`. Read back the bound port to build `redirect_uri = http://127.0.0.1:<port>`.
  2. Generate a cryptographically random `state` (CSRF) and hold it for the single expected
     callback.
  3. Build the authorize URL from `config.authorize_url` with query params `response_type`,
     `client_id`, `redirect_uri`, `scope`, `state`, and `open_url` it.
  4. Accept exactly **one** callback on the listener, then shut the listener down. Two request
     shapes are handled on the one port:
     - `GET /` (the browser redirect target): the server cannot see the fragment, so it returns
       the **fragment-bridge landing page** (task 3) — a `200 text/html` page whose inline JS reads
       `window.location.hash` and `POST`s `{ access_token, state }` back to `POST /callback`.
     - `POST /callback` (from that JS): parse the token + `state`, **validate `state`** against the
       held value (reject + render the error page on mismatch), and resolve the flow.
  5. On success write the token to the keyring via the Phase 2 keyring helper, keyed by
     `config.keyring_key`, and return it. On any failure return a typed `OAuthError`.
- `OAuthError` is a `thiserror` enum in this module (no stringly `Result<T, String>`): variants for
  bind/listen I/O, browser-open failure, state mismatch (CSRF), missing/garbled token, and timeout
  (an overall deadline so a user who never finishes login does not leak the listener). Propagate
  with `?`.
- The listener future is bounded by a timeout; on timeout the port is released and the keyring is
  untouched.

### 3. The Anima-styled fragment-bridge landing page

- Author one **self-contained** HTML document (inline CSS, **no external fetches** — no CDN fonts,
  no remote stylesheets, so it renders with the network cut and triggers no CSP surprises). Keep it
  as a `const &str` (or `include_str!` of an asset under `editor/src-tauri/`) used by
  `oauth_loopback.rs`.
- Two visual states share the template: **success** ("You're connected to Sketchfab — you can close
  this tab and return to Saffron Anima") and **error** (state mismatch / missing token / denied),
  both styled in the Anima palette to match the editor.
- The **success path is functional**: inline JS runs on load, parses `window.location.hash` for
  `access_token` and the echoed `state`, `fetch`-`POST`s them to `/callback` on the same origin,
  then swaps the DOM to the success message. If the hash has `error=...` (user denied), it renders
  the error message and does **not** POST a token.
- No secrets are baked into the page; it only relays the fragment the browser already holds.

### 4. The Sketchfab connector

Add `editor/src-tauri/src/connectors/sketchfab.rs` implementing the Phase 1 connector trait with
`auth_kind = oauth_loopback`.

- **Add-flow**: declares its `OAuthLoopbackConfig` and runs `run_loopback_login` (task 2). On
  success the token lands in the keyring under `saffron-anima/sketchfab`; the connector reads it
  back for every request.
- **search(query, perSourceCursor)**: call Data API v3 search/browse with the query and the
  per-source cursor/offset (matching the Phase 1 aggregator's per-source cursor + `exhausted`
  contract). Send `Authorization: Bearer <token>`. Map each API model onto the canonical
  `StoreResult`:
  - `kind: "model"` (filter-to-importable at the mapping step — only emit results with a Download
    API deliverable, never a generic post-filter).
  - `store: { id: "sketchfab", displayName: "Sketchfab" }`, `id`, `name`, `author`, `thumbnailUrl`,
    `sourceUrl` (the model page — used for both "view on site" and attribution).
  - `license`: the **structured** `StoreLicense` — map Sketchfab's per-model license to the
    canonical `{ id, requiresAttribution, url }` (CC-BY / CC-BY-SA ⇒ `requiresAttribution: true`,
    CC0 ⇒ `false`). Never a free string.
  - `importDescriptor: { format: "glb" | "gltf" | "usdz", ref: <model uid> }`.
  - optional `triCount`, `fileSize`, `tags`, `publishedAt`/`updatedAt` only when the API provides
    them.
- **download(importDescriptor)**: hit the Download API for the model uid, pick the deliverable
  (prefer GLB, fall back to glTF/USDZ), GET the file to a temp path under the editor's app data, and
  return the local path. On 401 surface a typed `re-login required` error so the UI can re-run the
  add-flow (monthly token expiry).
- A `SketchfabError` `thiserror` enum (HTTP status, deserialize, no-token, expired/401, no
  deliverable). No `unwrap`/stringly errors.

### 5. Attribution capture on the catalog asset (host side)

Attribution must be captured **at import** and stored so it travels with the asset. This rides the
Phase 1 `import-model` control command rather than adding a second command.

- Extend the `import-model` params DTO in `engine/crates/protocol/src/dto.rs` with an optional
  **`attribution`** sub-object carrying the structured license + author + sourceUrl (canonical:
  `{ license: { id, requiresAttribution, url }, author, sourceUrl }`). Update the `COMMANDS` entry's
  fixture in `engine/crates/protocol/src/command.rs` to include it. This is **not** a new command —
  it is one field on the existing one (no-legacy: one import path).
- In the host handler (`engine/crates/control/src/commands_asset.rs`, the `import-model` handler
  from Phase 1), thread the attribution onto the produced catalog metadata:
  - For models, into the `.smodel` container's `ContainerMetadata` (`engine/crates/assets/src/model.rs`
    — add an `attribution` field to the META chunk, written by `bake_model` /
    `encode_container_metadata`).
  - Onto the `AssetEntry` catalog row (`engine/crates/scene/src/environment.rs`) — add an optional
    `attribution` field, serialized by `catalog_to_json` / read by `catalog_from_json`
    (`engine/crates/assets/src/catalog.rs`), omitted when absent so existing rows stay clean.
- Use a single `Attribution` struct shared across the assets crate (one definition, reused by the
  container META and the catalog row) — not two parallel shapes.
- Regenerate the protocol after the DTO change: `cargo run -p xtask -- gen-protocol`.

### 6. Surface attribution in the editor (credits view + Sketchfab logo)

- The Store results UI honours the **Sketchfab crediting requirement**: results sourced from
  Sketchfab display the Sketchfab logo/wordmark on the result card or store-group header (bundle the
  logo as a local asset under `editor/src/` — no remote fetch).
- Add a **credits / attribution view**: a panel (or a section of the asset inspector) that lists
  imported assets whose `license.requiresAttribution` is `true`, showing author, license id (linked
  to `license.url`), and `sourceUrl` ("view on site"). It reads the attribution off the catalog
  metadata returned by the existing `list-assets` result (extend its DTO to carry the optional
  attribution, regenerate protocol). This is the user's one-stop "credits" surface for a project's
  CC-BY / Sketchfab content.

### 7. `sa` CLI

The Phase 1 connector/search/import `sa` commands already exist; this phase only needs the login
path reachable from a shell:

- Add an `sa connector-login <connector-id>` command — one DTO + one `COMMANDS` entry — that, for an
  `oauth_loopback` connector, triggers the loopback flow (the actual browser open + listener run
  live in `src-tauri`, so the CLI command drives the editor-side flow over the control plane or
  reports that login must be completed in the editor; keep it a single registration). Verify
  `sa connector-list` shows Sketchfab as connected once a token is present, and that
  `sa connector-search provider:sketchfab <query>` returns mapped results. Regenerate protocol after
  the DTO change.

### 8. Docs

- Add a docs page under `docs/content/explanations/` for **OAuth loopback & the Sketchfab
  connector** (the loopback/implicit-flow rationale, the fragment-bridge page, the CSRF `state`, the
  monthly re-login, the keyring token, and the attribution-follows-the-asset story), and extend the
  Asset Connectors hub page's `_index.md` row, in this same change. Include the Epic/Fab longevity
  risk note. Use the slim `What | File | Symbols` pointer table (symbols, not line numbers); run the
  prose through the `humanizer` pass.

### 9. e2e

- Add a `tests/e2e` (bun/TypeScript) test for `import-model` **with** the new `attribution` field:
  import a fixture model carrying a CC-BY license + author + sourceUrl over the control plane, then
  read the catalog back (`list-assets`) and assert the attribution is recorded on the catalog asset.
  Use a local fixture file (no live Sketchfab network call in CI) and a validation-clean log. The
  OAuth browser flow itself is interactive and stays out of e2e; the test covers the host-side
  attribution storage that the connector depends on.

## Done when

- [ ] Clicking **Connect** on Sketchfab opens the system browser to the Sketchfab authorize URL with
      a `redirect_uri = http://127.0.0.1:<free-port>` and a random `state`.
- [ ] After browser login, the **Anima-styled localhost landing page** confirms the connection; its
      inline JS reads `window.location.hash`, `POST`s the token back to the local listener, the
      `state` validates (CSRF), the listener accepts exactly one callback then shuts down, and the
      token is written to the OS keyring under `saffron-anima/sketchfab`.
- [ ] The listener binds loopback only (`127.0.0.1`, never `0.0.0.0`) and is released on timeout.
- [ ] The `oauth_loopback` capability is generalized: a future OAuth store enables it by declaring an
      `OAuthLoopbackConfig` (endpoints, client id, scope, keyring key) — Sketchfab is not special-
      cased in `oauth_loopback.rs`.
- [ ] Searching with the Store enabled returns Sketchfab results mapped onto the canonical
      `StoreResult` (structured `license`, `kind: "model"`, `sourceUrl`, `author`), unimportable
      results never emitted, and Sketchfab results show the Sketchfab logo per the crediting
      requirement.
- [ ] Importing a Sketchfab model downloads the deliverable (GLB/glTF/USDZ), runs the existing
      importer via `import-model`, and **records license + author + sourceUrl on the catalog asset**
      (`.smodel` META + `AssetEntry`), visible in the credits/attribution view.
- [ ] A 401 (expired monthly token) surfaces a typed re-login prompt and re-running the add-flow
      restores access.
- [ ] `sa connector-login`, `sa connector-list`, and `sa connector-search provider:sketchfab` work
      from a shell.
- [ ] All new wire DTOs added to `engine/crates/protocol/src/dto.rs` + `COMMANDS`
      (`engine/crates/protocol/src/command.rs`) with a fixture/skip, and `cargo run -p xtask --
      gen-protocol` regenerated `sa-types.ts`, `openrpc.generated.json`, the manifest, and the Luau
      defs (byte-identical tests pass). Editor calls go through the typed `client` in
      `editor/src/control/client.ts`.
- [ ] Errors are typed `thiserror` enums (`OAuthError`, `SketchfabError`); no stringly
      `Result<T, String>`, no `unwrap` on fallible paths; `unsafe_code` stays denied. No legacy /
      compat shim: one import path (`import-model` gains the `attribution` field rather than a second
      command), one `Attribution` struct.
- [ ] The docs page + `_index.md` hub row are added in this same change.
- [ ] The `tests/e2e` attribution test passes against a headless host with a validation-clean log.

**Milestone gate:** run `just engine` then `just prepare-for-commit` (format + lint) and fix every
warning this change raises.

**Git stays read-only.** Leave all work **unstaged** — do not stage, commit, or push. The user
reviews and commits.
