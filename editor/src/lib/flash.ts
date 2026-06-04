/// A transient status/error flash, the shared form of the local `flash`/`setStatus`
/// pattern the project menu and startup modal use. A panel calls `useFlash()`, shows
/// `message` in a small inline banner anchored in its own sidebar/topbar region (never
/// over the native viewport), and `flash(text)` clears itself after `ms`.
import { useCallback, useRef, useState } from "react";

export interface Flash {
  message: string | null;
  flash(message: string, ms?: number): void;
  clear(): void;
}

const DEFAULT_FLASH_MS = 4000;
const NOTIFY_EVENT = "saffron:notify";

export interface NotificationPayload {
  message: string;
  ms: number;
}

export function useFlash(): Flash {
  const [message, setMessage] = useState<string | null>(null);
  const timer = useRef<ReturnType<typeof setTimeout> | null>(null);

  const clear = useCallback((): void => {
    if (timer.current !== null) {
      clearTimeout(timer.current);
      timer.current = null;
    }
    setMessage(null);
  }, []);

  const flash = useCallback((next: string, ms = DEFAULT_FLASH_MS): void => {
    if (timer.current !== null) {
      clearTimeout(timer.current);
    }
    setMessage(next);
    timer.current = setTimeout(() => {
      timer.current = null;
      setMessage(null);
    }, ms);
  }, []);

  return { message, flash, clear };
}

export function notify(message: string, ms = DEFAULT_FLASH_MS): void {
  window.dispatchEvent(
    new CustomEvent<NotificationPayload>(NOTIFY_EVENT, { detail: { message, ms } }),
  );
}

export function subscribeNotifications(
  handler: (payload: NotificationPayload) => void,
): () => void {
  const listener = (event: Event): void => {
    if (!(event instanceof CustomEvent)) {
      return;
    }
    const detail = event.detail as Partial<NotificationPayload>;
    if (typeof detail.message === "string") {
      handler({ message: detail.message, ms: detail.ms ?? DEFAULT_FLASH_MS });
    }
  };
  window.addEventListener(NOTIFY_EVENT, listener);
  return () => window.removeEventListener(NOTIFY_EVENT, listener);
}

/// Normalize a rejected control call into a readable message. The Rust passthrough
/// rejects with the engine's error string (e.g. "ray tracing not supported …").
export function errorText(err: unknown): string {
  if (typeof err === "string") {
    return err;
  }
  if (err instanceof Error) {
    return err.message;
  }
  return String(err);
}
