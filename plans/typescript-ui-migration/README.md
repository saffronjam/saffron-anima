# TypeScript/Tauri Editor Migration

Replace the C++ ImGui editor entirely with a Bun/Tauri/React/TypeScript editor that
embeds the live `SaffronEditor` render output as a reparented native X11 child window,
driving every editor operation over the existing JSON-over-unix-socket control protocol
(the same one the `se` CLI speaks).

## Why

The MVP on branch `explore-ui` (`faf704d`) proved the approach: the Tauri Rust backend
spawns `SaffronEditor` with `SAFFRON_EDITOR_NATIVE_VIEWPORT=1` + `SAFFRON_CONTROL_SOCK` +
`SDL_VIDEODRIVER=x11`, sends `attach-native-viewport` with the Tauri window XID, and the
engine reparents its own SDL/Vulkan window over a placeholder div and presents only the
scene (no ImGui). That MVP branched at lighting phase 3 (`f8fc077`); current `main`
(HEAD `6460244`) has diverged far ahead (lighting 4-8, skybox, procedural sky,
render-graph MRT, renderer module split, 38 control commands), so the engine bridge must
be **forward-ported (not merged)** and the React UI grown from a single-entity demo into
a full editor at **100% feature parity**.

The goal of this migration:

1. Completely replace the C++ ImGui editor with the TypeScript/Tauri editor.
2. Auto-start the engine **and** auto-attach the viewport on app boot (the MVP needs
   manual "Start Engine" + "Attach Viewport" clicks — that gap is closed), with a
   "Preparing renderer…" loading overlay until attach succeeds.
3. Port every C++ editor feature (hierarchy, generic component inspector, asset browser
   with thumbnails + drag-drop, environment panel, create menu, gizmo T/R/S + world/local,
   fly-cam, ray-pick selection, save/load project/scene, import model/texture,
   render-stats).
4. A typed, programmatic, easy-to-use TypeScript client around the control protocol.
5. Shared types kept in sync between C++ and TypeScript (schema-first; C++26 reflection
   left as a forward seam).
6. A scalable bridge and a smooth startup.

## Directory-layout decision (resolved up front, not deferred)

The C++ `SaffronEditor` target **owns** all engine shader compilation and asset copying via
`editor/CMakeLists.txt:11` (`saffron_compile_shaders` on the `SaffronEditor` target emits
all ~31 shaders + models/fonts/icons to `SAFFRON_RUNTIME_DIR`). You therefore **cannot**
host both the C++ `SaffronEditor` (`editor/CMakeLists.txt` + `editor/source/main.cpp` +
`editor/assets/`) and the Tauri app at `editor/` simultaneously. The worktree resolved this
by moving the C++ editor to `editor-old/` at `faf704d` (`CMakeLists.txt:32` =
`add_subdirectory(editor-old)`).

This plan follows that resolution **immediately in phase 1**: the C++ host moves to
`editor-old/` (keeping its `CMakeLists`, shaders, assets, and the `SaffronEditor` target
intact and still built via `add_subdirectory(editor-old)`), and the new Tauri/TS editor
takes `editor/`. **"`SaffronEditor` + its shader/asset compilation" is treated as a single
named artifact** (the headless viewport host) that survives the entire migration,
independent of the C++ ImGui panel code, and is only retired once parity is confirmed in
phase 10 (where the host target relocates to a surviving location, not deleted).

## Working approach (resolved decisions)

- **X11-reparent + `presentViewportOnly` is the sole production bridge.** The
  fd-export/DMA-BUF path (`wt:editor/src-tauri/src/viewport_bridge.rs`, 417 lines, GTK4,
  never spawned) is **not forward-ported at all** — there is nothing to "quarantine" on
  `main` since those pieces never existed there. Wayland is XWayland-only (the parent XID
  hard-requires Xlib, `wt:lib.rs:89-97`).
- **One generic Rust `control(cmd, params)` passthrough** replaces the 8 bespoke MVP shims;
  Rust keeps only the window-handle commands (spawn / attach / resize / quit). Adding any
  new `se` command then needs zero Rust change.
- **Shared types are schema-first**: a hand-authored JSON Schema in `schemas/control/` is
  the source of truth, `json-schema-to-typescript` generates `editor/src/protocol/`, and a
  contract test validates live `se` output against it. C++26 reflection is deferred (not in
  stock Clang 21 + libc++), with a `dump-schema` command + `reflect-cpp` left as forward
  seams.
- **Selection round-trips** via a new `get-selection` + frame-stamped `selectionVersion`;
  the entity list gets a `sceneVersion` so the hierarchy poll diffs instead of re-rendering
  every tick.
- **In-viewport gizmo + ray-pick stay engine-side** (rendered through the engine overlay
  pipeline, not ImGui, which is skipped under present-only). Whether the fly-cam stays
  native or moves to control commands is decided by an early input-routing spike (phase 3)
  before any phase commits to native keyboard input.

## Plan status convention

Each phase file carries a `**Status:**` line (`NOT STARTED` / `IN PROGRESS` / `COMPLETED`).
Mark a phase `COMPLETED` when its work is done and validation-clean; delete a phase file
only *after* it is `COMPLETED` and merged. Delete this folder once all phases are done and
the C++ ImGui editor is retired.

Build inside the `saffron-build` toolbox (`cmake --build build/debug -j1`); the frontend
runs via `bun run dev` / `bun run check` (the build-environment spike in phase 3 confirms
whether the frontend toolchain runs inside the toolbox or on the host).

## Cross-cutting decisions

These decisions span every phase; each phase grounds back to them.

### UI toolkit (shadcn/ui + Tailwind v4)

All React UI is built with **shadcn/ui** (Radix primitives copied into the repo via the shadcn
CLI) on **Tailwind CSS v4** (`@tailwindcss/vite` plugin + `@import "tailwindcss"`), not
hand-rolled CSS. shadcn is not an npm dependency — `shadcn init` writes `components.json` +
`src/lib/utils.ts` (`cn`) + the theme CSS variables, and `shadcn add <component>` copies each
component's source into `src/components/ui/` (we own + can tweak it). Setup follows the official
Vite guide: a `@/*` path alias in `tsconfig.json` + `vite.config.ts`, `@types/node`, and the
Tailwind Vite plugin. The editor runs in one known-modern webview and uses shadcn's CSS token
shape (`--background`, `--primary`, `--border`, …) with neutral app-level values. lucide-react
(already used) is shadcn's icon set, and shadcn's `resizable` IS `react-resizable-panels`
(phase-9's dock layout). **Phase 9 is therefore expressed in shadcn terms** (theme tokens plus
Button / DropdownMenu / ContextMenu / Select / Slider / Tabs / ScrollArea / Separator / Dialog /
Tooltip). Panels built in phases 3–8 use shadcn from the start; any built before this decision was
recorded are retrofitted.
**Occlusion still applies:** Radix popovers/dialogs/menus portal to the document root — any that
would overlap the reparented X11 viewport rect must render outside it or while the native window
is unmapped (same rule as the loading overlay).

### Type-sharing strategy (schema-first)

The wire contract lives in a hand-authored JSON Schema (draft 2020-12) under
`schemas/control/`:

- The envelope `{ok, error, result}` (matches `control_server.cpp:221-234`).
- Shared sub-DTOs: `Vec3`/`Vec4`/`Uuid`/`EntityRef`/`Transform`/`Material`/lights/
  `RenderStats`/`Environment`/`AssetEntry`.
- One result schema per command.
- The component family as a **discriminated union keyed on component name** (mirrors
  `inspect`'s `components:{[Name]:DTO}`).

TS types are generated by `json-schema-to-typescript` into `editor/src/protocol/` in a Bun
prebuild step (runs before `tsc`/vite build and before `bun run check`; re-running
reproduces output deterministically). The **C++ side is a validated consumer, not the
origin**: a contract test (`tools/check-control-schema`) captures real `se` outputs and
validates them against the schemas, catching inline-json drift — because there are **no
named C++ DTO structs today** (every response is an ad-hoc `nlohmann::json` literal;
components are type-erased `std::function` closures in `scene.cppm` /
`editor_components.cpp`).

C++26 P2996 reflection is **not** usable on stock Clang 21 + system libc++ (only the
Bloomberg `clang-p2996` fork ships `std::meta`), so it is deferred. Forward seam: the
`dump-schema` command (phase 2) emits the live component/env/stats shapes, and when named
C++ DTO structs eventually exist, `reflect-cpp`'s `rfl::json::to_schema` can regenerate the
**same** schema files the TS pipeline already consumes. `quicktype` bootstraps the initial
skeletons **once** from captured `se` payloads, then is hand-corrected for the discriminated
union + nullability and never wired into the steady-state build (it is lossy on exactly the
union shapes).

**Critical invariants:**

- Every `Uuid`/id is a `u64` (`core.cppm:51-53`) exceeding `Number.MAX_SAFE_INTEGER`, so it
  is typed as **`string` end-to-end** (never JS `number`; bigint-aware or string-preserving
  parse required).
- Casing is **camelCase on the wire** (`drawCalls`, `albedoTexture`) and the schema enforces
  it so Rust serde renames and TS types are derived, not hand-aligned.
- Per-field units documented: `Transform.rotation` is **Euler XYZ radians** on the wire
  (UI shows degrees); SpotLight `innerAngle`/`outerAngle` are **degrees**.

### Auto-start / auto-attach / loading overlay

A Tauri `.setup()` hook + a React state machine driven by an engine-readiness signal.

On boot the Rust setup hook:

1. **Spawns** `SaffronEditor` (`build/debug/bin/SaffronEditor`, overridable by
   `SAFFRON_ENGINE_BIN`) with `SAFFRON_EDITOR_NATIVE_VIEWPORT=1` + `SAFFRON_CONTROL_SOCK`
   (a per-instance socket in `XDG_RUNTIME_DIR` including the Tauri pid for two-window
   isolation) + `SDL_VIDEODRIVER=x11` + `cwd = repo_root`.
2. **Polls** `viewport-native-info` with a child-liveness-aware bounded retry (~10s
   exponential backoff, `child.try_wait()` to distinguish "socket not bound yet" from
   "process crashed") — replacing the MVP's fragile fixed 20×100ms (`wt:lib.rs:274-306`).
3. Once ready **and** the `ViewportPanel` div has a non-zero rect (layout-effect +
   `ResizeObserver` wait), **attaches** with the real bounds × `window.scaleFactor()` for
   HiDPI.
4. Emits `app.emit('viewport-attached')` or `'viewport-error'`.

React drives `engineStatus.phase` (`idle | starting | attaching | ready | error`) and shows
the `LoadingOverlay` until `phase === 'ready'`.

**Runtime crash recovery:** a liveness watchdog (the reconcile poll detects socket EOF / a
dedicated `child.try_wait` check) flips `phase` back to `error` mid-session, re-shows the
overlay, and offers **Restart** (re-spawn + re-attach). `state_snapshot` must use
`child.try_wait()`, **not** `Option::is_some` (the MVP bug at `wt:lib.rs:178-194` reports a
dead engine as running). The MVP placeholder div (`wt:main.tsx:352-356`) becomes the
overlay; manual Start/Attach buttons are removed.

### Viewport-bridge decision

X11-reparent + `presentViewportOnly` is the sole bridge. The fd path copies every frame,
never presents, would pull `vulkan.hpp` + VMA into the clean `control_server.cpp` TU, and
would gate device selection — so it is dropped entirely:

- **Do NOT** add `viewport_bridge.rs`, the `[[bin]] viewport-bridge` target, the
  `gtk4`/`gdk4`/`glib` Cargo deps, the engine `sendViewportFd`/`drainViewportServer`/
  `.viewport`-second-socket/`viewportServer`+`viewportActive` fields, or the external-memory
  device extensions. `command.cppm` needs **no** viewport-server fields.

**Forward-port (Tier A):**

- `presentViewportOnly` flag + `setPresentViewportOnly` + `presentViewportToSwapchain`
  (offscreen→swapchain blit `transferSrc`→`transferDst` Nearest→`PresentSrcKHR`) in
  `renderer.cppm` + `renderer_types.cppm`.
- `attach-native-viewport` (`SDL_PROP_WINDOW_X11_DISPLAY_POINTER` /
  `SDL_PROP_WINDOW_X11_WINDOW_NUMBER` + `XReparentWindow`/`XMoveResizeWindow`/`XMapRaised`/
  `XFlush`) + `viewport-native-info` in `control_commands_render.cpp` (anchors verified at
  `wt:control_commands_render.cpp:61/76/129/131/137`).
- The `SAFFRON_EDITOR_NATIVE_VIEWPORT` branch in `editor_app.cppm`
  (`wt:editor_app.cppm:484`).
- `X11::X11` linkage in `Dependencies.cmake`.

**Two conflicts vs `main`:** (1) **MRT** — `render_graph.cppm:58` `RgAttachment` has
`std::optional<RgResource> resolve` (`:64`) and `RgPass` has
`std::vector<RgAttachment> colors` (`:75`), so present-only/overlay edits use
`colors.push_back(RgAttachment{...})`, **not** `.color=`. (2) Device extensions are
irrelevant since the fd path is dropped (no external-memory extensions added at all).

Add a dedicated **`resize-native-viewport`** command (`XMoveResizeWindow` +
`setViewportDesiredSize` only; **no** reparent/remap) so the per-tick bounds sync does not
re-`XReparent` and flicker (the MVP re-sends a full attach every 250ms,
`wt:lib.rs:343-364`).

### `se`-client architecture (two layers)

**Layer 1 — transport (Rust):** ONE generic `control(cmd, params) -> JsonValue` command
proxying `control_request_with_params` (replacing the 8 bespoke MVP shims at
`wt:lib.rs:219-388`). Rust keeps ONLY window-handle commands (`start_engine`,
`attach_native_viewport`, `resize_native_viewport`, `quit_engine`). Adding any new `se`
command then needs zero Rust change.

**Layer 2 — `editor/src/control/client.ts`:** `async call<R>(cmd, params)` wrapping
`{id, cmd, params}` → `{ok, result | error}` (surfacing `ok:false` as a typed rejection),
plus one typed method per command mapped against `@saffron/protocol`'s `CommandResultMap`
(`listEntities`, `inspect`, `setTransform(id, Partial<TransformC>)`, `addComponent`,
`removeComponent`, `setComponent`, `setComponentField`, `pick`, `deselect`, `getSelection`,
`addEntity`, `copyEntity`, `getGizmo`/`setGizmo`, `getCamera`/`setCamera`, `listAssets`,
`getThumbnail`, `importModel`/`importTexture`, `assignAsset`, `renameAsset`,
`getEnvironment`/`setEnvironment`, `saveProject`/`loadProject`, `saveScene`/`loadScene`,
`renderStats`, plus all `set-*` render toggles). **Always NAMED params, never positional**
(the engine reads `positionalOr` from `params[name]`, `command.cppm:50-61`). **ids are
strings end-to-end.**

A **coalesced-write helper** (port of the MVP `queueTransform` throttle/coalesce +
sent/completed/inFlight counters, `wt:main.tsx:104-144`) backs all high-frequency mutations
(gizmo echo, scrub fields, sliders). State lives in a small **Zustand** store (`entities`,
`selectedId`, `sceneVersion`, `selectionVersion`, `componentsBySelected`, `assets`,
`environment`, `renderStats`, `engineStatus{running, attached, phase}`). A focus-gated
~5-10Hz **reconcile poll** reads `get-selection` (cheap, version-stamped) + `render-stats`
every tick, but only re-fetches `list-entities` when `sceneVersion` changed and the selected
`inspect` when selection/`sceneVersion` changed; writes are gated OFF during an active drag
to avoid clobbering optimistic local state. The poll also doubles as the engine-liveness
watchdog.

**Loading overlay:** an absolutely-positioned `LoadingOverlay` shown while
`engineStatus.phase !== 'ready'`. Because the reparented X11 child window **always** paints
on top of its rect, the overlay renders in a **sibling stacking layer** occupying the
viewport region while the native window is not yet mapped (the engine only `XMapRaised`'s on
a successful attach). The same sibling layer hosts any future element that must overlap the
viewport (context menus, dropdowns, the asset View modal, gizmo-mode popover) — the strategy
is to position the element **outside** the native rect or temporarily lower/unmap the native
window (the webview cannot draw over it). The bounds-sync glue (`ResizeObserver` +
window-resize + a resize-end commit calling `resize-native-viewport` on a real diff, ported
from `wt:main.tsx:156-190`) lives in `ViewportPanel` and re-fires on every dock/panel
split-resize; CSS-px bounds are multiplied by `window.scaleFactor()` before being sent.

## Phases (dependency order)

| # | Phase | File | Depends on |
|---|-------|------|-----------|
| 1 | Engine viewport-bridge forward-port + C++ editor relocation to `editor-old/` | [`phase-1-engine-viewport-bridge-forward-port-and-dir-move.md`](phase-1-engine-viewport-bridge-forward-port-and-dir-move.md) | — |
| 2 | Control-surface hardening: new editor commands + thumbnail PNG readback + schema-first DTO catalog | [`phase-2-control-surface-schema-thumbnails-new-commands.md`](phase-2-control-surface-schema-thumbnails-new-commands.md) | phase 1 |
| 3 | Build/input spikes + Tauri/React skeleton + generic passthrough + typed client + auto-start/attach + crash recovery | [`phase-3-spikes-skeleton-typed-client-autostart-lifecycle.md`](phase-3-spikes-skeleton-typed-client-autostart-lifecycle.md) | phase 2 |
| 4 | Viewport interaction unit: native gizmo overlay + billboards + input + ray-pick selection round-trip | [`phase-4-gizmo-billboards-input-selection-as-one-unit.md`](phase-4-gizmo-billboards-input-selection-as-one-unit.md) | phase 3 |
| 5 | Hierarchy panel + Create menu | [`phase-5-hierarchy-create-menu.md`](phase-5-hierarchy-create-menu.md) | phase 4 |
| 6 | Generic component inspector (schema-driven, all component types) | [`phase-6-generic-component-inspector.md`](phase-6-generic-component-inspector.md) | phase 5 |
| 7 | Asset browser (thumbnails over the wire) + pickers + drag-drop + import | [`phase-7-asset-browser-thumbnails-pickers-dragdrop.md`](phase-7-asset-browser-thumbnails-pickers-dragdrop.md) | phase 6 |
| 8 | Environment panel + Render Stats + save/load project/scene + menus | [`phase-8-environment-renderstats-fileops-menus.md`](phase-8-environment-renderstats-fileops-menus.md) | phase 7 |
| 9 | Theme, fonts, and dock-like layout parity | [`phase-9-theme-fonts-dock-layout-parity.md`](phase-9-theme-fonts-dock-layout-parity.md) | phase 8 |
| 10 | Retire the C++ ImGui editor + CMake/CI/docs finalize | [`phase-10-retire-cpp-editor-cmake-ci-docs-finalize.md`](phase-10-retire-cpp-editor-cmake-ci-docs-finalize.md) | phase 9 |

**How to read the phases.** Phase 1 forward-ports the Tier-A engine bridge and resolves the
directory move. Phase 2 hardens the control surface (new editor commands + schema-first DTO
pipeline + thumbnail PNG readback). Phase 3 runs the build-environment + input-routing
spikes, stands up the Tauri/React skeleton + generic passthrough + typed client, and
implements auto-start/auto-attach + loading overlay + runtime crash recovery. Phase 4
delivers the gizmo + billboards as one coherent unit (native overlay render + hit-test +
input + selection round-trip). Phases 5-8 port the data panels (hierarchy/create, generic
inspector, asset browser, environment/stats/file-ops). Phase 9 does theme/dock parity. Phase
10 retires the C++ ImGui editor and finalizes CMake/CI/docs. The C++ `SaffronEditor` host
keeps building (as `editor-old/`) until phase 10 so it stays runnable as the viewport host.

## Non-goals (parity-correct — the C++ editor lacks these too)

State these explicitly so they are not mistaken for gaps or read as regressions:

- **Undo/redo.**
- **Scene-graph parenting / `resolveRefs`.**
- **Multi-viewport** (and multi-viewport gizmo).
- **Native Wayland** (XWayland-only; the parent XID hard-requires Xlib).
- The **fd-export/DMA-BUF** viewport path (confirmed not forward-ported).

## Open questions

1. **Build environment (phase-3 spike, gates everything):** do Bun + cargo + tauri-cli +
   webkit2gtk run inside the `saffron-build` toolbox, or must the frontend build on the
   immutable Silverblue host with only the engine in the toolbox? This gates the phase-3
   scaffolding and the phase-10 CI; phase 3 resolves it as its first deliverable before any
   scaffolding.
2. **Keyboard-into-reparented-child input (phase-3 spike):** can WASD / W-E-R keyboard reach
   the reparented SDL child window given webview focus competition? If not, the fly-cam +
   gizmo keyboard must be driven over control commands (`get-camera`/`set-camera` +
   `set-gizmo`) instead of staying native. Phase 3 decides this with a concrete spike before
   phase 4 commits to an input model; the plan assumes a control-command fallback is
   acceptable.
3. **Gizmo input model (resolved by the phase-3 spike, implemented in phase 4):** drive
   begin/drag/end-drag + hover via a `gizmo-pointer` control command (robust, NDC) vs forward
   raw SDL pointer to the child window. Plan defaults to command-driven with raw-pointer as
   an optimization.
4. **fd-export/DMA-BUF:** confirmed not forward-ported. Revisit only if native Wayland
   (cannot `XReparent`) becomes a hard requirement. Plan accepts X11/XWayland-only.
5. **Shared types:** schema-first accepted as the durable contract; introducing named C++ DTO
   structs now (to enable `reflect-cpp` as origin) is deferred because it would refactor every
   inline-json command body. Confirm schema-first + contract test is sufficient long-term.
6. **One engine per Tauri window** (plan assumes this with per-instance pid-scoped sockets)
   vs a single engine serving multiple windows. Plan assumes one-per-window.
7. **Asset thumbnail transport:** base64 PNG inline in the JSON result (simple, larger
   payloads) vs a side fd/temp-file channel. Plan defaults to base64 PNG with client-side
   caching; revisit if payload size hurts the poll.
