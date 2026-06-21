//! The `SubscriberList` event primitive: a hand-rolled signal/slot list with
//! stop-propagation and snapshot-iterate re-entrant safety.
//!
//! This is the engine-wide event mechanism (a `Window` exposes typed instances
//! as `on_resize`, `on_key_pressed`, …). The contract is exact and load-bearing:
//!
//! - a handler returns `true` to **stop propagation** to later subscribers;
//! - [`SubscriberList::publish`] iterates a *snapshot* of the subscriber set, so a
//!   handler may [`subscribe`](SubscriberList::subscribe) /
//!   [`unsubscribe`](SubscriberList::unsubscribe) (including itself) during
//!   dispatch without disturbing the in-flight iteration.
//!
//! Handlers are `Box<dyn FnMut(Args) -> bool>` and the list is single-thread
//! (`!Send`): every consumer dispatches on the main thread, so there is no
//! `Arc<Mutex>` around the subscriber set. Interior mutability (`RefCell`/`Cell`)
//! gives every operation a `&self` receiver, which is what lets a handler reach
//! the list to subscribe/unsubscribe itself while a `publish` is in flight.

#![deny(unsafe_code)]

use std::cell::{Cell, RefCell};

/// A token returned by [`SubscriberList::subscribe`]; pass it to
/// [`SubscriberList::unsubscribe`] to remove the handler.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct SubscriptionId(pub u64);

/// A handler is invoked with the published `Args` and returns `true` to stop
/// propagation to later subscribers.
type Handler<Args> = Box<dyn FnMut(Args) -> bool>;

struct Entry<Args> {
    id: u64,
    handler: Handler<Args>,
}

/// A signal/slot list — the engine-wide event primitive.
///
/// Subscribe a handler (any `FnMut(Args) -> bool`) to receive every
/// [`publish`](Self::publish). A handler returning `true` halts dispatch to the
/// remaining subscribers for that publish. `publish` iterates a snapshot of the
/// current subscriber ids, so a handler may subscribe or unsubscribe (itself
/// included) mid-dispatch: a handler removed during dispatch will not be invoked
/// again in the same publish, and one added during dispatch will not fire until
/// the next publish.
///
/// `Args` is the published payload — a single value, or a tuple for several
/// (e.g. `SubscriberList<(u32, u32)>`). `Args` must be `Clone` so each
/// subscriber receives its own copy.
///
/// The list is single-thread: handlers need not be `Send`, and every method
/// takes `&self` via interior mutability.
pub struct SubscriberList<Args> {
    entries: RefCell<Vec<Entry<Args>>>,
    next_id: Cell<u64>,
}

impl<Args> SubscriberList<Args> {
    /// Creates an empty subscriber list.
    pub fn new() -> Self {
        Self {
            entries: RefCell::new(Vec::new()),
            next_id: Cell::new(1),
        }
    }

    /// Registers `handler` and returns its [`SubscriptionId`].
    ///
    /// Ids are monotonic and never reused for the lifetime of the list. Safe to
    /// call from within a handler during dispatch; the new handler does not fire
    /// until the next [`publish`](Self::publish).
    pub fn subscribe(&self, handler: impl FnMut(Args) -> bool + 'static) -> SubscriptionId {
        let id = self.next_id.get();
        self.next_id.set(id + 1);
        self.entries.borrow_mut().push(Entry {
            id,
            handler: Box::new(handler),
        });
        SubscriptionId(id)
    }

    /// Removes the handler with the given id. A no-op if it is already gone.
    ///
    /// Safe to call from within a handler during dispatch — including a handler
    /// unsubscribing itself; the snapshot iteration skips an id removed this way.
    pub fn unsubscribe(&self, id: SubscriptionId) {
        self.entries.borrow_mut().retain(|entry| entry.id != id.0);
    }

    /// Returns the number of currently registered handlers.
    pub fn len(&self) -> usize {
        self.entries.borrow().len()
    }

    /// Returns `true` when no handlers are registered.
    pub fn is_empty(&self) -> bool {
        self.entries.borrow().is_empty()
    }
}

impl<Args: Clone + 'static> SubscriberList<Args> {
    /// Dispatches `args` to every subscriber in subscription order, stopping
    /// early if a handler returns `true`.
    ///
    /// Iteration is over a snapshot of the subscriber ids taken at entry. The
    /// shared `entries` borrow is released around each handler call, so a handler
    /// may subscribe or unsubscribe during dispatch: an id removed mid-dispatch
    /// is skipped, and an id added mid-dispatch is not visited until the next
    /// call.
    pub fn publish(&self, args: Args) {
        let snapshot: Vec<u64> = self.entries.borrow().iter().map(|entry| entry.id).collect();
        for id in snapshot {
            // Take the handler out under a short borrow so the handler body can
            // re-enter `subscribe`/`unsubscribe` (which borrow `entries` afresh)
            // without aliasing a live borrow. A `None` means a prior handler
            // already unsubscribed this id — skip it.
            let taken = {
                let mut entries = self.entries.borrow_mut();
                entries
                    .iter()
                    .position(|entry| entry.id == id)
                    .map(|index| std::mem::replace(&mut entries[index].handler, Box::new(no_op)))
            };
            let Some(mut handler) = taken else {
                continue;
            };

            let stop = handler(args.clone());

            // Restore the handler if it still belongs to the list (it may have
            // unsubscribed itself, in which case its slot is gone and the handler
            // is dropped here).
            if let Some(entry) = self
                .entries
                .borrow_mut()
                .iter_mut()
                .find(|entry| entry.id == id)
            {
                entry.handler = handler;
            }

            if stop {
                break;
            }
        }
    }
}

fn no_op<Args>(_args: Args) -> bool {
    false
}

impl<Args> Default for SubscriberList<Args> {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::rc::Rc;

    /// Fan-out: two subscribers both fire and accumulate (`sum == 22`,
    /// `calls == 2`).
    #[test]
    fn fan_out_invokes_every_subscriber() {
        let sum = Rc::new(Cell::new(0));
        let calls = Rc::new(Cell::new(0));

        let list: SubscriberList<i32> = SubscriberList::new();
        {
            let sum = Rc::clone(&sum);
            let calls = Rc::clone(&calls);
            list.subscribe(move |v| {
                sum.set(sum.get() + v);
                calls.set(calls.get() + 1);
                false
            });
        }
        {
            let sum = Rc::clone(&sum);
            let calls = Rc::clone(&calls);
            list.subscribe(move |v| {
                sum.set(sum.get() + v * 10);
                calls.set(calls.get() + 1);
                false
            });
        }

        list.publish(2);

        assert_eq!(sum.get(), 22, "fan-out sum");
        assert_eq!(calls.get(), 2, "both handlers fired");
    }

    /// Stop-propagation: a first handler returning `true` prevents the second
    /// from running (`first_seen == 1`, `second_seen == 0`).
    #[test]
    fn stop_propagation_halts_later_subscribers() {
        let order = Rc::new(Cell::new(0));
        let first_seen = Rc::new(Cell::new(0));
        let second_seen = Rc::new(Cell::new(0));

        let list: SubscriberList<()> = SubscriberList::new();
        {
            let order = Rc::clone(&order);
            let first_seen = Rc::clone(&first_seen);
            list.subscribe(move |()| {
                order.set(order.get() + 1);
                first_seen.set(order.get());
                true
            });
        }
        {
            let order = Rc::clone(&order);
            let second_seen = Rc::clone(&second_seen);
            list.subscribe(move |()| {
                order.set(order.get() + 1);
                second_seen.set(order.get());
                false
            });
        }

        list.publish(());

        assert_eq!(first_seen.get(), 1, "first handler ran first");
        assert_eq!(second_seen.get(), 0, "second handler was skipped");
    }

    /// Unsubscribe: after removing a handler, a publish does not invoke it.
    #[test]
    fn unsubscribe_deactivates_handler() {
        let sum = Rc::new(Cell::new(0));

        let list: SubscriberList<i32> = SubscriberList::new();
        let first = {
            let sum = Rc::clone(&sum);
            list.subscribe(move |v| {
                sum.set(sum.get() + v);
                false
            })
        };
        {
            let sum = Rc::clone(&sum);
            list.subscribe(move |v| {
                sum.set(sum.get() + v * 10);
                false
            });
        }

        list.unsubscribe(first);
        sum.set(0);
        list.publish(1);

        assert_eq!(sum.get(), 10, "only the surviving handler fired");
    }

    /// Re-entrant self-unsubscribe: a handler that unsubscribes itself during
    /// dispatch fires exactly once across two publishes (the snapshot guarantee).
    #[test]
    fn reentrant_self_unsubscribe_fires_once() {
        let fired = Rc::new(Cell::new(0));
        let list: Rc<SubscriberList<()>> = Rc::new(SubscriberList::new());

        let id_slot: Rc<Cell<SubscriptionId>> = Rc::new(Cell::new(SubscriptionId(0)));
        let id = {
            let fired = Rc::clone(&fired);
            let list_ref = Rc::clone(&list);
            let id_slot = Rc::clone(&id_slot);
            list.subscribe(move |()| {
                fired.set(fired.get() + 1);
                list_ref.unsubscribe(id_slot.get());
                false
            })
        };
        id_slot.set(id);

        list.publish(());
        list.publish(());

        assert_eq!(fired.get(), 1, "self-unsubscribing handler fired once");
    }

    /// A handler subscribed mid-dispatch does not fire until the next publish.
    #[test]
    fn subscribe_during_dispatch_defers_to_next_publish() {
        let outer_calls = Rc::new(Cell::new(0));
        let inner_calls = Rc::new(Cell::new(0));
        let list: Rc<SubscriberList<()>> = Rc::new(SubscriberList::new());

        {
            let outer_calls = Rc::clone(&outer_calls);
            let inner_calls = Rc::clone(&inner_calls);
            let list_ref = Rc::clone(&list);
            list.subscribe(move |()| {
                outer_calls.set(outer_calls.get() + 1);
                if outer_calls.get() == 1 {
                    let inner_calls = Rc::clone(&inner_calls);
                    list_ref.subscribe(move |()| {
                        inner_calls.set(inner_calls.get() + 1);
                        false
                    });
                }
                false
            });
        }

        list.publish(());
        assert_eq!(
            inner_calls.get(),
            0,
            "new handler deferred past current publish"
        );

        list.publish(());
        assert_eq!(
            inner_calls.get(),
            1,
            "new handler fires on the next publish"
        );
        assert_eq!(
            outer_calls.get(),
            2,
            "original handler fired both publishes"
        );
    }

    /// Multi-arg payloads via a tuple (the `SubscriberList<u32, u32>` shape that
    /// `Window::on_resize` uses) deliver every component to each subscriber.
    #[test]
    fn tuple_payload_delivers_all_components() {
        let seen = Rc::new(RefCell::new(Vec::new()));

        let list: SubscriberList<(u32, u32)> = SubscriberList::new();
        {
            let seen = Rc::clone(&seen);
            list.subscribe(move |(w, h)| {
                seen.borrow_mut().push((w, h));
                false
            });
        }

        list.publish((1280, 720));

        assert_eq!(&*seen.borrow(), &[(1280u32, 720u32)]);
    }
}
