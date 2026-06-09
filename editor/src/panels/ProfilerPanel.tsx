/// The Profiler tool (right sidebar): capture controls in the header, then the per-pass GPU
/// table over the last capture. A self-contained surface peer to the always-on Stats HUD (never
/// folded into it). Request-scoped: it fetches only on Capture, adds no polling lane. The flame
/// chart lives in its own main tab (opened from the capture controls' Flame button).
import { useEditorStore } from "../state/store";
import { CaptureControls } from "../components/CaptureControls";
import { CaptureTable } from "../components/CaptureTable";
import { ScrollArea } from "@/components/ui/scroll-area";
import { cn } from "@/lib/utils";

export function ProfilerPanel() {
  const capture = useEditorStore((s) => s.capture);
  const captureState = useEditorStore((s) => s.captureState);

  const meta = capture?.metadata;
  const softwareGpu = meta?.softwareGpu ?? false;
  const uncorrelated = meta !== undefined && !meta.correlated;
  const recording = captureState === "recording" || captureState === "arming";

  return (
    <div className="flex h-full min-h-0 flex-col">
      <div
        className={cn(
          "flex-none border-b border-border p-2.5 transition-colors",
          recording && "bg-red-500/5",
        )}
      >
        <CaptureControls />
      </div>
      <ScrollArea className="min-h-0 flex-1">
        <div className="flex flex-col gap-2 p-2.5">
          {softwareGpu ? (
            <p className="rounded-sm border border-amber-500/40 bg-amber-500/10 px-2 py-1 text-[10px] leading-snug text-amber-300">
              Software rasterizer (llvmpipe) — GPU timings are CPU rasterization time, not
              representative of hardware.
            </p>
          ) : null}
          {uncorrelated ? (
            <p className="rounded-sm border border-sky-500/40 bg-sky-500/10 px-2 py-1 text-[10px] leading-snug text-sky-300">
              GPU spans are uncorrelated (no calibrated timestamps) — the GPU lane is shown on its
              own zero, not the CPU axis.
            </p>
          ) : null}

          {capture === null ? (
            <div className="flex flex-col items-center gap-1 py-10 text-center text-xs text-muted-foreground">
              <p className="text-sm">No capture yet</p>
              <p>Click Capture to profile a frame's CPU + GPU passes on one timeline.</p>
            </div>
          ) : (
            <CaptureTable />
          )}
        </div>
      </ScrollArea>
    </div>
  );
}
