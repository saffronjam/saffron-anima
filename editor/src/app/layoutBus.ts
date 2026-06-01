/// A tiny synchronous event bus that the resizable dock `Layout` pings whenever a
/// PanelGroup's layout settles (`onLayoutChanged`), so the `ViewportPanel` can fire
/// an exact resize-end commit for the reparented native window. This is decoupled
/// from the Zustand store on purpose: a panel-split layout change is a transient UI
/// signal, not editor state, so it should not churn the store or trigger renders.
///
/// The ViewportPanel's ResizeObserver already catches the host div's geometry change
/// during a drag (throttled live sync); this bus only adds the *final exact* commit
/// on drag-end, diff-guarded inside the subscriber.
type LayoutListener = () => void;

const listeners = new Set<LayoutListener>();

/// Subscribe to layout-settled notifications. Returns an unsubscribe fn.
export function onLayoutSettled(listener: LayoutListener): () => void {
  listeners.add(listener);
  return () => {
    listeners.delete(listener);
  };
}

/// Notify every subscriber that a dock layout just settled. Called from the
/// `Layout` PanelGroup `onLayoutChanged` callbacks.
export function emitLayoutSettled(): void {
  for (const listener of listeners) {
    listener();
  }
}
