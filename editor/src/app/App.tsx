/// Top-level editor shell. Wires the Tauri lifecycle events to the store, starts
/// the reconcile poll + the global W/E/R gizmo shortcuts, and composes the chrome
/// above the resizable dock `Layout` and a status bar below.
/// The dock arrangement (Hierarchy + tabbed Inspector/Environment/Stats on the
/// left, Assets bottom, Viewport center) lives in `Layout`; the embedded viewport's
/// LoadingOverlay is a sibling inside ViewportPanel, never a panel the native window
/// paints over. The dock stays mounted while an asset tab is active (display:none),
/// so split ratios, scroll positions, and the viewport survive tab navigation.
import { useEffect, useState } from "react";
import { listen, type UnlistenFn } from "@tauri-apps/api/event";
import { getCurrentWindow } from "@tauri-apps/api/window";
import { client } from "../control/client";
import { loadEditorSettings, startReconcile, useEditorStore } from "../state/store";
import type { AssetEntry } from "../protocol";
import { Topbar } from "../panels/Topbar";
import { Layout } from "./Layout";
import { WindowTitlebar } from "./WindowTitlebar";
import { useGizmoShortcuts } from "./useGizmoShortcuts";
import { TooltipProvider } from "@/components/ui/tooltip";
import { ProjectStartupModal } from "./ProjectStartupModal";
import { SettingsModal } from "./SettingsModal";
import type { ProjectInfo } from "../control/client";
import { AssetPreview } from "../components/AssetViewer";
import { CaptureFlame } from "../components/CaptureFlame";
import { MaterialGraphEditor } from "../panels/MaterialGraphEditor";
import { emitLayoutSettled } from "./layoutBus";
import { logRender } from "../lib/renderLog";
import { Toaster } from "@/components/ui/sonner";
import { cn } from "@/lib/utils";

type EnginePhaseEvent = "starting" | "attaching";

let didRevealWindow = false;
let revealWindowPromise: Promise<void> | null = null;

function revealEditorWindow(): Promise<void> {
  if (revealWindowPromise === null) {
    revealWindowPromise = getCurrentWindow()
      .show()
      .then(() => {
        didRevealWindow = true;
      });
  }
  return revealWindowPromise;
}

export function App() {
  logRender("App");
  const setPhase = useEditorStore((s) => s.setPhase);
  const setProject = useEditorStore((s) => s.setProject);
  const setViewportHidden = useEditorStore((s) => s.setViewportHidden);
  const phase = useEditorStore((s) => s.engineStatus.phase);
  const activeViewTabId = useEditorStore((s) => s.activeViewTabId);
  const projectPath = useEditorStore((s) => s.project?.path);
  const activeKind = useEditorStore(
    (s) => s.viewTabs.find((candidate) => candidate.id === s.activeViewTabId)?.kind ?? "scene",
  );
  const activeAsset = useEditorStore((s) => {
    const tab = s.viewTabs.find((candidate) => candidate.id === s.activeViewTabId);
    return tab?.kind === "asset"
      ? (s.assets.find((asset) => asset.id === tab.assetId) ?? null)
      : null;
  });
  const activeGraphMaterialId = useEditorStore((s) => {
    const tab = s.viewTabs.find((candidate) => candidate.id === s.activeViewTabId);
    return tab?.kind === "materialGraph" ? tab.materialId : null;
  });
  const [revealed, setRevealed] = useState(didRevealWindow);
  const [projectModalOpen, setProjectModalOpen] = useState(false);
  const sceneTabActive = activeViewTabId === "scene";

  // W/E/R → translate/rotate/scale, gated off while a text field is focused.
  useGizmoShortcuts();

  useEffect(() => {
    if (didRevealWindow) {
      return;
    }
    let cancelled = false;
    void revealEditorWindow().finally(() => {
      if (!cancelled) {
        requestAnimationFrame(() => setRevealed(true));
      }
    });
    return () => {
      cancelled = true;
    };
  }, []);

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

  // Hydrate the keybinding overrides from appdata/settings.json once at startup
  // (editor-wide state, independent of the engine phase).
  useEffect(() => {
    void loadEditorSettings();
  }, []);

  useEffect(() => {
    let raf = 0;
    let last = performance.now();
    let sampleStart = last;
    let frames = 0;
    let averageMs = 0;

    const tick = (now: number): void => {
      const delta = now - last;
      last = now;
      frames += 1;
      averageMs = averageMs === 0 ? delta : averageMs * 0.9 + delta * 0.1;

      if (now - sampleStart >= 500) {
        const hz = (frames * 1000) / (now - sampleStart);
        useEditorStore.getState().setUiFrameStats(hz, averageMs);
        sampleStart = now;
        frames = 0;
      }

      raf = requestAnimationFrame(tick);
    };

    raf = requestAnimationFrame(tick);
    return () => cancelAnimationFrame(raf);
  }, []);

  useEffect(() => {
    if (phase !== "ready") {
      return;
    }
    let cancelled = false;
    const syncProject = async (): Promise<void> => {
      try {
        const [info, project] = await Promise.all([client.appDataInfo(), client.getProject()]);
        if (cancelled) {
          return;
        }
        setProject(project.loaded ? project : null);
        setProjectModalOpen(!project.loaded && !info.envProject && !info.autoEmptyProject);
      } catch {
        if (!cancelled) {
          setProjectModalOpen(false);
        }
      }
    };
    void syncProject();
    return () => {
      cancelled = true;
    };
  }, [phase, setProject]);

  const handleProjectLoaded = (project: ProjectInfo): void => {
    setProject(project);
    setProjectModalOpen(false);
  };

  useEffect(() => {
    if (sceneTabActive) {
      setViewportHidden(false);
      requestAnimationFrame(() => emitLayoutSettled({ force: true }));
      return;
    }
    setViewportHidden(true);
  }, [sceneTabActive, setViewportHidden]);

  // The single bridge to the presenter's park state: whatever sets the store flag
  // (the asset View modal, the asset workspace tab), the subsurface follows — even
  // while the dock is display:none and the ViewportPanel's host rect is 0x0.
  const viewportHidden = useEditorStore((s) => s.viewportHidden);
  useEffect(() => {
    void client.setViewportHidden(viewportHidden).catch(() => {});
  }, [viewportHidden]);

  return (
    <TooltipProvider delayDuration={300}>
      <div
        className="flex h-full min-w-[900px] flex-col overflow-hidden transition-opacity duration-300 ease-out"
        style={{ opacity: revealed ? 1 : 0 }}
      >
        <WindowTitlebar />
        {/* The dock is hidden, never unmounted, while an asset tab is active: its
            in-memory layout state survives, and the ViewportPanel's host rect goes
            0x0 (computeBounds skips degenerate rects) while viewportHidden parks
            the subsurface. The key remounts the dock once per project so the
            persisted per-project layout applies. */}
        <div className={cn("flex min-h-0 min-w-0 flex-1 flex-col", !sceneTabActive && "hidden")}>
          <Topbar />
          <Layout key={projectPath ?? ""} />
        </div>
        {activeKind === "asset" && <AssetWorkspace asset={activeAsset} />}
        {activeKind === "flamegraph" && <FlameGraphWorkspace />}
        {activeKind === "materialGraph" && (
          <MaterialGraphWorkspace materialId={activeGraphMaterialId} />
        )}
        <ProjectStartupModal open={projectModalOpen} onProjectLoaded={handleProjectLoaded} />
        <SettingsModal />
        <Toaster />
        <StatusFooter />
      </div>
    </TooltipProvider>
  );
}

/// The status strip below the dock. A leaf so the fps meter's twice-a-second store
/// write re-renders this one line, not the entire shell above it.
function StatusFooter() {
  const phase = useEditorStore((s) => s.engineStatus.phase);
  const uiFrameRateHz = useEditorStore((s) => s.uiFrameRateHz);
  return (
    <footer className="flex h-[22px] flex-none items-center justify-end border-t border-border bg-card px-3">
      <span className="font-mono text-[10px] uppercase tracking-wide text-muted-foreground">
        {phase} · UI {uiFrameRateHz > 0 ? uiFrameRateHz.toFixed(0) : "--"} fps
      </span>
    </footer>
  );
}

function AssetWorkspace({ asset }: { asset: AssetEntry | null }) {
  if (!asset) {
    return (
      <main className="flex min-h-0 flex-1 items-center justify-center bg-background text-xs italic text-muted-foreground">
        Asset not found
      </main>
    );
  }
  return (
    <main className="flex min-h-0 flex-1 items-center justify-center overflow-hidden bg-background p-6">
      <AssetPreview entry={asset} className="h-full max-h-full w-auto max-w-full" />
    </main>
  );
}

/// The Flame graph main tab: a large view of the last profiler capture's flame chart.
function FlameGraphWorkspace() {
  const capture = useEditorStore((s) => s.capture);
  if (capture === null) {
    return (
      <main className="flex min-h-0 flex-1 items-center justify-center bg-background text-xs italic text-muted-foreground">
        Capture a frame in the Profiler to populate the flame graph.
      </main>
    );
  }
  return (
    <main className="min-h-0 flex-1 overflow-hidden bg-background p-3">
      <CaptureFlame />
    </main>
  );
}

/// The Material graph main tab: the node-graph editor for one material, filling the work area.
function MaterialGraphWorkspace({ materialId }: { materialId: string | null }) {
  if (materialId === null) {
    return (
      <main className="flex min-h-0 flex-1 items-center justify-center bg-background text-xs italic text-muted-foreground">
        Material not found
      </main>
    );
  }
  return (
    <main className="min-h-0 flex-1 overflow-hidden bg-background">
      <MaterialGraphEditor materialId={materialId} />
    </main>
  );
}
