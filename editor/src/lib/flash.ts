/// A transient status/error flash, the shared form of the local `flash`/`setStatus`
/// pattern the project menu and startup modal use. A panel calls `useFlash()`, shows
/// `message` in a small inline banner anchored in its own sidebar/topbar region (never
/// over the native viewport), and `flash(text)` clears itself after `ms`.
import { useCallback, useRef, useState } from "react";
import { toast } from "sonner";

export interface Flash {
  message: string | null;
  flash(message: string, ms?: number): void;
  clear(): void;
}

const DEFAULT_FLASH_MS = 4000;

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

/// A bottom-right operation toast (Sonner), for results that have no panel of
/// their own (save/load/import/screenshot).
export function notify(message: string, ms = DEFAULT_FLASH_MS): void {
  toast(message, { duration: ms });
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
