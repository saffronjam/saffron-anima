/// The resizable dock layout, reproducing the C++ editor's default DockBuilder
/// arrangement (ui.cppm:568-577): Hierarchy + a tabbed Inspector/Environment/Stats
/// column on the LEFT, Assets along the BOTTOM, Viewport in the CENTER. The split
/// ratios mirror the DockBuilder splits — Left 0.20, Down 0.28 (Assets bottom),
/// leftBottom 0.55 (so Hierarchy is the top 0.45 of the left column).
///
/// Nested ResizablePanelGroups (react-resizable-panels via the shadcn wrapper):
///   outer vertical  : top region (~72) + Assets bottom (~28)
///   top horizontal  : left column (~20) + Viewport center
///   left vertical   : Hierarchy (~45) + tabbed panel (~55)
///
/// Every Radix popover/menu and every resize handle lives in a non-viewport region,
/// so none of them are occluded by the reparented native X11 window. The Viewport
/// panel owns the only host div the engine paints over; the LoadingOverlay is a
/// sibling inside ViewportPanel (NOT a panel the native window paints over).
///
/// `onLayoutChanged` (the stable, internally-debounced callback) pings the layout
/// bus so the ViewportPanel commits an exact resize-end bounds for the native window
/// once a split-drag settles.
import {
  ResizableHandle,
  ResizablePanel,
  ResizablePanelGroup,
} from "@/components/ui/resizable";
import { Tabs, TabsContent, TabsList, TabsTrigger } from "@/components/ui/tabs";
import { HierarchyPanel } from "../panels/HierarchyPanel";
import { InspectorPanel } from "../panels/InspectorPanel";
import { EnvironmentPanel } from "../panels/EnvironmentPanel";
import { RenderStatsPanel } from "../panels/RenderStatsPanel";
import { AssetsPanel } from "../panels/AssetsPanel";
import { ViewportPanel } from "../panels/ViewportPanel";
import { emitLayoutSettled } from "./layoutBus";

export function Layout() {
  return (
    <ResizablePanelGroup
      orientation="vertical"
      className="min-h-0 flex-1"
      onLayoutChanged={emitLayoutSettled}
    >
      <ResizablePanel defaultSize={72} minSize={30} className="min-h-0">
        <ResizablePanelGroup orientation="horizontal" onLayoutChanged={emitLayoutSettled}>
          <ResizablePanel defaultSize={20} minSize={12} className="min-w-0">
            <ResizablePanelGroup orientation="vertical" onLayoutChanged={emitLayoutSettled}>
              <ResizablePanel defaultSize={45} minSize={15} className="min-h-0 bg-card">
                <HierarchyPanel />
              </ResizablePanel>
              <ResizableHandle />
              <ResizablePanel defaultSize={55} minSize={15} className="min-h-0 bg-card">
                <LeftBottomTabs />
              </ResizablePanel>
            </ResizablePanelGroup>
          </ResizablePanel>
          <ResizableHandle />
          <ResizablePanel minSize={30} className="min-w-0 overflow-hidden">
            <ViewportPanel />
          </ResizablePanel>
        </ResizablePanelGroup>
      </ResizablePanel>
      <ResizableHandle />
      <ResizablePanel defaultSize={28} minSize={12} className="min-h-0 bg-card">
        <AssetsPanel />
      </ResizablePanel>
    </ResizablePanelGroup>
  );
}

/// The left-bottom dock node: ImGui tabs Inspector + Environment into one node
/// (ui.cppm:573-574). The C++ editor floats Render Stats as a separate window;
/// tabbing it here next to Inspector/Environment is the accepted parity choice
/// (keeps every panel in a non-viewport region — see the file header).
function LeftBottomTabs() {
  return (
    <Tabs defaultValue="inspector" className="flex h-full min-h-0 flex-col gap-0">
      <TabsList
        variant="line"
        className="h-9 flex-none justify-start gap-0 rounded-none border-b border-border bg-card px-1.5"
      >
        <TabsTrigger value="inspector" className="px-2 text-[11px]">
          Inspector
        </TabsTrigger>
        <TabsTrigger value="environment" className="px-2 text-[11px]">
          Environment
        </TabsTrigger>
        <TabsTrigger value="stats" className="px-2 text-[11px]">
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
