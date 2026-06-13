/// Mouse Back / Forward (the side buttons) → undo / redo on the active tab's history,
/// and the suppression of the webview's default Back/Forward navigation that would
/// otherwise reload the single-page app and drop all editor state. W3C button numbers:
/// 3 = Back, 4 = Forward.
///
/// A window-level `pointerdown` listener so the buttons act over every panel, not only
/// the viewport. `pointerdown` (not `auxclick`) because the viewport captures the
/// pointer during a gizmo/fly drag, which suppresses `auxclick` per spec. Gated like the
/// key shortcuts EXCEPT it never skips on a focused text field — a side-button click is
/// not typing, so Back should still undo while an input has focus. The button mapping is
/// fixed, not rebindable: the keybinding registry keys off `event.key`/`event.code` and
/// has no notion of a pointer button.
import { useEffect } from "react";
import { useEditorStore } from "../state/store";

export function useMouseHistoryNav(): void {
  useEffect(() => {
    const onPointerDown = (event: PointerEvent): void => {
      if (event.button !== 3 && event.button !== 4) {
        return;
      }
      // Always stop the webview's default Back/Forward navigation (an SPA reload).
      event.preventDefault();
      const store = useEditorStore.getState();
      if (store.settingsOpen || store.engineStatus.phase !== "ready") {
        return;
      }
      if (event.button === 3) {
        void store.undo();
      } else {
        void store.redo();
      }
    };
    window.addEventListener("pointerdown", onPointerDown);
    return () => {
      window.removeEventListener("pointerdown", onPointerDown);
    };
  }, []);
}
