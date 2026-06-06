# Phase 3 — pacing polish and native-rate input

**Status:** NOT STARTED

Only after phase 2; these refine cadence and input latency on top of the zero-copy path.

## Commit pacing

- `fifo-v1` + `commit-timing-v1` (in mutter since GNOME 48) for deadline-accurate commit
  cadence instead of the frame-callback-or-timeout loop.
- Revisit the engine cap: `SAFFRON_MAX_FPS` defaults to 500 from the editor spawn; with a
  release-driven lifecycle the cap is about power, not correctness — tie it to ~2× the
  fastest monitor rate instead of a constant.

## Native-rate viewport input

Today every input path rides the webview at its ~60Hz pointer cadence (`gizmo-pointer`,
`fly-input`), with engine-side smoothing hiding the sample staircase. The presenter
already owns a Wayland connection: bind `wl_pointer` (+ `relative-pointer-unstable-v1`)
there and forward compositor-delivered events over the control socket. That removes both
the webview cadence cap and the JS→IPC→socket latency during drags and flying — rate and
latency then match a native window.

Scoping note: the subsurface receives pointer events only where its surface is not
occluded by the (input-transparent?) toplevel — GTK's surface takes the input. The likely
shape is keeping DOM events as the *gesture* source (begin/end, focus) and the raw
`relative-pointer` stream for the *motion* while a gesture is active. Decide against the
real protocol behaviour when implementing.
