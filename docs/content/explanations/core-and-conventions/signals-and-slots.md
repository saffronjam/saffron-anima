+++
title = 'Signals'
weight = 6
+++

# Signals

A signal is a typed broadcast channel: a producer publishes an event, and any number of subscribed
handlers receive it in turn. The pattern decouples the producer from its consumers — the producer
knows nothing about who listens, only the event's payload type.

Anima expresses this with one type, `SubscriberList<Args>` in `saffron-signal`. A handler may
stop the event from reaching the rest, which makes the list both a fan-out channel and a
prioritized chain. The window reports input through it, and selection changes ripple through the
editor on it.

## The shape

```rust
pub struct SubscriberList<Args> { /* entries + next id, behind RefCell/Cell */ }

impl<Args> SubscriberList<Args> {
    pub fn subscribe(&self, handler: impl FnMut(Args) -> bool + 'static) -> SubscriptionId;
    pub fn unsubscribe(&self, id: SubscriptionId);
}

impl<Args: Clone + 'static> SubscriberList<Args> {
    pub fn publish(&self, args: Args);
}
```

`Args` is the event payload, fixed at the type. A single value carries one thing; a tuple carries
several — `SubscriberList<(u32, u32)>` carries a resize's width and height. `Args` must be `Clone`
to `publish` (each subscriber receives its own copy). The list is single-thread (`!Send`): every
consumer dispatches on the main thread, so the entries sit behind `RefCell`/`Cell` interior
mutability and every method takes `&self` — which is what lets a handler reach the list to
subscribe or unsubscribe itself mid-dispatch.

## Subscription tokens

`subscribe` stores the handler under a monotonically increasing id and returns a `SubscriptionId`,
a thin `u64` newtype. The caller holds it to call `unsubscribe(id)` later. Ids only ever increase
and are never reused, so a stale token cannot match a newer subscription.

## Stop-propagation dispatch

A handler returns `bool` to mean "stop here". `publish` walks the subscribers in subscription order
and breaks the moment one returns `true`, so each list is also a priority chain. The decision is
explicit: returning `true` is a visible statement in the handler, not a hidden `consumed` flag
mutated elsewhere. A handler claims an event this way — returning `true` when it wants a keystroke
or click, so later handlers never see it.

## Snapshot iteration

A handler may subscribe or unsubscribe *during* dispatch, including unsubscribing itself. `publish`
makes that safe by iterating a **snapshot** of the subscriber ids taken at entry, and releasing the
shared `entries` borrow around each handler call. The set of handlers for this publish is fixed at
the moment it starts: an id removed mid-dispatch is skipped, and one added mid-dispatch does not
fire until the next publish. That removes a whole class of reentrancy bug for the price of one id
vector per publish — cheap for the handful of subscribers these lists carry.

## Where it is used

The [window](../../app-lifecycle-and-window/window-and-events/) owns the most-used lists: `on_close`,
`on_resize`, `on_key_pressed`, `on_key_released`, `on_file_dropped`, and a raw `on_raw_event` the
gizmo and editor camera feed off. The editor uses a `SubscriberList` keyed on the selected entity,
so the scene-edit state and the gizmo stay in sync without knowing about each other.

## In the code

| What | File | Symbols |
|---|---|---|
| The primitive | `engine/crates/signal/src/lib.rs` | `SubscriberList`, `SubscriptionId` |
| Subscribe / unsubscribe | `engine/crates/signal/src/lib.rs` | `subscribe`, `unsubscribe` |
| Stop-propagation dispatch | `engine/crates/signal/src/lib.rs` | `publish` (snapshot + stop) |
| Typed window signals | `engine/crates/window/src/lib.rs` | `on_resize`, `on_key_pressed`, `on_raw_event` |

> [!NOTE]
> A subscribe/unsubscribe done inside a handler doesn't change who else receives the *current*
> event — `publish` froze the id set before the loop. The change lands on the next publish.

## Related

- [Rust house style](../go-flavored-design/) — events as a plain struct with a method set
- [Window and events](../../app-lifecycle-and-window/window-and-events/) — the typed signals built on this
