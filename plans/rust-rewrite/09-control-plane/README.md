# 09 — Control plane: the synchronous socket server and the 153-command surface

The control plane is the engine's entire external API: a newline-delimited JSON-over-unix-socket
server that the Tauri editor and the `sa` CLI use to drive and inspect the running host. It is the
**frozen wire boundary** — the editor (frontend and the already-Rust `editor/src-tauri/`) speaks this
protocol unchanged, so the Rust port reproduces the envelope, the encodings, and every command's
observable behavior byte-for-byte. This area ports `Saffron.Control` into `saffron-control`, the DTO
types into the standalone `saffron-protocol` crate, and the socket loop onto `rustix` with **no tokio**.

`saffron-control` is the integration hub: its handlers reach into six already-Rust subsystems through
an `EngineContext`, so it ports **last** in the global order — after rendering, scene, sceneedit,
assets, animation, and physics all exist. The walking-skeleton milestone (PP-10/PP-14) lands an
earlier *minimal* server (`ping` + the framing) so the editor sees a live engine; this area then
fleshes that spine out to the full surface.

Read this with [`catalog.md`](./catalog.md) (the authoritative 153-command + 236-DTO list that the
codegen, the CLI, and the e2e fixtures all consume) and `00-foundations/conventions.md` (the idiom
rules this area obeys). The DTO crate's derive mechanics belong to `10-protocol-codegen` (PP-7); this
area defines the *server*, the *handlers*, and the *frozen contract* the derive must satisfy.

---

## 1. The frozen wire contract (what must not drift)

The protocol is defined by `schemas/control/envelope.schema.json` (hand-authored, the only
hand-authored schema) and the DTO-generated OpenRPC + manifest. The Rust port keeps all of it
identical. Four things fail *silently* if they drift — they are the reason this area's acceptance
gates assert raw bytes, not just round-trips:

- **The envelope.** Request `{ "id"?, "cmd": "<name>", "params": { ... } }`; reply
  `{ "id": <echoed>, "ok": true|false, "result": { ... } | "error": "<msg>" }`. `id` echoes whatever
  the request sent (any json, including absent → `null`); `ok` is required; exactly one of
  `result`/`error` accompanies it. Grounded in `dispatch` (`control_server.cpp:226`) and
  `envelope.schema.json`.
- **Decimal-string u64 ids.** Every id crosses the wire as a JSON *decimal string* (the JS 2^53
  limit), emitted by `uuidToJson` and accepted leniently (string **or** number, whole-string parse)
  by `readWireUuid`. A default `u64` would emit a JSON number and silently corrupt the id on a JS
  client; the contract test checks raw bytes. This is owned jointly by `saffron-json` (the imperative
  helper) and `saffron-protocol` (the `serde_with::PickFirst<(DisplayFromStr, _)>` derive on the
  `Uuid` newtype) — both must emit byte-identically.
- **Enum kebab-case spelling.** Every `*Dto`/preset enum is a kebab-case string on the wire
  (`"point-light"`, `"rotate"`, `"timestamps"`); an unknown value is a typed error, not a default.
- **Lenient param reads + positional args.** A handler accepts `--name value` (object key) **or** a
  bare positional (the index-th element of `params.args`); booleans accept `"0"/"false"/"off"` and
  numbers; `f32` reads narrow an f64 wire value. The `fieldValue`/`requiredField`/`optionalField`
  readers in the generated serde and `positionalOr` (`command.cppm:81`) encode this; the Rust DTO
  deserialize path must reproduce it (PP-7 decides whether it is a custom `Deserialize` or a
  pre-pass that flattens `args` into keys before serde runs — see §6).

Path resolution is also frozen: `SAFFRON_CONTROL_SOCK` if set, else
`$XDG_RUNTIME_DIR/saffron-control.sock`, else `/tmp/saffron-control-<uid>.sock`, mode `0600`
(`controlSocketPath`, `control_server.cpp:160`).

## 2. The server model: synchronous, single-threaded, drain-once-per-frame

The C++ server is a **non-blocking, single-threaded, polled** loop — there is no async runtime, no
worker pool, no background thread. `pollControl` is called once per frame from the host's main loop
with the live subsystem references; it accepts pending clients, drains readable bytes, splits on
`\n`, runs each request **on the calling (main) thread**, and writes one compact JSON line back. This
model is kept verbatim — **NO tokio, no async** (the feasibility study §4.6 and the locked ground
rules are explicit). It ports 1:1 onto `rustix`:

| C++ (`control_server.cpp`) | Rust (`rustix` / std) |
|---|---|
| `socket(AF_UNIX, SOCK_STREAM\|NONBLOCK\|CLOEXEC)` + `bind` + `chmod 0600` + `listen(8)` | `rustix::net::{socket_with, bind, listen}` with `SocketFlags::NONBLOCK\|SocketFlags::CLOEXEC`; `rustix::fs::chmodat` |
| `accept4(NONBLOCK\|CLOEXEC)` in a loop until `EAGAIN` | `rustix::net::accept_with(SocketFlags::NONBLOCK\|SocketFlags::CLOEXEC)` loop |
| `recv(MSG_DONTWAIT)` accumulating into `inbuf` | `rustix::net::recv(RecvFlags::DONTWAIT)` |
| split on `\n`, `parseJson` each line, `dispatch` | same: `split('\n')`, `serde_json::from_str`, dispatch |
| `send(MSG_NOSIGNAL)` in a flush loop, `poll(POLLOUT, 1000)` when the buffer fills | `rustix::net::send(SendFlags::NOSIGNAL)` loop, `rustix::event::poll` on `POLLOUT` |
| `std::erase_if(clients, fd<0)` | `Vec::retain` |

The **send flush loop is load-bearing** and must be ported intact: the client socket is non-blocking,
so a single `send` short-writes any reply larger than the socket buffer (e.g. a multi-frame profiler
capture) and silently drops the tail — the client then never sees the `\n` and hangs. The loop sends
until the whole reply is flushed, `poll`-waiting for writability when the buffer fills, dropping the
client on a fatal error and ignoring `EINTR` (`control_server.cpp:305`–`322`). The 5s reply budget the
pre-plan cites is the per-`poll(POLLOUT)` 1000ms wait repeated — there is no separate timer; the Rust
port keeps the same `poll`-with-1000ms-timeout shape.

`MSG_NOSIGNAL` matters: a vanished client must not raise `SIGPIPE` and kill the engine. Rust gets this
from `SendFlags::NOSIGNAL` on `send` (the same flag, no signal-handler hack needed).

The server owns no `unsafe`: `rustix` wraps the raw syscalls safely, so `saffron-control` keeps
`#![deny(unsafe_code)]` (the FFI exception list is rendering/physics-sys/host only). File descriptors
are `OwnedFd`/`RawFd` from `rustix`; client cleanup is `Drop` on the owned fds plus the explicit
`unlink` of the socket path on server stop (`stopControlServer`, `control_server.cpp:205`).

## 3. The phase split

The server scaffolding ports first (it is the runnable spine the editor needs), then the five command
domains port as independent, separately-gateable increments. Each domain is one phase because each is
a coherent slice with a distinct `EngineContext` reach and a distinct upstream-subsystem dependency,
and each can land green (its commands answer, its fixtures pass) without the others. The DTO crate
(`saffron-protocol`) is shared infrastructure owned by `10-protocol-codegen`; this area's phases
*consume* it and add the handler logic.

| Phase | What | Depends on | Commands |
|---|---|---|---|
| `phase-1-socket-server-and-dispatch` | `saffron-control` crate: the `rustix` socket server, the framing/drain/flush loop, `dispatch`, the `CommandRegistry` (fn-pointer table), `EngineContext`, `ping`/`help` | protocol crate, host walking-skeleton | `ping`, `help` |
| `phase-2-render-commands` | render-stats/profiler/perf/alarms/AA/GI toggles/viewport-native, exposure, probes | rendering | 29 |
| `phase-3-scene-commands` | entity lifecycle, components (registry-driven), selection/pick/inspect, camera/gizmo/fly, play-state, environment, scripting drains/schema/overrides, `quit` | scene, sceneedit, scripting | 47 |
| `phase-4-asset-commands` | import/catalog/thumbnails, project/scene save+load, materials, asset-preview, screenshot, active-view | assets, sceneedit, rendering | 52 |
| `phase-5-animation-commands` | playback, clips, skeleton overlay, debug overlays, foot-IK, joint pick | animation, sceneedit | 13 |
| `phase-6-physics-commands` | physics-state/bodies, impulse, contacts, kinematic bones, character move, raycast/shapecast, ragdoll | physics, sceneedit | 12 |

Registration order is preserved (`registerBuiltinCommands`: render → scene → asset → animation →
physics, `control.cppm`/`control_server.cpp:151`) because `help` iterates the registry in insertion
order and the manifest/OpenRPC are generated in that order — a contract test compares against it.

The cross-domain rows in `catalog.md` (`quit`, `set-exposure`, the probe commands, the script-domain
commands) are assigned to a phase by the C++ **registration file** they live in, not the manifest's
flat order: `set-exposure`/`set-probes`/`recapture-probes`/`list-probes` are render-file commands
(phase 2); `quit` and `drain-script-*`/`get-script-*`/`set-script-override`/`create-script` are
scene-file commands (phase 3).

## 4. EngineContext: the live-state seam

`EngineContext` is the slice of live engine state a handler may touch — references only, rebuilt fresh
each frame, never stored (`command.cppm:31`). In C++ it is
`{ Window&, Renderer&, SceneEditContext&, AssetServer&, PhysicsWorld* }`. The Rust port is a struct of
`&mut`/`&` borrows assembled in `poll_control` and passed to each handler; it holds **no ownership**
and lives only for the drain call. The nullable `physics` (live play world or null in Edit) becomes
`Option<&mut PhysicsWorld>`.

The measured reach (from the `ctx.*` grep, recorded in `catalog.md`):

- **render** → `renderer` only.
- **scene** → `sceneEdit` (heavily), `renderer`, `assets`.
- **asset** → `assets`, `sceneEdit`, `renderer`, `window` (the highest-coupling domain).
- **animation** → `sceneEdit`, `assets`, `renderer`.
- **physics** → `physics` (nullable), `sceneEdit`.

This map *is* the reason control ports last and the reason each domain phase lists its upstream
dependency. It also surfaces the one ownership subtlety the Rust port must resolve: a single handler
can need `&mut` to two subsystems at once (e.g. `assign-asset` touches `assets` + `sceneEdit` +
`renderer`). Because `EngineContext` holds distinct fields, disjoint-borrow through the struct is
borrow-checker-legal — the handler takes `ctx: &mut EngineContext` and Rust permits simultaneous
`&mut ctx.assets` and `&mut ctx.scene_edit` as separate fields. No `RefCell` is needed at this seam
(the host owns the subsystems; `EngineContext` just borrows them for the frame).

The `renderer` field is `&mut dyn ControlRenderer` (the concrete `Renderer` is not headless-buildable
on lavapipe). Beyond the render-domain query/toggle methods (phase 2), the trait carries the
asset/scene-domain renderer seam those phases need: **view-select**
(`set_active_view` + `view_desired_size`/`set_view_desired_size`, the per-`ViewId` desired-size pair —
this replaced the old active-only `set_viewport_size`, which is gone), **screenshot**
(`capture_viewport(path)`), **wait-gpu-idle**, and the **GPU-upload** access point
(`with_gpu_uploader(&mut dyn FnMut(&dyn GpuUploader))`) that hands the asset loaders
(`import_texture`, `load_mesh_asset`, `resolve_material_asset`, `pick_entity`, the preview floor, …) a
transient upload seam for the call's duration. The concrete impl is the host's `HostControlRenderer`
(`saffron-host`), which bundles `&mut Renderer` with the host-owned one-off `Uploader` (the renderer
owns none) and builds a `RendererUploader` inside `with_gpu_uploader`. `pick_entity`
(`07-assets-and-materials`) was decoupled from the full `SceneRenderer` to `&dyn GpuUploader` +
`(width, height)` so it reaches through this single seam — picking needs only the AABB mesh upload +
the aspect ratio, not the per-frame render driver.

> **All four seams are now LIVE (06-rendering substrate built — see
> `06-rendering/phase-16-capture-shm-profiler.md` "Deferred seams closed"):** the thumbnail render
> (`ThumbnailGpu` impl over `Renderer::encode_*_thumbnail_png`), the material-preview render
> (`render_material_preview` with the codegen `_preview.spv` argument), the window-capture request
> (`request_window_capture`), and the project `renderSettings` serde
> (`ProjectHost::render_settings_to_json` / `apply_render_settings`). The asset commands that route
> through them — `get-thumbnail` / `view-asset`, `preview-render`, `screenshot {window}`,
> `save-project`'s render block — are functional. `screenshot {window}` validates live under the
> toolbox weston (it needs a real present surface, so it skips off a display).

## 5. The command registry: fn-pointer table, not `std::function` vtable

The C++ `CommandRegistry` is a `vector<CommandTraits>` + a `byName` map, where each `CommandTraits` is
`{ name, help, std::function<Result<json>(EngineContext&, const json&)> }`, and the typed
`registerCommand<Params, Result>` wraps a typed handler in a closure that parses params → runs →
serializes the result (`command.cppm:42`–`76`). This is a per-command registration record keyed by
name — exactly the "registration table of fn-pointers" idiom in `conventions.md` (§ on `std::function`
itables). The Rust port:

- A `CommandRegistry` = `Vec<Command>` + `HashMap<String, usize>` (insertion order preserved for `help`
  + manifest parity), where `Command = { name: &'static str, help: &'static str, run: HandlerFn }`.
- `HandlerFn` is a boxed `dyn Fn(&mut EngineContext, &Value) -> Result<Value>` (the handlers are
  `!Send`, single-thread-confined — same as C++). A typed registration helper mirrors the C++
  template: `register_typed::<P, R>(name, help, |ctx, p: P| -> Result<R>)` deserializes `P` from the
  params `Value`, runs the closure, serializes `R` back to `Value`. The typed wrapper is where the
  lenient/positional param read and the decimal-string emit happen, so all 153 handlers get the frozen
  encoding for free.
- `register_builtin_commands` calls the five `register_*_commands` in the frozen order. Whether the
  registry is built once at startup (it has no per-frame mutation) and stored on the host, with
  `EngineContext` rebuilt each frame — exactly the C++ `ControlContext { registry, server, active }`
  shape (`command.cppm:134`).

`help` is the one untyped command (returns a raw `{ commands: [{name, help}] }` array by iterating the
registry) — it is the manifest's lone `skip` (reason: "reflective registry"). It ports as a plain
untyped `register` over `&registry`, kept exactly so the editor's command palette still works.

## 6. What the codegen owns vs what this area owns (the boundary)

`10-protocol-codegen` (PP-7) owns the `saffron-protocol` crate: the 236 DTO structs + 17 enums as
Rust types deriving `serde`/`schemars`/`ts-rs`, the `Uuid` newtype derive, and the OpenRPC/manifest
emitter that regenerates `schemas/control/*.generated.json` + `editor/src/protocol/sa-types.ts`. The
command list itself (the 153 name→params→result triples) lives in the codegen's source (the C++
`commands: CommandDef[]` in `gen.ts`); in Rust it becomes a single registration site that both the
codegen and the runtime read (the inventory/registration-macro discipline PP-7 designs).

**This area owns** the server, the drain/dispatch/flush loop, `EngineContext`, the registry, and the
153 handler bodies — the logic that turns a parsed `Params` DTO into a `Result<ResultDto>` by reaching
into the subsystems. The handlers depend on the DTO crate but not on the codegen tooling.

One decision this area pins for PP-7 to honor: **the lenient/positional param read is a wire-contract
behavior, not a per-handler concern**, so it must live in the typed-registration wrapper (a pre-pass
that flattens `params.args[i]` into keys by DTO field order, then runs serde), not be re-implemented in
each handler. The C++ `fieldValue`/`positionalOr` proves this is uniform across all commands.

## 7. Subtractions (NO LEGACY)

- **`control_dto_serde.generated.cpp` (167KB of hand-generated parse/serialize) is deleted** — serde
  derives replace it entirely (PP-7). The `JSON_NOEXCEPTION` abort-firewall reason for the imperative
  readers is gone (serde returns `Result`).
- **No tokio / no async runtime** — the synchronous polled drain is kept; adding async would be a
  second model for the same job (forbidden).
- **No second framing/transport** — newline-delimited JSON over `AF_UNIX` is the one wire; no
  length-prefix variant, no message-pack, no websocket.
- **The `DtoTag<T>` template dispatch** (`parseDto(params, DtoTag<Params>{})`, `command.cppm:64`)
  disappears — Rust resolves `P: Deserialize` by the type parameter directly.
- **`viewIdFromWire`/`viewIdWire`** (the `"scene"`/`"assetPreview"` ↔ `ViewId` mapping,
  `command.cppm:85`) ports as a small `enum ViewId` + `FromStr`/`Display`, the single translation
  place, kept (not duplicated per call site).

## 8. Grounding (real files / symbols)

| What | File | Symbols |
|---|---|---|
| Module re-export | `engine-old/source/saffron/control/control.cppm` | `Saffron.Control`, `:Dto`, `:Command` |
| Registry, context, typed register, selectors | `engine-old/source/saffron/control/command.cppm` | `EngineContext`, `CommandTraits`, `CommandRegistry`, `registerCommand<Params,Result>`, `positionalOr`, `viewIdFromWire`/`viewIdWire`, `resolveEntity`, `entityRefDto`, `ControlContext`, `pollControl` |
| Socket server, dispatch, drain/flush | `engine-old/source/saffron/control/control_server.cpp` | `controlSocketPath`, `startControlServer`, `stopControlServer`, `dispatch`, `drainControlServer`, `newControlContext`, `pollControl` |
| DTO source of truth (236 structs, 17 enums, wire-helpers) | `engine-old/source/saffron/control/control_dto.cppm` | `WireUuid`, `EntitySelector`, `AssetSelector`, `EntityRef`, all `*Params`/`*Result`/`*Dto`, the enums |
| Generated serde (deleted in Rust) | `engine-old/source/saffron/control/control_dto_serde.generated.cpp` | `readWireUuid`, `uuidToJson`, `fieldValue`/`requiredField`/`optionalField`, `readBool`/`readF32`/`readU32`/`readWireUuid`, the per-enum `read*`/`*Name` |
| Render handlers | `engine-old/source/saffron/control/control_commands_render.cpp` | `registerRenderCommands`, `help`, `aaModeDto`/`applyAaMode` |
| Scene handlers | `engine-old/source/saffron/control/control_commands_scene.cpp` | `registerSceneCommands`, `findByName` (component registry), `resolveEntity` use |
| Asset handlers | `engine-old/source/saffron/control/control_commands_asset.cpp` | `registerAssetCommands` |
| Animation handlers | `engine-old/source/saffron/control/control_commands_animation.cpp` | `registerAnimationCommands` |
| Physics handlers | `engine-old/source/saffron/control/control_commands_physics.cpp` | `registerPhysicsCommands`, `physicsWorldStats`, `listPhysicsBodies` |
| Envelope schema (hand-authored, kept) | `schemas/control/envelope.schema.json` | `Envelope` |
| Generated contract artifacts (regenerated by PP-7) | `schemas/control/{openrpc,command-manifest}.generated.json` | 153 commands + 1 skip |
| JSON wire helpers | `engine-old/source/saffron/json/json.cppm` | `uuidToJson`, `jsonU64`, `jsonStringOr`, `parseJson`, `dumpJson` |
