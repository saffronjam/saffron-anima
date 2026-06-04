/// Custom Tauri titlebar with editor view tabs.
import { getCurrentWindow } from "@tauri-apps/api/window";
import { Maximize2, Minus, Square, X } from "lucide-react";
import { useEffect, useState } from "react";
import type { MouseEvent, ReactNode } from "react";
import { Button } from "@/components/ui/button";
import { cn } from "@/lib/utils";

const appWindow = getCurrentWindow();
const TITLEBAR_TABS = [{ id: "scene", label: "Scene" }];

export function WindowTitlebar() {
  const [maximized, setMaximized] = useState(false);

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

  return (
    <header
      className="flex h-9 flex-none items-center border-b border-border bg-card"
      data-tauri-drag-region
      onMouseDown={beginTitlebarDrag}
    >
      <div className="flex h-full min-w-0 flex-none items-end px-2" role="tablist">
        {TITLEBAR_TABS.map((tab) => (
          <TitlebarTab key={tab.id} active label={tab.label} />
        ))}
      </div>
      <div className="min-w-0 flex-1 self-stretch" data-tauri-drag-region />
      <div
        className="flex w-32 flex-none justify-end"
        data-tauri-drag-region="false"
        data-titlebar-control="true"
      >
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
  label: string;
};

function TitlebarTab({ active, label }: TitlebarTabProps) {
  return (
    <button
      type="button"
      className={cn(
        "h-8 min-w-28 max-w-48 truncate rounded-t-md border px-4 text-left text-sm font-medium",
        active
          ? "border-border border-b-background bg-background text-foreground"
          : "border-transparent text-muted-foreground hover:bg-accent hover:text-accent-foreground",
      )}
      aria-selected={active}
      role="tab"
      title={label}
    >
      {label}
    </button>
  );
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
      title={label}
      onClick={onClick}
    >
      {children}
    </Button>
  );
}
