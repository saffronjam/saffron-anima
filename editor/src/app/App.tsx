/// Top-level editor shell. Wires the Tauri lifecycle events to the store, starts
/// the reconcile poll + the global W/E/R gizmo shortcuts, and composes the chrome
/// (MenuBar + Topbar) above the resizable dock `Layout` and a status bar below.
/// The dock arrangement (Hierarchy + tabbed Inspector/Environment/Stats on the
/// left, Assets bottom, Viewport center) lives in `Layout`; the embedded viewport's
/// LoadingOverlay is a sibling inside ViewportPanel, never a panel the native window
/// paints over.
import { useEffect } from "react";
import { listen, type UnlistenFn } from "@tauri-apps/api/event";
import { client } from "../control/client";
import { startReconcile, useEditorStore } from "../state/store";
import { MenuBar } from "./MenuBar";
import { Topbar } from "../panels/Topbar";
import { Layout } from "./Layout";
import { useGizmoShortcuts } from "./useGizmoShortcuts";
import { TooltipProvider } from "@/components/ui/tooltip";

type EnginePhaseEvent = "starting" | "attaching";

export function App() {
  const setPhase = useEditorStore((s) => s.setPhase);
  const phase = useEditorStore((s) => s.engineStatus.phase);

  // W/E/R → translate/rotate/scale, gated off while a text field is focused.
  useGizmoShortcuts();

  // Subscribe to the Rust-emitted lifecycle events. StrictMode double-mounts in
  // dev, so the cleanup must unlisten idempotently.
  useEffect(() => {
    let disposed = false;
    const unlisteners: UnlistenFn[] = [];

    const register = async (): Promise<void> => {
      const offPhase = await listen<EnginePhaseEvent>("engine-phase", (event) => {
        setPhase(event.payload);
      });
      const offError = await listen<string>("viewport-error", (event) => {
        setPhase("error", event.payload);
      });
      if (disposed) {
        offPhase();
        offError();
        return;
      }
      unlisteners.push(offPhase, offError);
    };

    void register();

    return () => {
      disposed = true;
      for (const off of unlisteners) {
        off();
      }
    };
  }, [setPhase]);

  // Start the focus-gated reconcile poll once; it self-gates on phase === 'ready'.
  useEffect(() => {
    const stop = startReconcile(client);
    return stop;
  }, []);

  return (
    <TooltipProvider delayDuration={300}>
      <div className="flex h-full flex-col overflow-hidden">
        <MenuBar />
        <Topbar />
        <Layout />
        <footer className="flex h-[22px] flex-none items-center justify-end border-t border-border bg-card px-3">
          <span className="font-mono text-[10px] uppercase tracking-wide text-muted-foreground">
            {phase}
          </span>
        </footer>
      </div>
    </TooltipProvider>
  );
}
