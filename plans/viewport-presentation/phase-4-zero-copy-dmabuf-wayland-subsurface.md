# Phase 4 — Zero-copy restore: dmabuf image + Wayland subsurface

**Status:** NOT STARTED (measurement-gated — may be deleted unstarted)

**Depends on:** phase 3

## When this phase runs

Only if phase 2's instrumentation shows the frame-streaming latency (GPU→CPU readback +
transport + texture upload, ~1 frame) is unacceptable for camera-orbit / gizmo-drag at the
viewport sizes you actually use. If streaming feels fine — likely for an editor on a
3070 Ti — **this phase is not built**, and the plan is done at phase 3. Do not start it on
spec; start it on a measured number (Open Question #1).

## Goal

Get back zero-copy GPU presentation *without* reintroducing the overlapping native X11 child
that started all this. Render the viewport into a dmabuf-backed Vulkan image and composite it
as a **Wayland `wl_subsurface`** under the webview's surface. A subsurface is the
compositor's native mechanism for stacking a separate buffer (how video players and games
embed) — Mutter composites it in hardware **without** forcing the parent webview off its
render path. So this keeps the chrome smooth (the phase-3 win) *and* removes the per-frame
copy (restoring viewport latency to the current native-child design).

## Prior art

A 417-line GTK4 fd-export prototype (`viewport_bridge.rs`) exists in git history — referenced
in [`../typescript-ui-migration/README.md`](../typescript-ui-migration/README.md) as the
DMA-BUF path that was *not* forward-ported. It is a **starting reference for the export
mechanics, not a drop-in**: it predates the current renderer module split and never shipped.

## Spike first (Open Question #4)

Before committing, prove the two uncertain pieces in a throwaway branch:

1. **Surface access.** Can the editor reach the webview's `wl_surface` (via wry/gdk-wayland)
   to parent a `wl_subsurface` to it, position it over the viewport rect, and clip it to the
   panel bounds during dock resizes? This is the highest-risk unknown.
2. **dmabuf round-trip.** Engine allocates an image with `VK_EXT_external_memory_dma_buf` +
   `VK_KHR_external_memory_fd`, exports the fd, and the compositor scans it out via a
   `linux-dmabuf` `wl_buffer` with a matching DRM format/modifier (the NVIDIA path needs
   explicit-modifier support; the engine currently uses **no** external-memory extensions —
   `grep` confirms zero usage today).

If either spike fails, keep frame-streaming (phases 1–3) and close this phase as "not viable
on this stack."

## Steps (only after the spike passes)

1. **`Dmabuf` viewport transport** (extends phase 1's `viewportTransport` enum). Allocate
   `targets.offscreen` (or a dedicated present image) as an external-memory dmabuf image;
   add the device extensions in `Dependencies.cmake` / device creation
   (`renderer.cppm:102` selector). Export the fd + DRM format/modifier over a new control
   command.
2. **Subsurface compositing in the editor shell** (`lib.rs`): import the fd as a
   `linux-dmabuf` `wl_buffer`, attach it to a `wl_subsurface` parented under the webview
   surface, place/clip it to the viewport rect, and commit on each engine `seqno`. Double-
   buffer to avoid tearing; sync subsurface commits to the parent.
3. **Input/lifecycle reuse.** Pointer mapping, pick/gizmo, size negotiation, and the
   first-frame `ready` lifecycle from phase 3 are unchanged — only the pixel transport
   differs. The canvas becomes a transparent placeholder owning the rect (or is removed in
   favor of the subsurface rect), with the surface composited behind/over per stacking needs.
4. **Re-evaluate dropping X11.** This path *requires* native Wayland (no XWayland), which
   phase 3 already established. Confirm the subsurface approach has no XWayland fallback need.

## Validation

- Chrome stays at display refresh (phase-3 result preserved) **and** viewport latency matches
  the old native-child build (no readback frame) — measured, side by side.
- No tearing on fast camera moves; correct clipping/stacking when the viewport panel is
  resized or a modal opens over it.
- Validation-clean Vulkan log with the external-memory extensions enabled.

## Risks

- **wry/gdk surface access may simply not be exposed** → spike-1 fails → phase abandoned.
- **NVIDIA dmabuf explicit modifiers + Mutter** interop is historically fiddly; the spike
  must use the real GPU (the toolbox NVIDIA ICD wired during the debug session), not llvmpipe.
- Highest complexity in the plan for a latency win that may not be needed — hence
  measurement-gated and last.
