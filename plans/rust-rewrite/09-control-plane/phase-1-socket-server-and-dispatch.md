# Phase 1 — `saffron-control`: the rustix socket server, dispatch, and the registry

**Status:** COMPLETED

**Depends on:** 00-foundations:phase-4-json-crate, 10-protocol-codegen (saffron-protocol DTO crate), 08-host-and-viewport (the walking-skeleton host that calls `poll_control` once per frame)

## Goal

Stand up the `saffron-control` crate: the synchronous, single-threaded, drain-once-per-frame
JSON-over-`AF_UNIX` server, the dispatch envelope, the fn-pointer command registry, the
`EngineContext` borrow seam, and the two builtin commands `ping` and `help`. This is the runnable
spine the editor connects to — after this phase, the host answers `ping` over the real socket with the
frozen envelope, and `help` lists the (so-far two) registered commands. The five domain phases then
register their handlers onto this registry.

## Why this shape (NO LEGACY)

- **Synchronous polled drain, NO tokio.** The C++ server is a non-blocking single-threaded loop polled
  once per frame from the host main loop (`pollControl` → `drainControlServer`); there is no async
  runtime, worker pool, or background thread. The locked ground rules and feasibility §4.6 are explicit
  that this is kept. Adding tokio would be a second concurrency model for a job that is fundamentally
  "answer requests between frames on the main thread" — forbidden. The Rust loop is the same shape over
  `rustix`: `accept4` loop → per-client `recv(MSG_DONTWAIT)` accumulate → split on `\n` → `dispatch` →
  `send(MSG_NOSIGNAL)` flush loop → `retain` live clients.
- **The send flush loop is ported intact, not simplified.** The client socket is non-blocking, so a
  single `send` short-writes any reply larger than the socket buffer (a multi-frame profiler capture)
  and silently drops the tail; the client then never sees the `\n` terminator and hangs. The loop
  sends until the whole reply is flushed, `poll`-waiting on `POLLOUT` (1000ms) when the buffer fills,
  dropping the client on a fatal non-`EINTR` error (`control_server.cpp:305`–`322`). This is a real
  bug the C++ code already fixed; the port keeps the fix.
- **`MSG_NOSIGNAL` keeps a vanished client from killing the engine** — a `SIGPIPE` on `send` to a gone
  peer would terminate the process. `rustix`'s `SendFlags::NOSIGNAL` reproduces it with no
  signal-handler hack.
- **The registry is a fn-pointer table, not a vtable hierarchy.** `CommandRegistry` =
  `Vec<Command>` + `HashMap<String, usize>`, `Command = { name, help, run: Box<dyn Fn(&mut
  EngineContext, &Value) -> Result<Value>> }` — the per-command registration record idiom from
  `conventions.md`. Insertion order is preserved (`help` iterates it; the manifest is generated in it).
  The typed registration helper `register_typed::<P, R>` mirrors the C++ `registerCommand<Params,
  Result>` template: deserialize `P`, run the typed closure, serialize `R` — so every later handler
  gets the frozen lenient/positional read and decimal-string emit for free (§6 of the README).
- **`EngineContext` is a borrow struct, never stored.** `{ window: &mut Window, renderer: &mut
  Renderer, scene_edit: &mut SceneEditContext, assets: &mut AssetServer, physics: Option<&mut
  PhysicsWorld> }`, assembled in `poll_control` and dropped at the end of the drain. No ownership, no
  `Arc`/`RefCell` — the host owns the subsystems and lends them for the frame. Disjoint-field borrows
  let a handler hold `&mut` to two subsystems at once.
- **No `unsafe`.** `rustix` wraps the syscalls safely, so `saffron-control` keeps
  `#![deny(unsafe_code)]` — it is not on the FFI exception list. FDs are `OwnedFd`; the socket path is
  `unlink`ed on stop and on rebind (the C++ `::unlink(path)` before `bind`, `control_server.cpp:180`).
- **The `JSON_NOEXCEPTION` abort firewall and `DtoTag<T>` dispatch are deleted** — serde returns
  `Result`; the type parameter resolves the DTO directly. `parse_json`/`dump_json` come from
  `saffron-json`.

## Grounding (real files/symbols)

- `engine-old/source/saffron/control/control_server.cpp`
  - `controlSocketPath` (`:160`) — `SAFFRON_CONTROL_SOCK` → `$XDG_RUNTIME_DIR/saffron-control.sock` →
    `/tmp/saffron-control-<uid>.sock`.
  - `startControlServer` (`:173`) — `socket(AF_UNIX, SOCK_STREAM|NONBLOCK|CLOEXEC)`, `unlink`, `bind`
    (path-length guard), `chmod 0600`, `listen(8)`.
  - `stopControlServer` (`:205`) — close clients, close listen fd, `unlink` path.
  - `dispatch` (`:226`) — echo `id`, find command, run, build `{ok, result|error}`.
  - `drainControlServer` (`:251`) — `accept4` loop, per-client `recv(MSG_DONTWAIT)`, `\n` split,
    the `send(MSG_NOSIGNAL)` flush loop with `poll(POLLOUT, 1000)`, `erase_if(fd<0)`.
  - `newControlContext`/`destroyControlContext`/`pollControl` (`:329`–`371`) — registry+server
    ownership, the once-per-frame entry, the bind-failure-is-non-fatal path (logs a warning, runs
    inactive).
- `engine-old/source/saffron/control/command.cppm`
  - `EngineContext` (`:31`), `CommandTraits`/`CommandRegistry` (`:42`/`:49`), `registerCommand` +
    typed template (`:55`/`:58`), `findCommand` (`:77`), `positionalOr` (`:81`), `ControlContext`
    (`:134`).
- `help` registration: `control_commands_render.cpp:488` (untyped, iterates `reg.rows`).
- `schemas/control/envelope.schema.json` — the reply shape; `PingParams`/`PingResult` in
  `control_dto.cppm` (`:192`/`:200`).
- `09-control-plane/README.md` §1–§5; `09-control-plane/catalog.md` (the `ping`/`help` rows).

## Acceptance gate

- `cargo build -p saffron-control` and the full workspace build are green; the crate depends on
  `saffron-core`, `saffron-json`, `saffron-protocol`, `rustix`, and the subsystem crates it borrows
  (`window`, `rendering`, `scene`, `sceneedit`, `assets`, `physics`) per the foundations crate graph.
- Crate root `#![deny(unsafe_code)]`; `cargo clippy -p saffron-control` and `cargo fmt --check` clean.
- `cargo test -p saffron-control` passes named unit/integration tests:
  - **socket round-trip** — a test client connects to a server bound to a temp path, sends
    `{"id":1,"cmd":"ping","params":{}}\n`, and reads back exactly one `\n`-terminated line that
    deserializes to `{id:1, ok:true, result:{...}}`.
  - **envelope on unknown command** — `{"cmd":"nope"}` returns `{ok:false, error:"unknown command
    'nope'"}` with `id` echoed (here absent → `null`).
  - **invalid JSON** — a non-JSON line returns `{ok:false, error:"invalid JSON request"}`.
  - **id echo** — `id` round-trips for a number, a string, and an absent id (→ `null`).
  - **flush loop** — a reply larger than the socket buffer is fully delivered (terminated by exactly
    one trailing `\n`); the test reads until `\n` and asserts the whole payload arrived.
  - **`help`** — returns `{commands:[{name:"ping",...},{name:"help",...}]}` in registration order.
  - **path resolution** — `control_socket_path` honors `SAFFRON_CONTROL_SOCK`, then `XDG_RUNTIME_DIR`,
    then the `/tmp` fallback (env-var-driven, no real bind needed).
- A wire-driven e2e check (the `tests/e2e` harness, `13-testing-and-verification`) boots the headless
  host and gets a `ping` reply over the real socket with a validation-clean log — the walking-skeleton
  control assertion.
