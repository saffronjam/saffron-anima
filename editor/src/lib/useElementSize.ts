/// Track an element's content-box size via a single `ResizeObserver`. Returns `{0,0}` until the
/// first observation, so callers should seed their derived state with a sensible first-paint
/// default. Lightweight on purpose — for responsive layout flips, not viewport/subsurface bounds
/// (that is `useSubsurfaceBounds`, which talks to the Wayland presenter).
import { useEffect, useState, type RefObject } from "react";

export function useElementSize<T extends HTMLElement>(
  ref: RefObject<T | null>,
): { width: number; height: number } {
  const [size, setSize] = useState({ width: 0, height: 0 });
  useEffect(() => {
    const el = ref.current;
    if (!el) {
      return;
    }
    const observer = new ResizeObserver((entries) => {
      const rect = entries[0]?.contentRect;
      if (rect) {
        setSize((prev) =>
          prev.width === rect.width && prev.height === rect.height
            ? prev
            : { width: rect.width, height: rect.height },
        );
      }
    });
    observer.observe(el);
    return () => observer.disconnect();
  }, [ref]);
  return size;
}
