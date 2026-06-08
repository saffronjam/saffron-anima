/// Custom Tauri titlebar with editor view tabs.
import { getCurrentWindow } from "@tauri-apps/api/window";
import {
  Box,
  File,
  House,
  Image as ImageIcon,
  Maximize2,
  Minus,
  Settings,
  Square,
  X,
} from "lucide-react";
import { useEffect, useLayoutEffect, useRef, useState } from "react";
import type { CSSProperties, MouseEvent, PointerEvent, ReactNode } from "react";
import { useEditorStore, type ViewTab } from "../state/store";
import { Button } from "@/components/ui/button";
import { Tooltip, TooltipContent, TooltipTrigger } from "@/components/ui/tooltip";
import { cn } from "@/lib/utils";

const appWindow = getCurrentWindow();
const TAB_DRAG_THRESHOLD_PX = 4;
/// Hidden dev-mode gesture: five Scene-tab clicks, each within this gap of the last.
const DEV_GESTURE_CLICKS = 5;
const DEV_GESTURE_WINDOW_MS = 600;

interface TabDragState {
  id: string;
  startX: number;
  currentX: number;
  dragging: boolean;
  startIndex: number;
  previewIndex: number;
  width: number;
  order: string[];
  centers: Record<string, number>;
}

export function WindowTitlebar() {
  const [maximized, setMaximized] = useState(false);
  const [tabDrag, setTabDrag] = useState<TabDragState | null>(null);
  const tabRefs = useRef(new Map<string, HTMLButtonElement>());
  const settleRef = useRef<Map<string, number> | null>(null);
  const tabs = useEditorStore((s) => s.viewTabs);
  const activeTabId = useEditorStore((s) => s.activeViewTabId);
  const setActiveViewTab = useEditorStore((s) => s.setActiveViewTab);
  const closeViewTab = useEditorStore((s) => s.closeViewTab);
  const moveViewTab = useEditorStore((s) => s.moveViewTab);
  const setSettingsOpen = useEditorStore((s) => s.setSettingsOpen);
  const devMode = useEditorStore((s) => s.devMode);
  const setDevMode = useEditorStore((s) => s.setDevMode);
  const devClickCount = useRef(0);
  const devClickAt = useRef(0);

  // Activate the Scene tab and track the hidden dev-mode gesture: five clicks in a
  // row (each within DEV_GESTURE_WINDOW_MS of the last) toggle developer mode.
  const activateSceneTab = (): void => {
    setActiveViewTab("scene");
    const now = Date.now();
    devClickCount.current =
      now - devClickAt.current > DEV_GESTURE_WINDOW_MS ? 1 : devClickCount.current + 1;
    devClickAt.current = now;
    if (devClickCount.current >= DEV_GESTURE_CLICKS) {
      devClickCount.current = 0;
      setDevMode(!devMode);
    }
  };

  useEffect(() => {
    let cancelled = false;

    const syncMaximized = async (): Promise<void> => {
      const nextMaximized = await appWindow.isMaximized();
      if (!cancelled) {
        setMaximized(nextMaximized);
      }
    };

    void syncMaximized();
    const unlisten = appWindow.onResized(() => {
      void syncMaximized();
    });

    return () => {
      cancelled = true;
      void unlisten.then((off) => off());
    };
  }, []);

  const minimize = (): void => {
    void appWindow.minimize();
  };

  const toggleMaximize = async (): Promise<void> => {
    await appWindow.toggleMaximize();
    setMaximized(await appWindow.isMaximized());
  };

  const close = (): void => {
    void appWindow.close();
  };

  const beginTitlebarDrag = (event: MouseEvent<HTMLElement>): void => {
    if (event.button !== 0) {
      return;
    }

    const target = event.target;
    if (target instanceof Element && target.closest("[data-titlebar-control='true']")) {
      return;
    }

    if (event.detail === 2) {
      void toggleMaximize();
      return;
    }

    void appWindow.startDragging();
  };

  // FLIP settle for a tab drop. The drop render reorders the DOM, clears the drag
  // transforms, and re-enables the tabs' transitions all in one commit — left alone,
  // the browser would transition each moved tab from `new slot + old drag transform`,
  // a phantom slide from the wrong side. Before paint: suppress the transitions
  // (style writes do not force a recalc), so the first rect read commits the
  // transform removal without starting one and measures true final layout; then
  // animate each tab that would jump from its pre-drop visual position into its
  // slot via WAAPI. Neighbor tabs have zero diff (the drag preview already placed
  // them at their final positions), so only the dragged tab settles, from under
  // the cursor.
  useLayoutEffect(() => {
    const before = settleRef.current;
    if (!before) {
      return;
    }
    settleRef.current = null;
    const nodes = [...tabRefs.current.entries()];
    for (const [, node] of nodes) {
      node.style.transition = "none";
    }
    const settles: [HTMLButtonElement, number][] = [];
    for (const [id, node] of nodes) {
      const left = before.get(id);
      if (left === undefined) {
        continue;
      }
      const diff = left - node.getBoundingClientRect().left;
      if (Math.abs(diff) >= 0.5) {
        settles.push([node, diff]);
      }
    }
    for (const [, node] of nodes) {
      node.style.transition = "";
    }
    for (const [node, diff] of settles) {
      node.animate([{ transform: `translateX(${diff}px)` }, { transform: "none" }], {
        duration: 150,
        easing: "ease-out",
      });
    }
  });

  const setTabRef = (id: string, node: HTMLButtonElement | null): void => {
    if (node) {
      tabRefs.current.set(id, node);
    } else {
      tabRefs.current.delete(id);
    }
  };

  const insertionIndexForPointer = (drag: TabDragState, x: number): number => {
    const withoutMoving = drag.order.filter((id) => id !== drag.id);
    for (let i = 0; i < withoutMoving.length; i += 1) {
      const id = withoutMoving[i];
      if (id === "scene") {
        continue;
      }
      const center = drag.centers[id];
      if (center === undefined) {
        continue;
      }
      if (x < center) {
        return i;
      }
    }
    return withoutMoving.length;
  };

  const beginTabDrag = (tab: ViewTab, event: PointerEvent<HTMLButtonElement>): void => {
    if (!tab.closable || event.button !== 0) {
      return;
    }
    const target = event.target;
    if (target instanceof Element && target.closest("[data-tab-close='true']")) {
      return;
    }
    const node = tabRefs.current.get(tab.id);
    if (!node) {
      return;
    }
    const order = tabs.map((candidate) => candidate.id);
    const startIndex = order.indexOf(tab.id);
    if (startIndex < 0) {
      return;
    }
    const centers: Record<string, number> = {};
    for (const id of order) {
      const tabNode = tabRefs.current.get(id);
      if (tabNode) {
        const rect = tabNode.getBoundingClientRect();
        centers[id] = rect.left + rect.width / 2;
      }
    }
    event.preventDefault();
    event.currentTarget.setPointerCapture(event.pointerId);
    setTabDrag({
      id: tab.id,
      startX: event.clientX,
      currentX: event.clientX,
      dragging: false,
      startIndex,
      previewIndex: startIndex,
      width: node.getBoundingClientRect().width,
      order,
      centers,
    });
  };

  const moveTabDrag = (event: PointerEvent<HTMLButtonElement>): void => {
    if (!tabDrag) {
      return;
    }
    const delta = event.clientX - tabDrag.startX;
    const dragging = tabDrag.dragging || Math.abs(delta) >= TAB_DRAG_THRESHOLD_PX;
    setTabDrag({
      ...tabDrag,
      currentX: event.clientX,
      dragging,
      previewIndex: dragging
        ? insertionIndexForPointer(tabDrag, event.clientX)
        : tabDrag.previewIndex,
    });
  };

  const endTabDrag = (tab: ViewTab, event: PointerEvent<HTMLButtonElement>): void => {
    if (!tabDrag || tabDrag.id !== tab.id) {
      return;
    }
    if (event.currentTarget.hasPointerCapture(event.pointerId)) {
      event.currentTarget.releasePointerCapture(event.pointerId);
    }
    const activate = !tabDrag.dragging;
    const nextIndex = tabDrag.previewIndex;
    if (tabDrag.dragging) {
      const lefts = new Map<string, number>();
      for (const [id, node] of tabRefs.current) {
        lefts.set(id, node.getBoundingClientRect().left);
      }
      settleRef.current = lefts;
    }
    setTabDrag(null);
    if (activate) {
      setActiveViewTab(tab.id);
    } else {
      moveViewTab(tabDrag.id, nextIndex);
    }
  };

  const tabStyle = (tab: ViewTab): CSSProperties | undefined => {
    if (!tabDrag?.dragging) {
      return undefined;
    }
    if (tab.id === tabDrag.id) {
      return { transform: `translateX(${tabDrag.currentX - tabDrag.startX}px)` };
    }
    const index = tabDrag.order.indexOf(tab.id);
    if (
      tabDrag.previewIndex > tabDrag.startIndex &&
      index > tabDrag.startIndex &&
      index <= tabDrag.previewIndex
    ) {
      return { transform: `translateX(-${tabDrag.width}px)` };
    }
    if (
      tabDrag.previewIndex < tabDrag.startIndex &&
      index >= tabDrag.previewIndex &&
      index < tabDrag.startIndex
    ) {
      return { transform: `translateX(${tabDrag.width}px)` };
    }
    return undefined;
  };

  return (
    <header
      className="flex h-9 flex-none items-center border-b border-border bg-card"
      data-tauri-drag-region
      onMouseDown={beginTitlebarDrag}
    >
      <div
        className="flex h-full min-w-0 flex-none items-end px-2"
        role="tablist"
        data-titlebar-control="true"
      >
        {tabs.map((tab) => (
          <TitlebarTab
            key={tab.id}
            tab={tab}
            active={tab.id === activeTabId}
            dragging={tabDrag?.id === tab.id && tabDrag.dragging}
            style={tabStyle(tab)}
            setRef={(node) => setTabRef(tab.id, node)}
            onActivate={() => (tab.id === "scene" ? activateSceneTab() : setActiveViewTab(tab.id))}
            onClose={() => closeViewTab(tab.id)}
            onPointerDown={(event) => beginTabDrag(tab, event)}
            onPointerMove={moveTabDrag}
            onPointerUp={(event) => endTabDrag(tab, event)}
            onPointerCancel={() => setTabDrag(null)}
          />
        ))}
      </div>
      <div className="min-w-0 flex-1 self-stretch" data-tauri-drag-region />
      <div
        className="flex w-44 flex-none justify-end"
        data-tauri-drag-region="false"
        data-titlebar-control="true"
      >
        <Tooltip>
          <TooltipTrigger asChild>
            <TitlebarButton label="Editor settings" onClick={() => setSettingsOpen(true)}>
              <Settings />
            </TitlebarButton>
          </TooltipTrigger>
          <TooltipContent>Editor settings</TooltipContent>
        </Tooltip>
        <TitlebarButton label="Minimize" onClick={minimize}>
          <Minus />
        </TitlebarButton>
        <TitlebarButton
          label={maximized ? "Restore" : "Maximize"}
          onClick={() => void toggleMaximize()}
        >
          {maximized ? <Square /> : <Maximize2 />}
        </TitlebarButton>
        <TitlebarButton label="Close" onClick={close} variant="close">
          <X />
        </TitlebarButton>
      </div>
    </header>
  );
}

type TitlebarTabProps = {
  active: boolean;
  tab: ViewTab;
  dragging: boolean;
  style?: CSSProperties;
  setRef(node: HTMLButtonElement | null): void;
  onActivate(): void;
  onClose(): void;
  onPointerDown(event: PointerEvent<HTMLButtonElement>): void;
  onPointerMove(event: PointerEvent<HTMLButtonElement>): void;
  onPointerUp(event: PointerEvent<HTMLButtonElement>): void;
  onPointerCancel(): void;
};

function TitlebarTab({
  active,
  tab,
  dragging,
  style,
  setRef,
  onActivate,
  onClose,
  onPointerDown,
  onPointerMove,
  onPointerUp,
  onPointerCancel,
}: TitlebarTabProps) {
  const Icon = tabIcon(tab);
  return (
    <button
      ref={setRef}
      type="button"
      className={cn(
        "flex h-8 min-w-28 max-w-48 items-center gap-2 rounded-t-md border px-3 text-left text-sm font-medium",
        "transition-[transform,margin,background-color,border-color,color] duration-150 ease-out",
        tab.closable ? "cursor-default" : "cursor-pointer",
        dragging && "relative z-10 cursor-grabbing shadow-lg transition-none",
        active
          ? "border-border border-b-background bg-background text-foreground"
          : "border-transparent text-muted-foreground hover:bg-accent hover:text-accent-foreground",
      )}
      style={style}
      aria-selected={active}
      role="tab"
      onClick={() => {
        if (!tab.closable) {
          onActivate();
        }
      }}
      onPointerDown={onPointerDown}
      onPointerMove={onPointerMove}
      onPointerUp={onPointerUp}
      onPointerCancel={onPointerCancel}
    >
      <Icon className="size-4 flex-none" />
      <span className="min-w-0 flex-1 truncate">{tab.title}</span>
      {tab.closable ? (
        <span
          role="button"
          tabIndex={0}
          aria-label={`Close ${tab.title}`}
          data-tab-close="true"
          className="flex size-5 flex-none items-center justify-center rounded-sm hover:bg-accent"
          onClick={(event) => {
            event.stopPropagation();
            onClose();
          }}
          onKeyDown={(event) => {
            if (event.key === "Enter" || event.key === " ") {
              event.preventDefault();
              onClose();
            }
          }}
        >
          <X className="size-3.5" />
        </span>
      ) : null}
    </button>
  );
}

function tabIcon(tab: ViewTab) {
  if (tab.kind === "scene") {
    return House;
  }
  if (tab.assetType === "mesh") {
    return Box;
  }
  if (tab.assetType === "texture") {
    return ImageIcon;
  }
  return File;
}

type TitlebarButtonProps = {
  children: ReactNode;
  label: string;
  onClick: () => void;
  variant?: "default" | "close";
};

function TitlebarButton({ children, label, onClick, variant = "default" }: TitlebarButtonProps) {
  const className =
    variant === "close"
      ? "h-9 w-11 rounded-none hover:bg-destructive hover:text-destructive-foreground"
      : "h-9 w-11 rounded-none hover:bg-accent";

  return (
    <Button
      type="button"
      size="icon-sm"
      variant="ghost"
      className={className}
      aria-label={label}
      onClick={onClick}
    >
      {children}
    </Button>
  );
}
