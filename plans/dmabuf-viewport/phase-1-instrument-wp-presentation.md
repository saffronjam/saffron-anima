# Phase 1 — instrument with wp_presentation

**Status:** COMPLETED

The presenter binds `wp_presentation` and requests a feedback object per subsurface commit,
counting per second: `presented`/s, `discarded`/s, the mean hardware vblank sequence delta
between presented frames, the refresh period, and the accumulated presentation flags
(`vsync`, `hw-clock`, `hw-completion`, `zero-copy`). The line logs behind
`SAFFRON_VIEWPORT_STATS=1` (`PresentationStats` in `editor/src-tauri/src/wayland_viewport.rs`).

## Findings (2026-06, GNOME mutter 49 / NVIDIA, 240Hz panel)

1. **Presentation runs at the monitor rate.** ~240 presented/s, 0 discarded, vblank delta
   1.00, flags `vsync+hw-clock+hw-completion`. The "compositor discards most commits"
   theory was wrong; commits paced by frame callbacks land one per refresh.
2. **The felt-60 was input, not presentation.** Webview pointer events arrive at ~60Hz.
   Engine-side drag smoothing (`stepNativeGizmoDrag`, 25ms ease toward the latest sample)
   and `fly-input` streaming fixed the perceived rate; fast engine-driven motion verified
   smooth by eye at 240Hz.
3. **`presented` does not certify buffer-to-glass.** In mutter, feedback is keyed to the
   repaint frame counter with "surface was primary on the view" as the only gate
   (`meta-wayland-presentation-time.c`); the present path never consults buffer state, and
   the vblank seq comes from the repaint. Delta 1.00 appears even if a texture repeats
   (mutter issues #3937, #3725 confirm the bookkeeping). Pair the counters with an eyeball
   test on fast motion.
4. **The shm upload is real and per-commit.** Mutter uploads synchronously at commit-apply
   time (`process_shm_buffer_damage` → `_cogl_texture_set_region`, no skip path): at
   1600×900 XRGB8888 × 240/s that is ~1.3 GB/s of blocking CPU→GPU traffic on the
   compositor thread. This, plus the release-less ring, motivates phase 2.

After phase 2 this instrumentation is the cheap regression check: `zero-copy` must appear
in the flags, `discarded` stays ~0, presented/s stays at the monitor rate.
