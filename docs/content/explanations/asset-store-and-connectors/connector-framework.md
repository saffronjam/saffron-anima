+++
title = 'The connector framework'
weight = 1
+++

# The connector framework

A connector is one external asset service the editor can search and import from. The framework
that holds them has three jobs: present every service through one normalized result shape, run a
search across all enabled services at once, and turn a chosen result into a catalog asset. It
lives in `editor/src-tauri/src/connectors/` because service calls are HTTP from native Rust (no
browser CORS), any credentials stay out of the renderer, and provider thumbnails are just URLs
the webview loads directly.

| What | File | Symbols |
|---|---|---|
| Trait + normalized types | `editor/src-tauri/src/connectors/mod.rs` | `StoreConnector`, `StoreResult`, `StoreLicense`, `AuthKind` |
| First connector (keyless, CC0) | `editor/src-tauri/src/connectors/polyhaven.rs` | `PolyHaven` |
| Search aggregation | `editor/src-tauri/src/connectors/aggregator.rs` | `SearchSession` |
| Registry | `editor/src-tauri/src/connectors/registry.rs` | `ConnectorRegistry` |
| API-key connector | `editor/src-tauri/src/connectors/polypizza.rs` | `PolyPizza` |
| Credentials (keyring) | `editor/src-tauri/src/connectors/credentials.rs` | `Credentials` |
| Per-project enablement | `engine/crates/assets/src/project.rs` | `ProjectSidecar.stores`, `get-stores`, `set-stores` |
| Import (host side) | `engine/crates/control/src/commands_asset.rs` | `import-model` |

## One normalized result

Every connector maps its provider's response onto a single `StoreResult`. Two fields carry
weight. `kind` (`model` / `hdri` / `material` / `texture`) decides which importer the result runs
through. `license` is structured — `{ id, requiresAttribution, url }`, never a free string — so
attribution can be enforced at import: CC-BY and similar licenses set `requiresAttribution`, and
that, with the author and source URL, is written onto the catalog asset so it travels with the
asset everywhere.

A connector also declares an `auth_kind`: `none` (a `User-Agent` header only, like Poly Haven),
`api_key` (Poly Pizza), or `oauth_loopback`. It is the one switch the rest of the framework
branches on.

## Credentials and per-project enablement

These are deliberately split. **Which connectors a project uses** is shared, committed state: it
lives in `project.json` as a `stores` block (a list of enabled connector ids), travels with the
project through the existing `save-project` / `open-project` path, and is read or written over the
control plane via `get-stores` / `set-stores`. A teammate who opens the project sees the same
enabled stores.

**A connector's secret** (an API key, later an OAuth token) is the opposite: machine-local and never
committed. It lives in the OS keyring (service `saffron-anima`, account = connector id), read only in
the bridge — the webview sets or clears a key and learns only whether one is present, never its value.
So a teammate inherits the enabled set but enters their own key. The keyring degrades to an in-memory
backend when no Secret Service is reachable (`SAFFRON_NO_KEYRING`, or the CI/toolbox case), and a test
can inject a key with `SAFFRON_SECRET_<ID>`, so a headless host boots without a daemon.

Opening the Store with nothing enabled shows an onboarding panel: enabling a `none` connector is
instant; an `api_key` connector wants its key first.

## Search across sources

Heterogeneous services cannot share a page number, so the Store does not paginate — it scrolls. A
`SearchSession` holds each connector's own cursor and exhaustion flag, pulls a page from a source
only when its buffer runs low, and interleaves results round-robin. There is no synthesized global
relevance order; the grid's scroll position is what drives how many batches are pulled, and the
session stops only once every source is exhausted. A connector that errors is dropped from the
round for that session rather than blanking the whole Store.

Search runs only on a committed query — Enter or a chip commit in the shared `AnimaSearchbar` —
never on each keystroke, so typing does not fire a request per character.

## Import

Importing downloads the deliverable to a local file, then calls the engine's existing import command
for that asset `kind` with the file path plus the result's attribution. There is one command per
deliverable shape, reused for every connector — no connector-specific import path:

| `kind` | Deliverable | Host command | Engine path |
|---|---|---|---|
| `model` | one `.glb` / `.gltf` | `import-model` | `bake_model` → `.smodel` |
| `material`, `texture` | a zip of PBR maps (extracted to a folder) | `material-import` | `import_material_folder` → `.smat` |
| `hdri` | one `.hdr` | `import-texture` | `import_texture` → a texture |

`material-import` is the existing local-folder material importer, extended in place with the optional
attribution — the connector path is the same command, not a duplicate. Each command records the
license/author/source on the catalog entry, so attribution travels with the asset regardless of kind.

## Selective import (per-part)

A result can also expose its individual files. The card's split button — `[ Import ▾ ]` — imports the
whole asset on the main action; the dropdown lists the asset's parts (the PBR maps) and imports just
the chosen one as a standalone, colorspace-correct texture. This is a connector capability, not a
provider special case: a connector sets `has_parts` and implements `parts()` / `download_part()`
(`editor/src-tauri/src/connectors/mod.rs`), and the rest of the pipeline stays uniform.

How each provider fills it: **Poly Haven** lists every map as its own file over REST, so it fetches
only the picked file; **ambientCG** knows the map roles over REST but ships a per-resolution zip, so
`download_part` fetches the zip once (cached by hash) and extracts the chosen map; **Poly Pizza /
Sketchfab** expose no parts, so they show a plain Import button. A single map imports through
`import-texture` with a colorspace derived from its role — color/albedo as sRGB, every data map
(normal/roughness/metallic/AO) as linear — matching how UE5 treats texture imports.

## Gallery and the detail view

A result card shows one thumbnail, but an asset usually has more to look at. `gallery()` is the
connector method that returns an asset's preview images — lazily, the same as `parts()` — defaulting
to just the card thumbnail. **Poly Haven** overrides it to return the hero render, the site's
gallery renders (the textured `orth_front`/`orth_side`/`orth_top` angles and the `clay` pass — these
live at predictable `asset_img/renders/<slug>/` CDN paths that the JSON API does not list, so each is
HEAD-probed and included only if present), and one image per map (the `/files` listing it already
reads for parts). So a model's gallery shows the angle renders plus its diffuse, normal, and roughness
maps; the providers whose APIs expose a single render (ambientCG, Poly Pizza, Sketchfab) keep the default. The result is fetched only once the card is hovered or its detail modal opens, so
scrolling a hundred cards does not fire a hundred requests.

The webview drives this through one bridge command, `store_asset_gallery`, and shows it two ways: the
card thumbnail gets prev/next arrows when the gallery has more than one image, and an **expand** button
opens a two-pane detail modal — the gallery (with a thumbnail strip) on the left, the asset's
metadata, license, and the same Import split button on the right. Both views share one `useGallery`
fetch so they stay on the same image. Like every store call, `gallery()` is editor-side only; nothing
about it crosses to the host until an import.
