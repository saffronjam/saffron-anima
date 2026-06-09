/// Script-error toasts: the reconcile poll drains the engine's contained script
/// errors (which also pause play) and raises one persistent toast per script path
/// per play session — the traceback's first lines inline, so the author sees what
/// broke without leaving the editor. The engine clears its ring on every enterPlay;
/// a new session may legitimately re-toast the same script.
import { toast } from "sonner";
import type { ScriptErrorDto } from "../protocol";

const toastedScripts = new Set<string>();

/// The message head: "boom" plus the chunk/line, not the whole multi-frame traceback.
function describe(event: ScriptErrorDto): string {
  const head = event.message.split("\nstack traceback")[0] ?? event.message;
  return `Script error in ${event.script}\n${head.trim()}`;
}

export function routeScriptErrorToasts(events: ScriptErrorDto[]): void {
  for (const event of events) {
    if (toastedScripts.has(event.script)) {
      continue;
    }
    toastedScripts.add(event.script);
    toast.error(describe(event), { duration: 12_000 });
  }
}

/// A new play session starts a fresh ring engine-side; mirror that here so the
/// same script can report again next session.
export function resetScriptErrorToasts(): void {
  toastedScripts.clear();
}
