/// Central editor state (Zustand) plus the focus-gated reconcile poll that keeps
/// the slices in sync with the running engine. Data panels (phases 5-8) read the
/// slices this fills.
import { create } from "zustand";
import { client, type Client } from "../control/client";
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
  attached: boolean;
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
  /// Client-side reconcile-poll rate (Hz), an EMA over the actual tick interval.
  /// This is the WEBVIEW poll cadence, NOT the engine frame rate — there is no
  /// frame timing on the control wire (the native viewport paints independently).
  pollRateHz: number;
  engineStatus: EngineStatus;
  dragActive: boolean;
  gizmo: GizmoState;
  /// When true, the native viewport window is parked off-screen so an overlay
  /// (the asset View modal) can paint over the viewport rect — the reparented X11
  /// child always paints on top otherwise. The ViewportPanel reads this and skips
  /// gluing the native window to its div until it clears.
  viewportHidden: boolean;

  setEntities(entities: EntityRef[]): void;
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
  setPollRateHz(pollRateHz: number): void;
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
  pollRateHz: 0,
  engineStatus: { running: false, attached: false, phase: "idle" },
  dragActive: false,
  gizmo: { op: "translate", space: "world" },
  viewportHidden: false,

  setEntities: (entities) => set({ entities }),
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
  setPollRateHz: (pollRateHz) => set({ pollRateHz }),
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
  setEngineStatus: (patch) =>
    set((s) => ({ engineStatus: { ...s.engineStatus, ...patch } })),
  setPhase: (phase, error) =>
    set((s) => ({
      engineStatus: {
        ...s.engineStatus,
        phase,
        error: phase === "error" ? error : undefined,
        running: phase === "ready" || phase === "error" ? s.engineStatus.running : phase !== "idle",
        attached: phase === "ready" ? true : phase === "idle" ? false : s.engineStatus.attached,
      },
    })),
  setDragActive: (dragActive) => set({ dragActive }),
  setGizmo: (patch) => set((s) => ({ gizmo: { ...s.gizmo, ...patch } })),
  setViewportHidden: (viewportHidden) => set({ viewportHidden }),
}));

const RECONCILE_INTERVAL_MS = 160; // ~6 Hz

/// Start the focus-gated reconcile poll. Each tick, only while phase === 'ready'
/// and the document has focus:
///   - getSelection() + renderStats() (cheap, every tick),
///   - listEntities() only when sceneVersion changed,
///   - inspect(selectedId) only when selection/sceneVersion changed,
///   - engineAlive() watchdog → setPhase('error') on a mid-session crash.
/// Writes to the store are gated off while dragActive (avoid clobbering optimistic
/// local state during a drag). Returns a stop fn (idempotent).
export function startReconcile(client: Client): () => void {
  let stopped = false;
  let timer: ReturnType<typeof setTimeout> | null = null;
  let inTick = false;

  // Versions last applied to the store, used to decide what to re-fetch.
  let knownSceneVersion = -1;
  let knownSelectionVersion = -1;
  let knownSelectedId: string | null = null;

  // Client-side poll-rate EMA: the wall-clock interval between completed ticks,
  // smoothed and surfaced as Hz. This is the WEBVIEW reconcile cadence, never the
  // engine frame rate (the control wire carries no frame timing).
  let lastTickAt = 0;
  let emaIntervalMs = 0;

  const schedule = (): void => {
    if (stopped) {
      return;
    }
    timer = setTimeout(tick, RECONCILE_INTERVAL_MS);
  };

  async function tick(): Promise<void> {
    if (stopped) {
      return;
    }
    // Avoid overlapping ticks if a round of calls outruns the interval.
    if (inTick) {
      schedule();
      return;
    }
    inTick = true;
    try {
      const store = useEditorStore.getState();
      if (store.engineStatus.phase !== "ready" || !document.hasFocus()) {
        // Reset the EMA baseline so a paused poll (blurred/not-ready) does not
        // bake a huge interval into the rate when it resumes.
        lastTickAt = 0;
        return;
      }

      // Update the client-side poll-rate EMA from the wall-clock tick interval.
      const now = performance.now();
      if (lastTickAt !== 0) {
        const interval = now - lastTickAt;
        emaIntervalMs = emaIntervalMs === 0 ? interval : emaIntervalMs * 0.8 + interval * 0.2;
        if (emaIntervalMs > 0) {
          store.setPollRateHz(1000 / emaIntervalMs);
        }
      }
      lastTickAt = now;

      // Crash watchdog: a dead child flips us to the error overlay.
      let alive = true;
      try {
        alive = await client.engineAlive();
      } catch {
        alive = false;
      }
      if (stopped) {
        return;
      }
      if (!alive) {
        useEditorStore.getState().setPhase("error", "Engine process exited.");
        return;
      }

      const [selection, stats, gizmo] = await Promise.all([
        client.getSelection(),
        client.renderStats(),
        client.getGizmo(),
      ]);
      if (stopped) {
        return;
      }

      // Writes gated off while dragging (optimistic local state owns the truth).
      const live = useEditorStore.getState();
      if (live.dragActive || live.engineStatus.phase !== "ready") {
        return;
      }

      live.setRenderStats(stats);
      // Reflect the engine's gizmo state (so an external `se set-gizmo` shows up
      // in the Topbar); Topbar clicks set this optimistically, the poll confirms.
      live.setGizmo(gizmo);

      const nextSelectedId = selection.entity ? selection.entity.id : null;
      const sceneChanged = selection.sceneVersion !== knownSceneVersion;
      const selectionChanged =
        selection.selectionVersion !== knownSelectionVersion ||
        nextSelectedId !== knownSelectedId;

      live.setSelectionVersion(selection.selectionVersion);
      live.setSceneVersion(selection.sceneVersion);
      if (selectionChanged) {
        live.setSelectedId(nextSelectedId);
      }

      // Re-fetch the entity list only when the scene structure changed.
      if (sceneChanged) {
        // A scene/project load resets sceneVersion (it can drop below the last
        // seen value); when that happens the catalog changed under us, so revoke
        // every cached blob URL — stale thumbnails would otherwise point at gone
        // assets (parity with editor_app.cppm:235-240).
        if (selection.sceneVersion < knownSceneVersion) {
          invalidateThumbnails();
        }
        // Imports/loads change the catalog; refresh it (and let the lazy cache
        // re-fetch thumbnails on demand).
        void useEditorStore.getState().refreshAssets();
        // A scene/project load also swaps the environment; refresh it so the
        // Environment panel reflects the loaded sky/ambient (the panel gates its
        // own writes off mid-drag, so this poll-driven refresh is safe).
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

      // Re-inspect the selected entity only when selection or scene changed.
      if (selectionChanged || sceneChanged) {
        if (nextSelectedId === null) {
          if (!useEditorStore.getState().dragActive) {
            useEditorStore.getState().setComponentsBySelected(null);
          }
        } else {
          try {
            const inspected = await client.inspect(nextSelectedId);
            if (stopped) {
              return;
            }
            if (!useEditorStore.getState().dragActive) {
              useEditorStore.getState().setComponentsBySelected(inspected);
            }
          } catch {
            // Entity may have vanished between selection and inspect; ignore.
          }
        }
      }

      knownSceneVersion = selection.sceneVersion;
      knownSelectionVersion = selection.selectionVersion;
      knownSelectedId = nextSelectedId;
    } catch {
      // Transient errors (engine briefly busy) are swallowed; the watchdog and
      // next tick recover. A hard crash is caught by engineAlive() above.
    } finally {
      inTick = false;
      schedule();
    }
  }

  schedule();

  return () => {
    stopped = true;
    if (timer !== null) {
      clearTimeout(timer);
      timer = null;
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
