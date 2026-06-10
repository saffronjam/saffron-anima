/// The resizable dock layout: a full-height Hierarchy + tabbed
/// Inspector/Environment/Stats column on the LEFT; the RIGHT region stacks the
/// Viewport on top of the Assets browser (so Assets sits beside the sidebar, not
/// under it). The split ratios are right-region Assets 0.28 and leftBottom 0.55
/// (Hierarchy is the top 0.45 of the sidebar). The left sidebar uses a pixel width
/// so it cannot collapse while the native viewport is attaching.
///
/// ResizablePanelGroups (react-resizable-panels via the shadcn wrapper):
///   right vertical : Viewport (~72) + Assets bottom (~28)
///   left vertical  : Hierarchy (~45) + tabbed panel (~55)
///
/// Every Radix popover/menu and every resize handle lives in a non-viewport region,
/// so none of them are occluded by the reparented native X11 window. The Viewport
/// panel owns the only host div the engine paints over; the LoadingOverlay is a
/// sibling inside ViewportPanel (NOT a panel the native window paints over).
///
/// `onLayoutChanged` (the stable, internally-debounced callback) pings the layout
/// bus so the ViewportPanel commits an exact resize-end bounds for the native window
/// once a split-drag settles, and persists the split ratios (useDefaultLayout) +
/// sidebar width to localStorage keyed by project path. App remounts this component
/// per project (key), so the persisted layout loads on mount.
import { useEffect, useRef, useState } from "react";
import { useDefaultLayout, type Layout as PanelLayout } from "react-resizable-panels";
import { ResizableHandle, ResizablePanel, ResizablePanelGroup } from "@/components/ui/resizable";
import { Tabs, TabsContent, TabsList, TabsTrigger } from "@/components/ui/tabs";
import { HierarchyPanel } from "../panels/HierarchyPanel";
import { InspectorPanel } from "../panels/InspectorPanel";
import { EnvironmentPanel } from "../panels/EnvironmentPanel";
import { RenderPanel } from "../panels/RenderPanel";
import { AssetsPanel } from "../panels/AssetsPanel";
import { ViewportPanel } from "../panels/ViewportPanel";
import { RightSidebar } from "../panels/RightSidebar";
import { BottomDock } from "../panels/BottomDock";
import { emitLayoutSettled } from "./layoutBus";
import { logRender } from "../lib/renderLog";
import {
  loadRightSidebarWidth,
  loadSidebarWidth,
  persistRightSidebarWidth,
  persistSidebarWidth,
  useEditorStore,
  type BottomTab,
} from "../state/store";

const SIDEBAR_DEFAULT_WIDTH = 280;
const SIDEBAR_MIN_WIDTH = 240;
const SIDEBAR_MAX_WIDTH = 520;
const VIEWPORT_MIN_WIDTH = 520;
const RIGHT_SIDEBAR_DEFAULT_WIDTH = 320;

export function Layout() {
  logRender("Layout");
  const projectPath = useEditorStore((s) => s.project?.path);
  const playState = useEditorStore((s) => s.playState);
  const rightToolsOpen = useEditorStore((s) => s.rightTools.length > 0);
  const bottomToolsOpen = useEditorStore((s) => s.bottomTools.length > 0);
  const [sidebarWidth, setSidebarWidth] = useState(() =>
    clampSidebarWidth(loadSidebarWidth(projectPath) ?? SIDEBAR_DEFAULT_WIDTH),
  );
  const [rightWidth, setRightWidth] = useState(() =>
    clampSidebarWidth(loadRightSidebarWidth(projectPath) ?? RIGHT_SIDEBAR_DEFAULT_WIDTH),
  );
  const dragRef = useRef<{ startX: number; startWidth: number } | null>(null);
  const rightDragRef = useRef<{ startX: number; startWidth: number } | null>(null);
  const sidebarWidthRef = useRef(sidebarWidth);
  sidebarWidthRef.current = sidebarWidth;
  const rightWidthRef = useRef(rightWidth);
  rightWidthRef.current = rightWidth;
  const projectPathRef = useRef(projectPath);
  projectPathRef.current = projectPath;

  const leftLayout = useDefaultLayout({ id: `saffron.layout.left:${projectPath ?? ""}` });
  const rightLayout = useDefaultLayout({ id: `saffron.layout.right:${projectPath ?? ""}` });
  const onLeftLayoutChanged = (layout: PanelLayout): void => {
    leftLayout.onLayoutChanged(layout);
    emitLayoutSettled();
  };
  const onRightLayoutChanged = (layout: PanelLayout): void => {
    rightLayout.onLayoutChanged(layout);
    emitLayoutSettled();
  };

  // Opening or closing the bottom dock resizes the viewport rect (a panel is
  // added/removed). The viewport's ResizeObserver sees the host div change, but a
  // forced resize-end commit makes the Wayland subsurface re-glue exactly to the
  // new bounds (the LeftBottomTabs precedent — emit on the next frame so the panel
  // has laid out). Mounting fires it too, which is harmless (no dock, no move).
  useEffect(() => {
    const raf = requestAnimationFrame(() => emitLayoutSettled({ force: true }));
    return () => cancelAnimationFrame(raf);
  }, [bottomToolsOpen]);

  useEffect(() => {
    const onPointerMove = (event: PointerEvent): void => {
      const drag = dragRef.current;
      if (drag) {
        setSidebarWidth(clampSidebarWidth(drag.startWidth + event.clientX - drag.startX));
        return;
      }
      // The right resizer sits on the sidebar's left edge: dragging left widens it.
      const rightDrag = rightDragRef.current;
      if (rightDrag) {
        setRightWidth(clampSidebarWidth(rightDrag.startWidth + rightDrag.startX - event.clientX));
      }
    };
    const onPointerUp = (): void => {
      const wasLeft = dragRef.current !== null;
      const wasRight = rightDragRef.current !== null;
      if (!wasLeft && !wasRight) {
        return;
      }
      dragRef.current = null;
      rightDragRef.current = null;
      document.body.style.cursor = "";
      document.body.style.userSelect = "";
      if (wasLeft) {
        persistSidebarWidth(projectPathRef.current, sidebarWidthRef.current);
      }
      if (wasRight) {
        persistRightSidebarWidth(projectPathRef.current, rightWidthRef.current);
      }
      emitLayoutSettled();
    };

    window.addEventListener("pointermove", onPointerMove);
    window.addEventListener("pointerup", onPointerUp);
    return () => {
      window.removeEventListener("pointermove", onPointerMove);
      window.removeEventListener("pointerup", onPointerUp);
      document.body.style.cursor = "";
      document.body.style.userSelect = "";
    };
  }, []);

  const beginSidebarResize = (event: React.PointerEvent<HTMLDivElement>): void => {
    event.preventDefault();
    dragRef.current = { startX: event.clientX, startWidth: sidebarWidth };
    document.body.style.cursor = "col-resize";
    document.body.style.userSelect = "none";
  };

  const beginRightResize = (event: React.PointerEvent<HTMLDivElement>): void => {
    event.preventDefault();
    rightDragRef.current = { startX: event.clientX, startWidth: rightWidth };
    document.body.style.cursor = "col-resize";
    document.body.style.userSelect = "none";
  };

  // Play-mode tint: an amber inset ring around the whole dock marks the editor as live
  // (Unity's playmode-tint lesson — the single defense against "edited in play, lost it
  // on stop"). The viewport interior stays untinted; it is the game view.
  const playRing = playState === "edit" ? "" : "ring-2 ring-inset ring-amber-500/60 rounded-sm";

  return (
    <div className={`flex min-h-0 min-w-0 flex-1 ${playRing}`}>
      <aside className="min-h-0 flex-none bg-background" style={{ width: `${sidebarWidth}px` }}>
        <ResizablePanelGroup
          orientation="vertical"
          defaultLayout={leftLayout.defaultLayout}
          onLayoutChanged={onLeftLayoutChanged}
        >
          <ResizablePanel
            id="hierarchy"
            defaultSize={45}
            minSize={15}
            className="min-h-0 bg-background"
          >
            <HierarchyPanel />
          </ResizablePanel>
          <ResizableHandle />
          <ResizablePanel
            id="left-tabs"
            defaultSize={55}
            minSize={15}
            className="min-h-0 bg-background"
          >
            <LeftBottomTabs />
          </ResizablePanel>
        </ResizablePanelGroup>
      </aside>
      <div
        className="relative flex w-px flex-none cursor-col-resize items-center justify-center bg-border after:absolute after:inset-y-0 after:left-1/2 after:w-2 after:-translate-x-1/2"
        role="separator"
        aria-orientation="vertical"
        aria-label="Resize sidebar"
        onPointerDown={beginSidebarResize}
      />
      <ResizablePanelGroup
        orientation="vertical"
        className="min-h-0 min-w-0 flex-1"
        defaultLayout={rightLayout.defaultLayout}
        onLayoutChanged={onRightLayoutChanged}
      >
        <ResizablePanel id="viewport" defaultSize={72} minSize={30} className="min-h-0">
          <main className="h-full min-w-0 overflow-hidden">
            <ViewportPanel />
          </main>
        </ResizablePanel>
        <ResizableHandle />
        <ResizablePanel id="assets" defaultSize={28} minSize={12} className="min-h-0 bg-background">
          <AssetsPanel />
        </ResizablePanel>
        {/* The bottom dock (timeline) renders only when a tool is open; closing the last
            tab removes it and the viewport/assets reclaim the height. Stacked in the same
            vertical group so react-resizable-panels persists its split — no separate
            layout reset. The handle drag fires onRightLayoutChanged → emitLayoutSettled,
            re-gluing the viewport subsurface on resize-end. */}
        {bottomToolsOpen ? (
          <>
            <ResizableHandle />
            <ResizablePanel
              id="bottom-dock"
              defaultSize={22}
              minSize={8}
              className="min-h-0 bg-background"
            >
              <BottomDock />
            </ResizablePanel>
          </>
        ) : null}
      </ResizablePanelGroup>
      {/* The right sidebar (perf tools) renders only when a tool is open; closing all its
          tabs removes it and the viewport reclaims the width. Pixel width like the left,
          so it cannot collapse while the native viewport attaches. */}
      {rightToolsOpen ? (
        <>
          <div
            className="relative flex w-px flex-none cursor-col-resize items-center justify-center bg-border after:absolute after:inset-y-0 after:left-1/2 after:w-2 after:-translate-x-1/2"
            role="separator"
            aria-orientation="vertical"
            aria-label="Resize tools sidebar"
            onPointerDown={beginRightResize}
          />
          <aside className="min-h-0 flex-none bg-background" style={{ width: `${rightWidth}px` }}>
            <RightSidebar />
          </aside>
        </>
      ) : null}
    </div>
  );
}

function clampSidebarWidth(width: number): number {
  const max = Math.max(
    SIDEBAR_MIN_WIDTH,
    Math.min(SIDEBAR_MAX_WIDTH, window.innerWidth - VIEWPORT_MIN_WIDTH),
  );
  return Math.min(Math.max(width, SIDEBAR_MIN_WIDTH), max);
}

/// The left-bottom dock node: Inspector, Environment, and Render tabbed into one node.
/// Keeping every panel in a non-viewport region avoids the native viewport painting over
/// them (see the file header).
function LeftBottomTabs() {
  // Store-controlled so other surfaces can switch tabs (e.g. an inspector deep-link);
  // manual tab clicks route through the same slice.
  const bottomTab = useEditorStore((s) => s.bottomTab);
  const setBottomTab = useEditorStore((s) => s.setBottomTab);
  const onTabChange = (value: string): void => {
    setBottomTab(value as BottomTab);
    requestAnimationFrame(() => emitLayoutSettled({ force: true }));
  };

  return (
    <Tabs
      value={bottomTab}
      className="flex h-full min-h-0 flex-col gap-0"
      onValueChange={onTabChange}
    >
      <TabsList
        variant="line"
        className="h-10 flex-none justify-start gap-0 rounded-none border-b border-border bg-background px-2"
      >
        <TabsTrigger value="inspector" className="px-3 text-xs">
          Inspector
        </TabsTrigger>
        <TabsTrigger value="environment" className="px-3 text-xs">
          Environment
        </TabsTrigger>
        <TabsTrigger value="render" className="px-3 text-xs">
          Render
        </TabsTrigger>
      </TabsList>
      <TabsContent value="inspector" className="flex min-h-0 flex-1 flex-col">
        <InspectorPanel />
      </TabsContent>
      <TabsContent value="environment" className="flex min-h-0 flex-1 flex-col">
        <EnvironmentPanel />
      </TabsContent>
      <TabsContent value="render" className="flex min-h-0 flex-1 flex-col">
        <RenderPanel />
      </TabsContent>
    </Tabs>
  );
}
