# Rust house style — the idiom translation table

This document replaces the Go-flavored `CONVENTIONS.md` wholesale. It is the authoritative house
style for the Rust engine and the single reference every other area cites for "how does C++ construct
X port?" The rule for each construct is **decided here, once**, so the port is mechanical and a
reviewer can predict the shape of any translation.

The output reads like Rust written by a Rust author, not C++ transliterated. We do not carry the
free-function-over-method habit, the `Result<T, std::string>`-everywhere habit, the no-`?:` rule, or
the manual itable structs across — those existed because C++ lacked the better tool, and Rust has it.

---

## 1. The translation table

| C++ construct (engine-old) | Rust idiom | Rule |
|---|---|---|
| `Result<T> = std::expected<T, std::string>`, `Err("msg")` | `Result<T, E>` with a typed `enum E` (`thiserror`) | **Typed errors per crate.** See §2. No `Result<T, String>` carry-over. |
| check-`Result`-immediately, no unchecked propagation | the `?` operator | `?` *is* the immediate-check; propagation is now idiomatic and safe. |
| `Ref<T> = std::shared_ptr<T>` (read-shared) | `Arc<T>` | Default. A handle shared for reading after construction. |
| `Ref<T>` mutated through the shared handle | `Arc<Mutex<T>>` / `Arc<RwLock<T>>` | **Per-site decision.** See §3. The cascading one. |
| `Ref<T>` shared in a single-thread graph (no `Send`) | `Rc<RefCell<T>>` | Only where `Send` is provably unneeded (host overlay state). |
| `std::unique_ptr<T>`, sole owner | `Box<T>` or just `T` by value | Prefer owning by value; `Box` only for indirection/`dyn`. |
| `enum class E { A, B }` | `enum E { A, B }` | Direct. |
| `std::variant<A, B, C>` (tagged union) | `enum E { A(A), B(B), C(C) }` | Data-carrying enum; `switch`+`std::get` → `match`. A net win. |
| `std::optional<T>` | `Option<T>` | Direct. |
| struct of `std::function` used as a runtime interface (`Layer`) | a `trait` + `dyn`/`impl` | See §4. |
| struct of `std::function` as a per-type registration record | a registry of fn-pointers or `Box<dyn Fn>` keyed by type | See §4 (component/command tables). |
| small closed set of behaviors | `enum` + `match` | Prefer over `dyn` when the set is closed and known. |
| move-only RAII wrapper (Vulkan handle, file) | a struct with `impl Drop` | Move is the default in Rust; copy is opt-in. See §5. |
| `waitGpuIdle()` before teardown, manual `Ref` drop in `onExit` | designed `Drop` *order* (field order / explicit `drop`) | The ordering is a type-design concern, detailed in PP-10. |
| `SubscriberList<Args...>` | a hand-rolled generic events type in `saffron-signal` | See §6. |
| free-function "constructor" `newThing(...) -> Thing` | `impl Thing { fn new(...) -> Self }` / `From` / `Default` | Associated functions / trait impls where idiomatic. |
| free function transforming a plain-data struct | a method `impl T { fn f(&self) }` *where it reads well*, else a free fn | Methods are allowed and preferred when they read naturally; no dogma either way. |
| `camelCase` function `submitModel` | `snake_case` `submit_model` | |
| `PascalCase` type `RenderGraph` | `PascalCase` `RenderGraph` | Unchanged. |
| `PascalCase` enum value `ImageFormat::Rgba16f` | `PascalCase` variant `ImageFormat::Rgba16f` | Unchanged. |
| `PascalCase` constant `MaxFramesInFlight` | `SCREAMING_SNAKE_CASE` `MAX_FRAMES_IN_FLIGHT` | Rust const convention. |
| `snake_case.cppm` file | `snake_case.rs` file | Unchanged spelling, `.rs`. |
| one `sa::` namespace | one crate per area; `crate::` paths | Modules within a crate via `mod`. |
| `JSON_NOEXCEPTION` abort firewall | `serde` returning `Result` | The firewall disappears — serde never aborts. |
| `Uuid { u64 value }`, serialized as decimal string | a `Uuid(u64)` newtype, `serde_with` decimal-string | See §7. |
| ban on `?:` ternary | `if/else` *or* an `if`-expression `let x = if c { a } else { b }` | The C++ ban is dropped; Rust `if`-as-expression is idiomatic. |
| inline self-test function run at startup | `#[cfg(test)] mod tests` / `tests/` | **No runtime self-tests.** See §8. |

---

## 2. Error model — typed enums per crate, `?` to propagate

Each library crate defines its own error `enum` with `thiserror::Error`:

```rust
#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error("invalid JSON: {0}")]
    Parse(String),
    #[error("missing key '{0}'")]
    MissingKey(String),
    // ...
}
pub type Result<T> = std::result::Result<T, Error>;
```

- A crate exports its own `Result<T>` alias bound to its own `Error`. The C++ `Result<T>` alias
  pattern is kept — but typed, not stringly.
- Cross-crate composition uses `#[from]`: `saffron-scene`'s error has a
  `#[from] saffron_json::Error` variant, so `?` lifts a json error into a scene error with no manual
  conversion. This replaces the C++ habit of restringing every error at each boundary.
- **`anyhow` is allowed only in `[[bin]]` crates and `xtask`** (the top of the call stack, where a
  typed error buys nothing). Library crates never expose `anyhow::Error` in a public signature.
- A `String` payload inside a variant is fine when the underlying failure genuinely has no structure
  (a parser message); the *point* is that the variant is typed so callers can `match`, not that every
  message is structured.
- The C++ "MUST check immediately, never propagate unchecked" rule is satisfied by `?` itself —
  there is no unchecked `Result` in Rust (it warns/denies on `#[must_use]`).

## 3. `Ref` → `Arc` vs `Arc<Mutex>` — the cascading ownership policy

`std::shared_ptr` permits shared *mutation*; `Arc` does not. So `Ref<T>` does not uniformly become
`Arc<T>`. The decision is **per declaration site**, by this policy:

1. **`Arc<T>`** — the value is fully constructed, then only *read* through every shared handle
   (immutable assets after load: `Arc<GpuMesh>`, `Arc<Material>`, a loaded clip). This is the common
   case and the default.
2. **`Arc<Mutex<T>>`** — the value is mutated through a shared handle from *more than one thread*, or
   from re-entrant call paths that alias it. The C++ engine already names these sites with explicit
   mutexes: `gpuQueueMutex()` and `bindlessMutex()` (`renderer_types.cppm:33,42`) guard the GPU queue
   and the bindless descriptor table — those become `Arc<Mutex<Queue>>` / `Arc<Mutex<BindlessTable>>`.
   The thumbnail worker thread sharing the queue with the main thread is the canonical multi-thread
   shared-mutable case.
3. **`Arc<RwLock<T>>`** — same as (2) but read-dominated (many readers, rare writer); use only with a
   measured read/write ratio, not by default.
4. **`Rc<RefCell<T>>`** — shared-mutable but provably single-threaded (`!Send`), e.g. the host's
   per-frame CPU overlay/gizmo state that never crosses a thread. Cheaper than `Arc<Mutex>`; chosen
   only when `Send` is not required.

**Where to decide:** at the *struct field / function signature* that holds the `Ref`, not by global
search-replace. Each area's README records its own `Ref` sites and their bucket. The renderer and the
asset cache (the two hardest) are detailed in their own areas (PP-5, PP-7/assets); 00 only fixes the
policy. Negative caches (`Ref` that may be null) become `Option<Arc<T>>` and a negative cache map is
`HashMap<u64, Option<Arc<T>>>` (feasibility §3 assets).

## 4. `std::function` itables → traits, fn-tables, or enums

Three distinct C++ patterns, three distinct Rust answers:

- **The `Layer` struct of optional closures** (`onAttach/onUpdate/onRender/...`) → a `trait Layer`
  with provided (default-empty) methods, stored as `Box<dyn Layer>`. A client implements only the
  hooks it needs. This is the "Go-interface-as-itable" pattern expressed as a real trait. (Detailed in
  PP-10, decided here as: trait, not an enum of layer kinds — the set is open/client-extensible.)
- **Per-type registration records** (the component / command "traits" structs of `std::function`
  keyed by type) → a registration table: a `HashMap`/`inventory` of typed descriptors holding
  `fn` pointers or `Box<dyn Fn>`. Adding a component/command registers in **one** place (PP-7's
  derive/macro replaces the C++ four-place hand-sync).
- **A small, closed set of behaviors dispatched by tag** → a data-carrying `enum` + `match`, not
  `dyn`. Prefer this whenever the variants are known at compile time (it monomorphizes and is
  exhaustive-checked).

The choice rule: **open/extensible set → trait object; closed/known set → enum; per-type metadata →
registration table.**

## 5. Move-only RAII → `Drop`

- A type owning an external resource (Vulkan handle, `memfd`, socket) implements `Drop` to free it.
  Rust types are move-only by default and non-`Copy` unless derived, so the C++ "deleted copy,
  defaulted move" boilerplate evaporates.
- **Teardown order is a design concern, not free.** A use-after-free from dropping the device before
  the allocator is a *runtime* bug, not a compile error. The order is fixed by struct field order
  (fields drop in declaration order) or an explicit `Drop` impl that drops in sequence. The C++
  `waitGpuIdle()`-before-teardown choreography becomes the host type's `Drop`/field layout (PP-10).
- Never implement `Drop` just to log; only for resource release.

## 6. `SubscriberList` — hand-rolled, exact contract preserved

No crate matches the contract, so `saffron-signal` hand-rolls it (~60 lines). The contract:

- `subscribe(handler) -> SubscriptionId` where the handler returns `bool` (`true` = **stop
  propagation** to later subscribers — explicit, matching the C++ semantics).
- `unsubscribe(id)` removes by token.
- `publish(args)` iterates a **snapshot** of the subscriber list so a handler may `subscribe` /
  `unsubscribe` (including itself) *during* dispatch without invalidating iteration. This re-entrant
  safety is load-bearing — `runSignalSelfTest` (`signal.cppm:61`) tests exactly this (the
  self-unsubscribing handler must fire once across two publishes).

Rust shape: handlers are `Box<dyn FnMut(&Args...) -> bool>` (or `Fn` if the events crate must be
`Send`/`Sync`; decided in phase-3 by whether `Window` signals cross threads — they do not, so
single-thread `FnMut` is the default). `SubscriptionId(u64)` is a newtype. `publish` clones the entry
list (or uses a generation/retain-while-iterating scheme) to preserve snapshot semantics.

## 7. `Uuid` and the decimal-string wire contract

- `Uuid(pub u64)` newtype, deriving `Clone, Copy, PartialEq, Eq, Hash, Debug`.
- `Uuid::new()` mints from a thread-local RNG over `[1024, u64::MAX]` (the `<1024` reservation for
  built-in/synthetic assets is preserved from `newUuid`, `core.cppm:79`).
- **The wire encoding is frozen:** a `Uuid` serializes to JSON as a *decimal string*
  (`uuidToJson` = `std::to_string(value)`, `json.cppm:72`) because ids span the full u64 range past
  JS's 2^53 safe integer. On read it accepts a string **or** a number (`jsonU64`, `json.cppm:92`).
  In Rust this is `serde_with::PickFirst<(DisplayFromStr, _)>` on the field (emit string, accept
  either) — the protocol crate (PP-7) owns the exact attribute; `saffron-core` only defines the
  newtype and a documented note pointing at PP-7. Getting this wrong emits a JSON *number* and
  silently fails the `assertRawU64` contract test.

## 8. Tests — no runtime self-tests

- Unit tests live inline as `#[cfg(test)] mod tests { ... }` in the same file as the code under test.
- Cross-crate / wire-level tests live in a crate's `tests/` directory or in the e2e harness.
- **Every C++ in-engine self-test is deleted and re-expressed as `#[test]`.** The first instance in
  this area is `runSignalSelfTest` (`signal.cppm:61`): its four cases (fan-out sum, stop-propagation
  order, unsubscribe deactivation, re-entrant self-unsubscribe) become four `#[test]` functions or one
  test with four asserts in `saffron-signal`. There is no `runSignalSelfTest` function in the Rust
  tree, and nothing runs it at startup.

## 9. Naming and file layout, concretely

- Files: `snake_case.rs`. A crate's root is `src/lib.rs` (lib) or `src/main.rs` (bin). Submodules are
  `src/<name>.rs` or `src/<name>/mod.rs`.
- Functions/methods/locals/fields: `snake_case`. Types/traits/enum-variants: `PascalCase`.
  Consts/statics: `SCREAMING_SNAKE_CASE`. Crate identifiers: `saffron_<area>` (the `-` in the package
  name is normalized to `_`).
- Doc comments: `///` on public items (the Rust equivalent of the C++ `///` on exported
  declarations). Same minimalism rule: brief, contract-focused, no banner/section dividers, no
  change-journey notes ("previously/used to/now"). `//!` for crate/module-level docs.
- No `mod.rs`-only-to-re-export ceremony where a flat module reads better; keep the tree shallow.
- `#![deny(unsafe_code)]` at every crate root **except** the three FFI crates (`saffron-physics-sys`,
  `saffron-rendering`'s ash seam, the shm publisher in `saffron-host`), which `#![allow(unsafe_code)]`
  with a top-of-file justification comment naming the seam.
