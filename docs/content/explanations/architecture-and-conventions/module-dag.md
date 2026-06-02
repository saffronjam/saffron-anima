+++
title = 'Module DAG'
weight = 4
+++

# Module DAG

The engine modules form a directed acyclic graph of real imports, not a single chain. Knowing
which module sits above which tells you where code belongs, and why some glue lives in its own
module.

## The graph

Each area imports only what it needs. `Core` is the root (aliases, `Result`, logging); everything
depends on it directly or transitively. Read top to bottom:

```mermaid
flowchart TD
    Core[Saffron.Core]
    Signal[Saffron.Signal]
    Json[Saffron.Json]
    Window[Saffron.Window]
    Geometry[Saffron.Geometry]
    Scene[Saffron.Scene]
    Rendering[Saffron.Rendering<br/>+ :RenderGraph :Types :Detail]
    Ui[Saffron.Ui]
    Assets[Saffron.Assets]
    SceneEdit[Saffron.SceneEdit]
    Control[Saffron.Control]
    App[Saffron.App]
    Host[Saffron.Host]

    Signal --> Core
    Json --> Core
    Window --> Core
    Window --> Signal
    Geometry --> Core
    Scene --> Core
    Scene --> Json
    Rendering --> Core
    Rendering --> Window
    Rendering --> Geometry
    Ui --> Core
    Ui --> Window
    Ui --> Rendering
    Assets --> Core
    Assets --> Json
    Assets --> Geometry
    Assets --> Rendering
    Assets --> Scene
    SceneEdit --> Core
    SceneEdit --> Signal
    SceneEdit --> Scene
    SceneEdit --> Json
    SceneEdit --> Ui
    Control --> SceneEdit
    Control --> Rendering
    Control --> Assets
    App --> Core
    App --> Window
    App --> Rendering
    App --> Ui
    Host --> App
    Host --> SceneEdit
    Host --> Control
    Host --> Assets
```

`Saffron.Rendering` is itself split (see [module partitions](../module-partitions/)): the primary
re-exports the `:RenderGraph` and `:Types` partitions, so a consumer importing `Saffron.Rendering`
gets the render-graph and renderer types in one import.

## Why Host is its own module

The host application glue — the `Layer` callbacks, thumbnail cache, import routing, the
`onCreate`/`onExit` closures — calls into `App`, `SceneEdit`, `Control`, `Assets`, and the rest at
once. It cannot live in `Saffron.SceneEdit`, because `Control` already imports `SceneEdit`
(`Control → SceneEdit`); glue inside `SceneEdit` reaching back into `Control` would form a cycle. So it
sits in a separate module above everything:

```cpp
export module Saffron.Host;

import Saffron.App;
import Saffron.SceneEdit;
import Saffron.Control;
import Saffron.Assets;
// ...
export auto runHost(const char* title, int w, int h) -> int;
```

That keeps the graph acyclic. The host executable's `main.cpp` is then a six-line stub that
imports `Saffron.Host` and calls `runHost`.

> [!NOTE]
> `Saffron.Host` exists only because `Control → SceneEdit` already holds. The host glue
> needs both, so it lives in a module above both rather than inside `SceneEdit`, which would cycle.

## In the code

| What | File | Symbols |
|---|---|---|
| Module list + order | `engine/CMakeLists.txt` | `FILE_SET CXX_MODULES FILES` |
| Root module | `core.cppm` | `export module Saffron.Core;` |
| Re-exported partitions | `renderer.cppm` | `export import :RenderGraph;`, `export import :Types;` |
| Top-of-graph glue | `host.cppm` | `export module Saffron.Host;`, `runHost` |
| Thin entry point | `engine/source/main.cpp` | `import Saffron.Host;`, `se::runHost` |

## Related
- [Module partitions](../module-partitions/) — how `Saffron.Rendering` is split internally
- [C++26 modules](../cxx26-modules/) — the module mechanism itself
- [Main loop](../../app-lifecycle-and-window/main-loop-and-run/) — what `Saffron.App` exposes to `EditorApp`
