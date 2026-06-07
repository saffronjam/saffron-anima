/// Commit an inline edit when the pointer goes down anywhere outside the input.
/// Blur alone misses this in the webview: clicks do not reliably move focus off
/// the input (buttons never take focus, and the asset-grid marquee preventDefaults
/// its pointerdown), so "click elsewhere" must commit explicitly.
import { useEffect, useRef } from "react";

export function useOutsideCommit(
  ref: React.RefObject<HTMLElement | null>,
  commit: () => void,
): void {
  const commitRef = useRef(commit);
  commitRef.current = commit;
  useEffect(() => {
    const onPointerDown = (event: PointerEvent): void => {
      const el = ref.current;
      if (el && event.target instanceof Node && !el.contains(event.target)) {
        commitRef.current();
      }
    };
    document.addEventListener("pointerdown", onPointerDown, true);
    return () => document.removeEventListener("pointerdown", onPointerDown, true);
  }, [ref]);
}
