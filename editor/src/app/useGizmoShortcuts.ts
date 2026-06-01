/// Global W/E/R keyboard shortcuts mapping to the gizmo operation (translate /
/// rotate / scale), matching the C++ editor's W/E/R cycle (editor_gizmo.cpp).
///
/// INPUT MODEL (spike-0b, recorded in the phase-3 migration plan): the editor input
/// is **control-command-driven** — the webview owns the DOM and forwards intent to
/// the engine over the control socket; the engine's reparented X11 child does NOT
/// receive raw keyboard from the webview. So the webview is the right place to bind
/// W/E/R: it sets `store.gizmo` optimistically and fires `set-gizmo` (mirroring the
/// Topbar buttons). If spike-0b had chosen raw-keyboard-into-the-child-window the
/// engine would already handle W/E/R and this hook would double-fire — it does not.
///
/// The handler is gated OFF while a text input / textarea / select / contentEditable
/// is focused, so typing a value (e.g. an entity name or a number field) never
/// retargets the gizmo.
import { useEffect } from "react";
import { client } from "../control/client";
import { useEditorStore } from "../state/store";
import type { GizmoState } from "../protocol";

type GizmoOp = GizmoState["op"];

const KEY_TO_OP: Record<string, GizmoOp> = {
  w: "translate",
  e: "rotate",
  r: "scale",
};

/// True when the active element is a text-entry control, so shortcuts must not fire.
function isTextEntryFocused(): boolean {
  const el = document.activeElement;
  if (!el || !(el instanceof HTMLElement)) {
    return false;
  }
  if (el.isContentEditable) {
    return true;
  }
  const tag = el.tagName;
  return tag === "INPUT" || tag === "TEXTAREA" || tag === "SELECT";
}

export function useGizmoShortcuts(): void {
  useEffect(() => {
    const onKeyDown = (event: KeyboardEvent): void => {
      // Let modified chords (Ctrl/Cmd/Alt) through — they belong to menus / the OS.
      if (event.ctrlKey || event.metaKey || event.altKey) {
        return;
      }
      if (isTextEntryFocused()) {
        return;
      }
      const op = KEY_TO_OP[event.key.toLowerCase()];
      if (!op) {
        return;
      }
      // Only meaningful once the engine is live; the gizmo is engine state.
      if (useEditorStore.getState().engineStatus.phase !== "ready") {
        return;
      }
      event.preventDefault();
      // Optimistic local update + the command; the reconcile poll's get-gizmo read
      // keeps it in sync with any external mutation (e.g. `se set-gizmo`).
      useEditorStore.getState().setGizmo({ op });
      void client.setGizmo({ op }).catch(() => {});
    };

    window.addEventListener("keydown", onKeyDown);
    return () => {
      window.removeEventListener("keydown", onKeyDown);
    };
  }, []);
}
