/// Central editor state (Zustand) plus the focus-gated reconcile poll that keeps
/// the slices in sync with the running engine. Data panels (phases 5-8) read the
/// slices this fills.
import { create } from "zustand";
import { client, type Client } from "../control/client";
import type { ProjectInfo } from "../control/client";
import type {
  AssetEntry,
  EntityRef,
  Environment,
  GizmoState,
  InspectResult,
  RenderStats,
} from "../protocol";

export type EnginePhase = "idle" | "starting" | "attaching" | "ready" | "error";

export interface EngineStatus {
  running: boolean;
  phase: EnginePhase;
  error?: string;
}

export interface EditorState {
  entities: EntityRef[];
  selectedId: string | null;
  sceneVersion: number;
  selectionVersion: number;
  componentsBySelected: InspectResult | null;
  assets: AssetEntry[];
  environment: Environment | null;
  renderStats: RenderStats | null;
  project: ProjectInfo | null;
  /// Client-side reconcile-poll rate (Hz), an EMA over the actual tick interval.
  /// This is the WEBVIEW poll cadence, NOT the engine frame rate (the engine's own
  /// fps/frameMs/gpuMs ride on `render-stats`; the native viewport paints
  /// independently of this poll).
  pollRateHz: number;
  /// Webview repaint cadence measured with requestAnimationFrame.
  uiFrameRateHz: number;
  uiFrameMs: number;
  engineStatus: EngineStatus;
  dragActive: boolean;
  gizmo: GizmoState;
  /// When true, the native viewport window is parked off-screen so an overlay
  /// (a modal dialog) can paint over the viewport rect — the reparented X11
  /// child always paints on top otherwise. The ViewportPanel reads this and skips
  /// gluing the native window to its div until it clears.
  viewportHidden: boolean;

  setEntities(entities: EntityRef[]): void;
  /// Optimistically rename one entity's hierarchy row between polls (inline rename).
  /// The reconcile poll's sceneVersion bump re-fetches the authoritative list.
  applyOptimisticEntityName(id: string, name: string): void;
  setSelectedId(selectedId: string | null): void;
  selectEntity(id: string): void;
  setSceneVersion(sceneVersion: number): void;
  setSelectionVersion(selectionVersion: number): void;
  setComponentsBySelected(components: InspectResult | null): void;
  /// Overlay one component's full DTO onto the live inspect result between polls
  /// (optimistic write). The reconcile poll is gated off mid-drag, so this is the
  /// UI's truth until the next poll; a new selectionVersion drops it (poll re-inspects).
  applyOptimisticComponent(component: string, dto: object): void;
  setAssets(assets: AssetEntry[]): void;
  /// Re-fetch the catalog from the engine into `assets`. Called by the reconcile
  /// poll on a sceneVersion change and eagerly after an import/rename.
  refreshAssets(): Promise<void>;
  setEnvironment(environment: Environment | null): void;
  setRenderStats(renderStats: RenderStats | null): void;
  setProject(project: ProjectInfo | null): void;
  setPollRateHz(pollRateHz: number): void;
  setUiFrameStats(frameRateHz: number, frameMs: number): void;
  /// Hard scene reset after a project/scene load: clear entities + selection +
  /// the live inspect result + assets + environment, invalidate cached thumbnails,
  /// and let the next reconcile tick re-fetch everything against the new scene
  /// (the engine cleared its own selection on load, so we mirror that). Idempotent.
  resetSceneState(): void;
  setEngineStatus(patch: Partial<EngineStatus>): void;
  setPhase(phase: EnginePhase, error?: string): void;
  setDragActive(dragActive: boolean): void;
  setGizmo(patch: Partial<GizmoState>): void;
  setViewportHidden(viewportHidden: boolean): void;
}

export const useEditorStore = create<EditorState>((set) => ({
  entities: [],
  selectedId: null,
  sceneVersion: -1,
  selectionVersion: -1,
  componentsBySelected: null,
  assets: [],
  environment: null,
  renderStats: null,
  project: null,
  pollRateHz: 0,
  uiFrameRateHz: 0,
  uiFrameMs: 0,
  engineStatus: { running: false, phase: "idle" },
  dragActive: false,
  gizmo: { op: "translate", space: "world" },
  viewportHidden: false,

  setEntities: (entities) => set({ entities }),
  applyOptimisticEntityName: (id, name) =>
    set((s) => ({
      entities: s.entities.map((e) => (e.id === id ? { ...e, name } : e)),
    })),
  setSelectedId: (selectedId) => set({ selectedId }),
  // Optimistic local selection: set immediately on a hierarchy click so the row
  // highlights without waiting a poll interval; the reconcile poll confirms via
  // selectionVersion (engine is authoritative if a newer version arrives).
  selectEntity: (id) => set({ selectedId: id }),
  setSceneVersion: (sceneVersion) => set({ sceneVersion }),
  setSelectionVersion: (selectionVersion) => set({ selectionVersion }),
  setComponentsBySelected: (componentsBySelected) => set({ componentsBySelected }),
  applyOptimisticComponent: (component, dto) =>
    set((s) => {
      if (!s.componentsBySelected) {
        return {};
      }
      return {
        componentsBySelected: {
          ...s.componentsBySelected,
          components: {
            ...s.componentsBySelected.components,
            [component]: dto,
          },
        },
      };
    }),
  setAssets: (assets) => set({ assets }),
  refreshAssets: async () => {
    try {
      const list = await client.listAssets();
      set({ assets: list.assets });
    } catch {
      // Engine may be briefly busy; the next reconcile tick recovers.
    }
  },
  setEnvironment: (environment) => set({ environment }),
  setRenderStats: (renderStats) => set({ renderStats }),
  setProject: (project) => set({ project }),
  setPollRateHz: (pollRateHz) => set({ pollRateHz }),
  setUiFrameStats: (uiFrameRateHz, uiFrameMs) => set({ uiFrameRateHz, uiFrameMs }),
  resetSceneState: () => {
    // The catalog changed under us, so every cached thumbnail blob URL is stale.
    invalidateThumbnails();
    set({
      entities: [],
      selectedId: null,
      componentsBySelected: null,
      assets: [],
      environment: null,
      // Force the reconcile poll's version diff to fire on the next tick so the
      // hierarchy/inspector/assets/env all re-fetch against the loaded scene.
      sceneVersion: -1,
      selectionVersion: -1,
    });
  },
  setEngineStatus: (patch) => set((s) => ({ engineStatus: { ...s.engineStatus, ...patch } })),
  setPhase: (phase, error) =>
    set((s) => ({
      engineStatus: {
        ...s.engineStatus,
        phase,
        error: phase === "error" ? error : undefined,
        running: phase === "ready" || phase === "error" ? s.engineStatus.running : phase !== "idle",
      },
    })),
  setDragActive: (dragActive) => set({ dragActive }),
  setGizmo: (patch) => set((s) => ({ gizmo: { ...s.gizmo, ...patch } })),
  setViewportHidden: (viewportHidden) => set({ viewportHidden }),
}));

const FAST_RECONCILE_INTERVAL_MS = 50; // cheap state lane, target ~20 Hz
const WATCHDOG_INTERVAL_MS = 1000;

/// Start the focus-gated reconcile loops. Cheap interactive state runs frequently;
/// heavier scene/entity/inspect refreshes only run when versions change and never
/// block the next cheap tick.
export function startReconcile(client: Client): () => void {
  let stopped = false;
  let fastTimer: ReturnType<typeof setTimeout> | null = null;
  let watchdogTimer: ReturnType<typeof setInterval> | null = null;
  let fastInFlight = false;
  let refreshInFlight = false;
  let watchdogInFlight = false;
  let pendingRefresh: {
    selectedId: string | null;
    sceneChanged: boolean;
    selectionChanged: boolean;
    sceneVersion: number;
    previousSceneVersion: number;
  } | null = null;

  let knownSceneVersion = -1;
  let knownSelectionVersion = -1;
  let knownSelectedId: string | null = null;

  let lastTickAt = 0;
  let emaIntervalMs = 0;

  const schedule = (): void => {
    if (stopped) {
      return;
    }
    fastTimer = setTimeout(tick, FAST_RECONCILE_INTERVAL_MS);
  };

  const readyForSync = (): boolean => {
    const store = useEditorStore.getState();
    return store.engineStatus.phase === "ready" && document.hasFocus();
  };

  /// The crash watchdog runs once the engine is live (socket bound) and stops once
  /// it has died or never started. This includes the starting/attaching window —
  /// the gap between socket-up and the viewport attach — so a child that binds its
  /// socket and then exits before attaching is still caught (the ViewportPanel's
  /// phase flip to ready never fires in that case).
  const engineMayBeAlive = (): boolean => {
    const phase = useEditorStore.getState().engineStatus.phase;
    return phase !== "idle" && phase !== "error" && document.hasFocus();
  };

  const refreshHeavyState = (request: {
    selectedId: string | null;
    sceneChanged: boolean;
    selectionChanged: boolean;
    sceneVersion: number;
    previousSceneVersion: number;
  }): void => {
    const { selectedId, sceneChanged, selectionChanged, sceneVersion, previousSceneVersion } =
      request;
    if (refreshInFlight || (!sceneChanged && !selectionChanged)) {
      if (refreshInFlight) {
        pendingRefresh = request;
      }
      return;
    }
    refreshInFlight = true;

    void (async () => {
      try {
        if (sceneChanged) {
          if (sceneVersion < previousSceneVersion) {
            invalidateThumbnails();
          }
          void useEditorStore.getState().refreshAssets();
          client
            .getEnvironment()
            .then((env) => {
              if (!stopped && !useEditorStore.getState().dragActive) {
                useEditorStore.getState().setEnvironment(env);
              }
            })
            .catch(() => {});

          const list = await client.listEntities();
          if (stopped) {
            return;
          }
          if (!useEditorStore.getState().dragActive) {
            useEditorStore.getState().setEntities(list.entities);
          }
        }

        if (selectedId === null) {
          if (!useEditorStore.getState().dragActive) {
            useEditorStore.getState().setComponentsBySelected(null);
          }
          return;
        }

        const inspected = await client.inspect(selectedId);
        if (stopped) {
          return;
        }
        if (!useEditorStore.getState().dragActive) {
          useEditorStore.getState().setComponentsBySelected(inspected);
        }
      } catch {
      } finally {
        refreshInFlight = false;
        if (pendingRefresh !== null && !stopped) {
          const next = pendingRefresh;
          pendingRefresh = null;
          refreshHeavyState(next);
        }
      }
    })();
  };

  const watchdog = (): void => {
    if (stopped || watchdogInFlight || !engineMayBeAlive()) {
      return;
    }
    watchdogInFlight = true;
    void client
      .engineAlive()
      .then((alive) => {
        if (!alive && !stopped) {
          useEditorStore.getState().setPhase("error", "Engine process exited.");
        }
      })
      .catch(() => {
        if (!stopped) {
          useEditorStore.getState().setPhase("error", "Engine process exited.");
        }
      })
      .finally(() => {
        watchdogInFlight = false;
      });
  };

  async function tick(): Promise<void> {
    if (stopped || fastInFlight) {
      if (!stopped) {
        schedule();
      }
      return;
    }
    fastInFlight = true;
    try {
      if (!readyForSync()) {
        lastTickAt = 0;
        return;
      }

      const now = performance.now();
      if (lastTickAt !== 0) {
        const interval = now - lastTickAt;
        emaIntervalMs = emaIntervalMs === 0 ? interval : emaIntervalMs * 0.8 + interval * 0.2;
        if (emaIntervalMs > 0) {
          useEditorStore.getState().setPollRateHz(1000 / emaIntervalMs);
        }
      }
      lastTickAt = now;

      const [selection, stats, gizmo] = await Promise.all([
        client.getSelection(),
        client.renderStats(),
        client.getGizmo(),
      ]);
      if (stopped) {
        return;
      }

      const live = useEditorStore.getState();
      if (live.dragActive || live.engineStatus.phase !== "ready") {
        return;
      }

      live.setRenderStats(stats);
      // Reflect the engine's gizmo state (so an external `se set-gizmo` shows up
      // in the Topbar); Topbar clicks set this optimistically, the poll confirms.
      live.setGizmo(gizmo);

      const nextSelectedId = selection.entity ? selection.entity.id : null;
      const previousSceneVersion = knownSceneVersion;
      const sceneChanged = selection.sceneVersion !== knownSceneVersion;
      const selectionChanged =
        selection.selectionVersion !== knownSelectionVersion || nextSelectedId !== knownSelectedId;

      live.setSelectionVersion(selection.selectionVersion);
      live.setSceneVersion(selection.sceneVersion);
      if (selectionChanged) {
        live.setSelectedId(nextSelectedId);
      }

      refreshHeavyState({
        selectedId: nextSelectedId,
        sceneChanged,
        selectionChanged,
        sceneVersion: selection.sceneVersion,
        previousSceneVersion,
      });

      knownSceneVersion = selection.sceneVersion;
      knownSelectionVersion = selection.selectionVersion;
      knownSelectedId = nextSelectedId;
    } catch {
    } finally {
      fastInFlight = false;
      schedule();
    }
  }

  schedule();
  watchdogTimer = setInterval(watchdog, WATCHDOG_INTERVAL_MS);

  return () => {
    stopped = true;
    if (fastTimer !== null) {
      clearTimeout(fastTimer);
      fastTimer = null;
    }
    if (watchdogTimer !== null) {
      clearInterval(watchdogTimer);
      watchdogTimer = null;
    }
  };
}

/// Client-side thumbnail cache: assetId → { blob URL, the px size it was fetched
/// at }. Kept at module scope (NOT in Zustand) because it holds blob URLs that
/// must survive re-renders without provoking store churn, and a single fetch must
/// be shared by all tiles/pickers/viewers asking for the same asset.
///
/// A get-thumbnail call is a GPU→CPU readback + PNG encode (off the present path),
/// so we fetch lazily on first view, dedupe concurrent requests, and re-use a
/// cached URL whenever the cached image is at least as large as the requested
/// size (a 128 grid tile and a 16-from-64 combo swatch both read the 64/128 blob).
interface ThumbnailCacheEntry {
  url: string;
  size: number;
}

const thumbnailCache = new Map<string, ThumbnailCacheEntry>();
/// In-flight fetches keyed by assetId, so N tiles mounting at once issue ONE
/// get-thumbnail per asset (the rest await the same promise).
const thumbnailInflight = new Map<string, Promise<string>>();

function base64ToBlob(b64: string, mime = "image/png"): Blob {
  const bin = atob(b64);
  const bytes = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) {
    bytes[i] = bin.charCodeAt(i);
  }
  return new Blob([bytes], { type: mime });
}

/// Resolve a blob URL for an asset's thumbnail at (at least) `size` px. Cache hit
/// (same or larger cached size) returns immediately; a miss fetches once over the
/// socket, decodes the base64 PNG, and stores the object URL. Rejects to let the
/// caller fall back to a type icon.
export async function getThumbnailUrl(assetId: string, size: number): Promise<string> {
  const cached = thumbnailCache.get(assetId);
  if (cached && cached.size >= size) {
    return cached.url;
  }
  const inflight = thumbnailInflight.get(assetId);
  if (inflight) {
    return inflight;
  }
  const promise = (async (): Promise<string> => {
    const thumb = await client.getThumbnail(assetId, size);
    const url = URL.createObjectURL(base64ToBlob(thumb.base64));
    const prev = thumbnailCache.get(assetId);
    if (prev) {
      URL.revokeObjectURL(prev.url);
    }
    thumbnailCache.set(assetId, { url, size });
    return url;
  })();
  thumbnailInflight.set(assetId, promise);
  try {
    return await promise;
  } finally {
    thumbnailInflight.delete(assetId);
  }
}

/// Revoke every cached blob URL and clear the cache. Called on a scene/project
/// load (the catalog changed, so cached images are stale) — see the reconcile
/// poll's scene-reset branch.
export function invalidateThumbnails(): void {
  for (const entry of thumbnailCache.values()) {
    URL.revokeObjectURL(entry.url);
  }
  thumbnailCache.clear();
}
