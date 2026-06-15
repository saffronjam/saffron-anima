export module Saffron.Signal;

import std;
import Saffron.Core;

export namespace sa
{
    // Token returned by subscribe(); pass to unsubscribe().
    struct SubscriptionId
    {
        u64 value = 0;
    };

    // A signal/slot list — the engine-wide event primitive. A handler returns
    // true to stop propagation to later subscribers (explicit, like Go control flow).
    // This is a struct with methods (a Go-style method set), not a class hierarchy.
    template <typename... Args>
    struct SubscriberList
    {
        struct Entry
        {
            u64 id = 0;
            std::function<bool(Args...)> handler;
        };

        std::vector<Entry> entries;
        u64 nextId = 1;

        auto subscribe(std::function<bool(Args...)> handler) -> SubscriptionId
        {
            SubscriptionId id{ nextId };
            nextId = nextId + 1;
            entries.push_back(Entry{ id.value, std::move(handler) });
            return id;
        }

        void unsubscribe(SubscriptionId id)
        {
            std::erase_if(entries, [&](const Entry& entry) -> auto { return entry.id == id.value; });
        }

        void publish(Args... args) const
        {
            // Iterate a snapshot so a handler may safely subscribe/unsubscribe
            // during dispatch (which would otherwise invalidate the vector).
            std::vector<Entry> snapshot = entries;
            for (const Entry& entry : snapshot)
            {
                bool stop = entry.handler(args...);
                if (stop)
                {
                    break;
                }
            }
        }
    };

    /// Headless self-test for SubscriberList dispatch: fan-out, stop-propagation, unsubscribe, and a
    /// handler that unsubscribes itself mid-dispatch (the snapshot must keep iteration safe). Gated by
    /// SAFFRON_SELFTEST in the host; returns Err on a mismatch, logs OK otherwise.
    auto runSignalSelfTest() -> Result<void>
    {
        SubscriberList<int> list;
        int sum = 0;
        int calls = 0;
        const SubscriptionId first = list.subscribe(
            [&](int v) -> bool
            {
                sum = sum + v;
                calls = calls + 1;
                return false;
            });
        list.subscribe(
            [&](int v) -> bool
            {
                sum = sum + v * 10;
                calls = calls + 1;
                return false;
            });
        list.publish(2);
        if (sum != 22 || calls != 2)
        {
            return Err(std::format("signal self-test: fan-out wrong (sum={}, calls={})", sum, calls));
        }

        SubscriberList<> stopList;
        int order = 0;
        int firstSeen = 0;
        int secondSeen = 0;
        stopList.subscribe(
            [&]() -> bool
            {
                order = order + 1;
                firstSeen = order;
                return true;
            });
        stopList.subscribe(
            [&]() -> bool
            {
                order = order + 1;
                secondSeen = order;
                return false;
            });
        stopList.publish();
        if (firstSeen != 1 || secondSeen != 0)
        {
            return Err(
                std::format("signal self-test: stop-propagation failed (first={}, second={})", firstSeen, secondSeen));
        }

        list.unsubscribe(first);
        sum = 0;
        list.publish(1);
        if (sum != 10)
        {
            return Err(std::format("signal self-test: unsubscribe left the handler active (sum={})", sum));
        }

        SubscriberList<> reentrant;
        int fired = 0;
        SubscriptionId self{};
        self = reentrant.subscribe(
            [&]() -> bool
            {
                fired = fired + 1;
                reentrant.unsubscribe(self);
                return false;
            });
        reentrant.publish();
        reentrant.publish();
        if (fired != 1)
        {
            return Err(std::format("signal self-test: self-unsubscribe during dispatch fired {} times", fired));
        }

        logInfo("signal self-test OK");
        return {};
    }
}
