export module Saffron.Signal;

import std;
import Saffron.Core;

export namespace se
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

        SubscriptionId subscribe(std::function<bool(Args...)> handler)
        {
            SubscriptionId id{ nextId };
            nextId = nextId + 1;
            entries.push_back(Entry{ id.value, std::move(handler) });
            return id;
        }

        void unsubscribe(SubscriptionId id)
        {
            std::erase_if(entries, [&](const Entry& entry) { return entry.id == id.value; });
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
}
