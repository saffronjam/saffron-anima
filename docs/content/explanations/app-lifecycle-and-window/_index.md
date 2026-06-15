+++
title = 'App lifecycle & window'
weight = 2
bookCollapseSection = true
+++

# App lifecycle & window

The application lifecycle is the path a Saffron Anima program follows from start to shutdown:
the `run` loop, the layers a client extends, and the SDL3 window with its typed event signals.
Every feature hangs off a layer callback.

## Pages

| Page | Covers | Code |
|---|---|---|
| [main-loop-and-run](main-loop-and-run/) | `AppConfig`, `sa::run`, the per-frame sequence, `onCreate`/`onExit` | `app.cppm` · `run` |
| `layer-system` | `Layer` as a struct of closures, `attachLayer`, the callback set | `app.cppm` · `Layer`, `attachLayer` |
| `the-submit-and-rendergraph-seams` | `onRender` submit seam vs. `onRenderGraph` pass authoring | `app.cppm` · `frameGraph`; `renderer.cppm` · `submit` |
| `window-and-events` | SDL3 window, typed signals (`onResize`/`onKeyPressed`/…), raw event sinks | `window.cppm` |
| `headless-and-capture` | `SAFFRON_EXIT_AFTER_FRAMES`, `SAFFRON_CAPTURE` | `app.cppm` · `frameLimitFromEnv`, `captureViewport` |
