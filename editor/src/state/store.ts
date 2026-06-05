/// Central editor state (Zustand) plus the focus-gated reconcile poll that keeps
/// the slices in sync with the running engine. Data panels (phases 5-8) read the
/// slices this fills.
import { create } from "zustand";
import { client, type Client } from "../control/client";
import type { ProjectInfo } from "../control/client";
import type {
  AssetEntry,
  EntityListEntry,
  Environment,
  GizmoState,
  InspectResult,
  RenderStats,
} from "../protocol";

export type EnginePhase = "idle" | "starting" | "attaching" | "ready" | "error";
/// The active left-bottom dock tab. Tree rows switch it (the Environment sentinel
/// selects-and-switches); the tab strip itself stays clickable as before.
export type BottomTab = "inspector" | "environment" | "stats";
export type ViewTab =
  | { id: "scene"; kind: "scene"; title: "Scene"; closable: false }
  | {
      id: string;
      kind: "asset";
      assetId: string;
      title: string;
      assetType: AssetEntry["type"];
      closable: true;
    };

export interface EngineStatus {
  running: boolean;
  phase: EnginePhase;
  error?: string;
}

export interface EditorState {
  entities: EntityListEntry[];
  selectedId: string | null;
  /// Hierarchy rows whose children are shown. Plain UI state, deliberately outside
  /// the sceneVersion/selectionVersion keying so a scene mutation never collapses
  /// the tree; setEntities prunes ids that vanished from the scene.
  expandedIds: Set<string>;
  /// The selected non-entity tree row (the pinned Environment node). Never written
  /// to selectedId, so get-selection/inspect are never handed a non-entity id.
  selectedSentinel: "environment" | null;
  bottomTab: BottomTab;
  sceneVersion: number;
  selectionVersion: number;
  componentsBySelected: InspectResult | null;
  assets: AssetEntry[];
  assetFolders: string[];
  viewTabs: ViewTab[];
  activeViewTabId: string;
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
  /// True while a native OS file dialog (Tauri `open`/`save`) is showing. These
  /// dialogs are not window-modal under the reparented-viewport setup, so the
  /// webview stays live; this flag is the app-side lock that stops a second dialog
  /// from being opened and greys the controls that would open one.
  nativeDialogOpen: boolean;
  /// Show the SELECTED entity's components as read-only leaf subrows in the
  /// hierarchy (sourced from componentsBySelected — never an extra inspect).
  /// A persisted view preference, default off so the outliner stays clean.
  showComponentSubrows: boolean;
  /// Hide skeleton joints (rows flagged `bone` by list-entities) in the outliner;
  /// their non-bone descendants re-anchor to the nearest visible ancestor. A
  /// persisted view preference like the subrows toggle.
  hideBones: boolean;
  /// One-shot "jump the Inspector to this component" signal set by a subrow click;
  /// the Inspector consumes and clears it. Never gated on the poll's versions.
  focusComponent: string | null;

  setEntities(entities: EntityListEntry[]): void;
  /// Optimistically rename one entity's hierarchy row between polls (inline rename).
  /// The reconcile poll's sceneVersion bump re-fetches the authoritative list.
  applyOptimisticEntityName(id: string, name: string): void;
  setSelectedId(selectedId: string | null): void;
  selectEntity(id: string): void;
  toggleExpanded(id: string): void;
  setExpanded(id: string, expanded: boolean): void;
  selectSentinel(sentinel: "environment" | null): void;
  setBottomTab(bottomTab: BottomTab): void;
  /// Engine-authoritative reparent (null detaches to root): optimistically relink the
  /// moved entity's parentId in place — selection untouched — and hold dragActive over
  /// the round trip so the poll cannot clobber the relink. A rejection falls back to
  /// the next authoritative list (set-parent bumps sceneVersion on success only).
  setParent(id: string, parentId: string | null): Promise<void>;
  setSceneVersion(sceneVersion: number): void;
  setSelectionVersion(selectionVersion: number): void;
  setComponentsBySelected(components: InspectResult | null): void;
  /// Overlay one component's full DTO onto the live inspect result between polls
  /// (optimistic write). The reconcile poll is gated off mid-drag, so this is the
  /// UI's truth until the next poll; a new selectionVersion drops it (poll re-inspects).
  applyOptimisticComponent(component: string, dto: object): void;
  setAssetList(assets: AssetEntry[], folders: string[]): void;
  /// Re-fetch the catalog from the engine into `assets`. Called by the reconcile
  /// poll on a sceneVersion change and eagerly after an import/rename.
  refreshAssets(): Promise<void>;
  openAssetTab(asset: AssetEntry): void;
  closeViewTab(id: string): void;
  setActiveViewTab(id: string): void;
  moveViewTab(id: string, index: number): void;
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
  setNativeDialogOpen(nativeDialogOpen: boolean): void;
  toggleComponentSubrows(): void;
  toggleHideBones(): void;
  setFocusComponent(focusComponent: string | null): void;
}

export const useEditorStore = create<EditorState>((set) => ({
  entities: [],
  selectedId: null,
  expandedIds: new Set<string>(),
  selectedSentinel: null,
  bottomTab: "inspector",
  sceneVersion: -1,
  selectionVersion: -1,
  componentsBySelected: null,
  assets: [],
  assetFolders: [],
  viewTabs: [{ id: "scene", kind: "scene", title: "Scene", closable: false }],
  activeViewTabId: "scene",
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
  nativeDialogOpen: false,
  showComponentSubrows: loadShowSubrows(),
  hideBones: loadHideBones(),
  focusComponent: null,

  setEntities: (entities) =>
    set((s) => {
      // Prune expand-state for ids that left the scene; survivors keep their state
      // so a poll refresh never collapses the tree.
      const present = new Set(entities.map((e) => e.id));
      let expandedIds = s.expandedIds;
      if ([...expandedIds].some((id) => !present.has(id))) {
        expandedIds = new Set([...expandedIds].filter((id) => present.has(id)));
        persistExpanded(expandedIds);
      }
      return { entities, expandedIds };
    }),
  applyOptimisticEntityName: (id, name) =>
    set((s) => ({
      entities: s.entities.map((e) => (e.id === id ? { ...e, name } : e)),
    })),
  setSelectedId: (selectedId) => set({ selectedId }),
  // Optimistic local selection: set immediately on a hierarchy click so the row
  // highlights without waiting a poll interval; the reconcile poll confirms via
  // selectionVersion (engine is authoritative if a newer version arrives).
  selectEntity: (id) => set({ selectedId: id, selectedSentinel: null }),
  toggleExpanded: (id) =>
    set((s) => {
      const expandedIds = new Set(s.expandedIds);
      if (!expandedIds.delete(id)) {
        expandedIds.add(id);
      }
      persistExpanded(expandedIds);
      return { expandedIds };
    }),
  setExpanded: (id, expanded) =>
    set((s) => {
      if (s.expandedIds.has(id) === expanded) {
        return {};
      }
      const expandedIds = new Set(s.expandedIds);
      if (expanded) {
        expandedIds.add(id);
      } else {
        expandedIds.delete(id);
      }
      persistExpanded(expandedIds);
      return { expandedIds };
    }),
  selectSentinel: (sentinel) =>
    set((s) => ({
      selectedSentinel: sentinel,
      bottomTab: sentinel === "environment" ? "environment" : s.bottomTab,
    })),
  setBottomTab: (bottomTab) =>
    set((s) => ({
      bottomTab,
      // Leaving the Environment tab drops the sentinel highlight so the tree and
      // the tab strip never disagree about what is active.
      selectedSentinel: bottomTab === "environment" ? s.selectedSentinel : null,
    })),
  setParent: async (id, parentId) => {
    const previous = useEditorStore.getState().entities.find((e) => e.id === id)?.parentId;
    useEditorStore.getState().setDragActive(true);
    set((s) => ({
      entities: s.entities.map((e) =>
        e.id === id ? { ...e, parentId: parentId ?? undefined } : e,
      ),
    }));
    try {
      await client.setParent(id, parentId);
    } catch (err) {
      // A rejected reparent never bumps sceneVersion, so the poll will not restore
      // the row — roll the optimistic relink back by hand.
      set((s) => ({
        entities: s.entities.map((e) => (e.id === id ? { ...e, parentId: previous } : e)),
      }));
      throw err;
    } finally {
      useEditorStore.getState().setDragActive(false);
    }
  },
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
  setAssetList: (assets, assetFolders) =>
    set((s) => ({
      assets,
      assetFolders,
      viewTabs: s.viewTabs.map((tab) => {
        if (tab.kind !== "asset") {
          return tab;
        }
        const asset = assets.find((entry) => entry.id === tab.assetId);
        return asset ? { ...tab, title: asset.name, assetType: asset.type } : tab;
      }),
    })),
  refreshAssets: async () => {
    try {
      const list = await client.listAssets();
      useEditorStore.getState().setAssetList(list.assets, list.folders);
    } catch {
      // Engine may be briefly busy; the next reconcile tick recovers.
    }
  },
  openAssetTab: (asset) =>
    set((s) => {
      const id = `asset:${asset.id}`;
      const existing = s.viewTabs.some((tab) => tab.id === id);
      return {
        activeViewTabId: id,
        viewTabs: existing
          ? s.viewTabs
          : [
              ...s.viewTabs,
              {
                id,
                kind: "asset",
                assetId: asset.id,
                title: asset.name,
                assetType: asset.type,
                closable: true,
              },
            ],
      };
    }),
  closeViewTab: (id) =>
    set((s) => {
      if (id === "scene") {
        return {};
      }
      const index = s.viewTabs.findIndex((tab) => tab.id === id);
      const viewTabs = s.viewTabs.filter((tab) => tab.id !== id);
      const activeViewTabId =
        s.activeViewTabId === id
          ? (viewTabs[Math.max(0, index - 1)]?.id ?? "scene")
          : s.activeViewTabId;
      return { viewTabs, activeViewTabId };
    }),
  setActiveViewTab: (id) =>
    set((s) => (s.viewTabs.some((tab) => tab.id === id) ? { activeViewTabId: id } : {})),
  moveViewTab: (id, index) =>
    set((s) => {
      if (id === "scene") {
        return {};
      }
      const moving = s.viewTabs.find((tab) => tab.id === id);
      if (!moving) {
        return {};
      }
      const without = s.viewTabs.filter((tab) => tab.id !== id);
      const nextIndex = Math.min(Math.max(1, index), without.length);
      return {
        viewTabs: [...without.slice(0, nextIndex), moving, ...without.slice(nextIndex)],
      };
    }),
  setEnvironment: (environment) => set({ environment }),
  setRenderStats: (renderStats) => set({ renderStats }),
  // Rehydrate the persisted expand-state when a project (path) becomes current.
  setProject: (project) =>
    set({
      project,
      expandedIds: project?.path ? loadExpanded(project.path) : new Set<string>(),
    }),
  setPollRateHz: (pollRateHz) => set({ pollRateHz }),
  setUiFrameStats: (uiFrameRateHz, uiFrameMs) => set({ uiFrameRateHz, uiFrameMs }),
  resetSceneState: () => {
    // The catalog changed under us, so every cached thumbnail blob URL is stale.
    invalidateThumbnails();
    set({
      entities: [],
      selectedId: null,
      expandedIds: new Set<string>(),
      selectedSentinel: null,
      componentsBySelected: null,
      assets: [],
      assetFolders: [],
      viewTabs: [{ id: "scene", kind: "scene", title: "Scene", closable: false }],
      activeViewTabId: "scene",
      environment: null,
      // Force the reconcile poll's version diff to fire on the next tick so the
      // hierarchy/inspector/assets/env all re-fetch against the loaded scene.
      sceneVersion: -1,
      selectionVersion: -1,
      // One-shot UI intent; never meaningful across a scene swap. The subrows
      // toggle survives — it is a view preference, not scene state.
      focusComponent: null,
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
  setNativeDialogOpen: (nativeDialogOpen) => set({ nativeDialogOpen }),
  toggleComponentSubrows: () =>
    set((s) => {
      const showComponentSubrows = !s.showComponentSubrows;
      try {
        localStorage.setItem(SUBROWS_STORAGE_KEY, showComponentSubrows ? "1" : "0");
      } catch {
        // Storage unavailable; the preference is then session-only.
      }
      return { showComponentSubrows };
    }),
  toggleHideBones: () =>
    set((s) => {
      const hideBones = !s.hideBones;
      try {
        localStorage.setItem(HIDE_BONES_STORAGE_KEY, hideBones ? "1" : "0");
      } catch {
        // Storage unavailable; the preference is then session-only.
      }
      return { hideBones };
    }),
  setFocusComponent: (focusComponent) => set({ focusComponent }),
}));

/// Run a native file-dialog thunk (`open`/`save`) under the app-side dialog lock:
/// no-op if one is already open (the re-entry guard that stops stacked pickers),
/// otherwise hold `nativeDialogOpen` for the dialog's lifetime so the controls that
/// spawn dialogs grey out. The lock covers only the dialog, not the engine work that
/// follows it. Returns the thunk's result, or null when skipped by the guard.
export async function withNativeDialog<T>(fn: () => Promise<T>): Promise<T | null> {
  const store = useEditorStore.getState();
  if (store.nativeDialogOpen) {
    return null;
  }
  store.setNativeDialogOpen(true);
  try {
    return await fn();
  } finally {
    useEditorStore.getState().setNativeDialogOpen(false);
  }
}

/// One outliner node: an entity plus its resolved children. The tree is built
/// client-side from the flat `entities` slice; the engine ships only `parentId`.
export interface TreeNode {
  entity: EntityListEntry;
  children: TreeNode[];
}

/// Group the flat entity list into a forest by parentId. Absent/`"0"` parentId,
/// an unknown parent id, or a self-reference all land the entry at the root
/// (defensive: the engine roots dangling parents on load, and the client must
/// never loop). Sibling order preserves the engine's array order (unordered v1).
export function buildTree(entities: EntityListEntry[]): TreeNode[] {
  const nodes = new Map<string, TreeNode>();
  for (const entity of entities) {
    nodes.set(entity.id, { entity, children: [] });
  }
  const roots: TreeNode[] = [];
  for (const entity of entities) {
    const node = nodes.get(entity.id)!;
    const parentId = entity.parentId;
    const parent =
      parentId && parentId !== "0" && parentId !== entity.id ? nodes.get(parentId) : undefined;
    if (parent) {
      parent.children.push(node);
    } else {
      roots.push(node);
    }
  }
  return roots;
}

/// Expand-state persistence, keyed by project path (plain UI state, like the
/// thumbnail cache kept out of Zustand): survives reloads, never the poll's concern.
const EXPANDED_STORAGE_PREFIX = "saffron.expandedIds:";

function persistExpanded(ids: Set<string>): void {
  const path = useEditorStore.getState().project?.path;
  if (!path) {
    return;
  }
  try {
    localStorage.setItem(EXPANDED_STORAGE_PREFIX + path, JSON.stringify([...ids]));
  } catch {
    // Storage may be unavailable (private mode); expansion is then session-only.
  }
}

function loadExpanded(path: string): Set<string> {
  try {
    const raw = localStorage.getItem(EXPANDED_STORAGE_PREFIX + path);
    if (raw) {
      return new Set(JSON.parse(raw) as string[]);
    }
  } catch {
    // Fall through to empty.
  }
  return new Set<string>();
}

/// The component-subrows toggle is a view preference (one key, unlike the
/// per-project expand-state); default off keeps the outliner clean.
const SUBROWS_STORAGE_KEY = "saffron.showComponentSubrows";

function loadShowSubrows(): boolean {
  try {
    return localStorage.getItem(SUBROWS_STORAGE_KEY) === "1";
  } catch {
    return false;
  }
}

const HIDE_BONES_STORAGE_KEY = "saffron.hideBones";

function loadHideBones(): boolean {
  try {
    return localStorage.getItem(HIDE_BONES_STORAGE_KEY) === "1";
  } catch {
    return false;
  }
}

/// The outliner's bone filter: drop rows flagged `bone` and re-anchor every surviving
/// entity whose ancestry passes through bones to its nearest visible ancestor (walks
/// are bounded so corrupt data cannot loop). Pure — used before buildTree.
export function reanchorPastBones(entities: EntityListEntry[]): EntityListEntry[] {
  const byId = new Map(entities.map((e) => [e.id, e]));
  return entities
    .filter((e) => !e.bone)
    .map((e) => {
      let parent = e.parentId ? byId.get(e.parentId) : undefined;
      for (let steps = 0; parent?.bone && steps <= entities.length; steps++) {
        parent = parent.parentId ? byId.get(parent.parentId) : undefined;
      }
      const parentId = parent?.id;
      return parentId === e.parentId ? e : { ...e, parentId };
    });
}

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

export function getCachedThumbnailUrl(assetId: string, size: number): string | null {
  const cached = thumbnailCache.get(assetId);
  return cached && cached.size >= size ? cached.url : null;
}

/// Resolve a blob URL for an asset's thumbnail at (at least) `size` px. Cache hit
/// (same or larger cached size) returns immediately; a miss fetches once over the
/// socket, decodes the base64 PNG, and stores the object URL. Rejects to let the
/// caller fall back to a type icon.
export async function getThumbnailUrl(assetId: string, size: number): Promise<string> {
  const cached = getCachedThumbnailUrl(assetId, size);
  if (cached) {
    return cached;
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
