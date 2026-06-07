import type { CSSProperties } from "react";
import { Toaster as Sonner, type ToasterProps } from "sonner";

/// The shadcn Sonner wrapper, themed via the app's CSS variables (the editor is
/// dark-only, so the next-themes hook is replaced with a fixed theme).
function Toaster({ ...props }: ToasterProps) {
  return (
    <Sonner
      theme="dark"
      position="bottom-right"
      className="toaster group"
      style={
        {
          "--normal-bg": "var(--popover)",
          "--normal-text": "var(--popover-foreground)",
          "--normal-border": "var(--border)",
        } as CSSProperties
      }
      {...props}
    />
  );
}

export { Toaster };
