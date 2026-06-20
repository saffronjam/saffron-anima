# Phase 3 — `saffron-signal`: the hand-rolled event primitive

**Status:** COMPLETED

**Depends on:** 00-foundations:phase-2-core-crate

## Goal

Port `Saffron.Signal` into `saffron-signal`: the generic `SubscriberList` signal/slot primitive that
is the engine-wide event mechanism (`Window` exposes typed instances as `onResize`, `onKeyPressed`,
…). No off-the-shelf crate matches its exact contract — handler-returns-`bool`-to-stop-propagation
plus snapshot-iteration so a handler can sub/unsub itself mid-dispatch — so it is a deliberate ~60-line
hand-roll. The C++ `runSignalSelfTest` (the four-case oracle) is *deleted as a runtime function* and
re-expressed as `#[test]`s.

## Why this shape (NO LEGACY)

- **Hand-rolled, not a crate dependency.** Surveyed event/signal crates either drop the
  stop-propagation return contract or are not re-entrant-safe during dispatch. The C++ contract is
  precise and load-bearing (a `Window` resize handler may add/remove handlers); reproducing it exactly
  in ~60 lines is simpler and lower-risk than bending a crate. This is the `conventions.md` §6
  decision made concrete.
- **Snapshot-iterate is preserved verbatim as the re-entrancy guarantee.** `publish` iterates a
  *snapshot* of the subscriber set, so a handler that subscribes/unsubscribes (including itself)
  during dispatch cannot invalidate iteration (`signal.cppm:42`). The re-entrant self-unsubscribe case
  (`runSignalSelfTest`, `signal.cppm:119`) is the test that pins this: a handler that unsubscribes
  itself must fire exactly once across two publishes.
- **`bool`-returns-stop-propagation is kept** — `true` from a handler halts dispatch to later
  subscribers (`signal.cppm:50`). This is explicit control flow the engine relies on (an event
  consumer that "handles" an input stops it reaching lower layers); it is not replaced with an
  always-fan-out model.
- **Single-thread `FnMut` handlers, not `Send + Sync`.** The signal consumers (`Window` typed
  signals) all dispatch on the main thread; there is no cross-thread `publish`. So handlers are
  `Box<dyn FnMut(Args) -> bool>` and the type is `!Send` by default — no needless `Arc<Mutex>` around
  the list. If a later area proves a cross-thread signal exists, it gets its own `Send` variant then
  (NO LEGACY: we do not pre-emptively make everything `Send`).
- **`SubscriptionId(u64)` newtype**, the monotonic `next_id` counter, and `subscribe`/`unsubscribe`/
  `publish` map 1:1 to the C++ methods. The self-test becomes `#[cfg(test)]` units — there is no
  `run_signal_self_test` symbol and nothing runs it at startup (`conventions.md` §8).

## Grounding (real files/symbols)

- `engine-old/source/saffron/signal/signal.cppm`
  - `SubscriptionId` newtype (`:9`), `SubscriberList<Args...>` with the `Entry { id, handler }`
    record (`:17`), `next_id` (`:23`).
  - `subscribe` (`:29`, returns a `SubscriptionId`, monotonic id), `unsubscribe` (`:37`,
    `erase_if` by id), `publish` (`:42`, snapshot-iterate + stop-on-`true`).
  - `runSignalSelfTest` (`:61`) — the four-case oracle to port *into* `#[test]`s: (1) fan-out sum
    (`:66`–`:84`), (2) stop-propagation order (`:86`–`:109`), (3) unsubscribe deactivation
    (`:111`–`:117`), (4) re-entrant self-unsubscribe firing once (`:119`–`:134`).
- `conventions.md` §6 (the locked contract: stop-propagation + snapshot re-entrancy + single-thread
  `FnMut`), §8 (self-tests → `#[test]`).
- Consumer note: `Saffron.Window` (`window.cppm`) holds the typed signal instances; this phase only
  ports the primitive, `saffron-window` (a later area) wires the typed signals onto it.

## Acceptance gate

- `cargo build -p saffron-signal` and the full workspace build are green;
  `saffron-signal` depends only on `saffron-core`.
- `cargo test -p saffron-signal` passes the four ported oracle tests:
  - **fan-out** — two subscribers both fire; the accumulated value matches the C++ `sum == 22`,
    `calls == 2` case.
  - **stop-propagation** — a first handler returning `true` prevents the second from running
    (`firstSeen == 1`, `secondSeen == 0`).
  - **unsubscribe** — after `unsubscribe(first)`, a `publish` does not invoke the removed handler.
  - **re-entrant self-unsubscribe** — a handler that unsubscribes itself during dispatch fires exactly
    once across two `publish` calls (the snapshot-iteration guarantee).
- `cargo clippy -p saffron-signal` and `cargo fmt --check` clean; crate root `#![deny(unsafe_code)]`.
