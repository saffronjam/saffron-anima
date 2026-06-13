import { client } from "../control/client";

/// Resolve once the presenter has DISPLAYED a fresh frame after a viewport resize — used to lift a
/// resize mask only when the engine's new-size frame has actually landed, instead of guessing with a
/// timer (which flashes the stretched old frame the compositor shows mid-resize, wayland_viewport.rs).
///
/// The presented counter advances on every displayed frame, so we wait for it to climb by `margin`
/// frames past the count captured right after the resize was committed: that skips the at-most-one
/// stretched frame the presenter may show before it re-attaches the engine's new-size frame. A fallback
/// timeout guarantees we never hang (e.g. if `wp_presentation` is unavailable, the count never moves).
export async function waitForFreshFrame(margin = 2, timeoutMs = 500): Promise<void> {
  let since: number;
  try {
    since = await client.viewportPresentedCount();
  } catch {
    return; // presentation feedback unavailable — don't block the reveal
  }
  const start = Date.now();
  while (Date.now() - start < timeoutMs) {
    await new Promise((resolve) => setTimeout(resolve, 16));
    let count: number;
    try {
      count = await client.viewportPresentedCount();
    } catch {
      return;
    }
    if (count >= since + margin) {
      return;
    }
  }
}
