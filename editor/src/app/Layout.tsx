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
/// once a split-drag settles.
import { useEffect, useRef, useState } from "react";
import { ResizableHandle, ResizablePanel, ResizablePanelGroup } from "@/components/ui/resizable";
import { Tabs, TabsContent, TabsList, TabsTrigger } from "@/components/ui/tabs";
import { HierarchyPanel } from "../panels/HierarchyPanel";
import { InspectorPanel } from "../panels/InspectorPanel";
import { EnvironmentPanel } from "../panels/EnvironmentPanel";
import { RenderStatsPanel } from "../panels/RenderStatsPanel";
import { AssetsPanel } from "../panels/AssetsPanel";
import { ViewportPanel } from "../panels/ViewportPanel";
import { emitLayoutSettled } from "./layoutBus";
import { useEditorStore, type BottomTab } from "../state/store";

const SIDEBAR_DEFAULT_WIDTH = 280;
const SIDEBAR_MIN_WIDTH = 240;
const SIDEBAR_MAX_WIDTH = 520;
const VIEWPORT_MIN_WIDTH = 520;

export function Layout() {
  const [sidebarWidth, setSidebarWidth] = useState(SIDEBAR_DEFAULT_WIDTH);
  const dragRef = useRef<{ startX: number; startWidth: number } | null>(null);
  const syncViewportAfterLayout = (): void => emitLayoutSettled();

  useEffect(() => {
    const onPointerMove = (event: PointerEvent): void => {
      const drag = dragRef.current;
      if (!drag) {
        return;
      }
      setSidebarWidth(clampSidebarWidth(drag.startWidth + event.clientX - drag.startX));
    };
    const onPointerUp = (): void => {
      if (!dragRef.current) {
        return;
      }
      dragRef.current = null;
      document.body.style.cursor = "";
      document.body.style.userSelect = "";
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

  return (
    <div className="flex min-h-0 min-w-0 flex-1">
      <aside className="min-h-0 flex-none bg-background" style={{ width: `${sidebarWidth}px` }}>
        <ResizablePanelGroup orientation="vertical" onLayoutChanged={syncViewportAfterLayout}>
          <ResizablePanel defaultSize={45} minSize={15} className="min-h-0 bg-background">
            <HierarchyPanel />
          </ResizablePanel>
          <ResizableHandle />
          <ResizablePanel defaultSize={55} minSize={15} className="min-h-0 bg-background">
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
        onLayoutChanged={syncViewportAfterLayout}
      >
        <ResizablePanel defaultSize={72} minSize={30} className="min-h-0">
          <main className="h-full min-w-0 overflow-hidden">
            <ViewportPanel />
          </main>
        </ResizablePanel>
        <ResizableHandle />
        <ResizablePanel defaultSize={28} minSize={12} className="min-h-0 bg-background">
          <AssetsPanel />
        </ResizablePanel>
      </ResizablePanelGroup>
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

/// The left-bottom dock node: Inspector, Environment, and Render Stats tabbed into
/// one node. Keeping every panel in a non-viewport region avoids the native viewport
/// painting over them (see the file header).
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
        <TabsTrigger value="stats" className="px-3 text-xs">
          Stats
        </TabsTrigger>
      </TabsList>
      <TabsContent value="inspector" className="flex min-h-0 flex-1 flex-col">
        <InspectorPanel />
      </TabsContent>
      <TabsContent value="environment" className="flex min-h-0 flex-1 flex-col">
        <EnvironmentPanel />
      </TabsContent>
      <TabsContent value="stats" className="flex min-h-0 flex-1 flex-col">
        <RenderStatsPanel />
      </TabsContent>
    </Tabs>
  );
}
