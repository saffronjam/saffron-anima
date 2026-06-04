/// Topbar-anchored transient operation notifications.
import { useEffect, useRef, useState } from "react";
import { subscribeNotifications } from "../lib/flash";

export function Notifications() {
  const [message, setMessage] = useState<string | null>(null);
  const timer = useRef<ReturnType<typeof setTimeout> | null>(null);

  useEffect(() => {
    return subscribeNotifications(({ message: next, ms }) => {
      if (timer.current !== null) {
        clearTimeout(timer.current);
      }
      setMessage(next);
      timer.current = setTimeout(() => {
        timer.current = null;
        setMessage(null);
      }, ms);
    });
  }, []);

  useEffect(() => {
    return () => {
      if (timer.current !== null) {
        clearTimeout(timer.current);
      }
    };
  }, []);

  if (!message) {
    return <div className="h-7 min-w-0 flex-1" aria-hidden="true" />;
  }

  return (
    <div className="flex h-7 min-w-0 flex-1 justify-end" role="status" aria-live="polite">
      <div
        className="max-w-[min(520px,42vw)] truncate rounded-md border border-border bg-popover px-3 py-1 text-xs text-popover-foreground shadow-sm"
        title={message}
      >
        {message}
      </div>
    </div>
  );
}
