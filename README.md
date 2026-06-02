<h1 align="center">SaffronEngine</h1>

<p align="center">
  A from-scratch Vulkan renderer and C++26 game engine, driven by a Tauri editor.
</p>

<p align="center">
  <a href="https://github.com/saffronjam/saffron-engine/actions/workflows/ci.yml"><img src="https://github.com/saffronjam/saffron-engine/actions/workflows/ci.yml/badge.svg" alt="CI" /></a>
  <img src="https://img.shields.io/badge/C%2B%2B-26-00599C.svg?logo=cplusplus&logoColor=white" alt="C++26" />
  <img src="https://img.shields.io/badge/Vulkan-1.4-A41E22.svg?logo=vulkan&logoColor=white" alt="Vulkan 1.4" />
  <img src="https://img.shields.io/badge/Tauri-2-24C8DB.svg?logo=tauri&logoColor=white" alt="Tauri 2" />
</p>

---

A C++26 static library that builds its own present-only viewport host (`SaffronEngine`). The editor is a separate Tauri/React app that embeds that host as a native child window and drives it over a JSON-over-socket control plane.

## Features

- Vulkan 1.4 via Vulkan-Hpp (no exceptions), dynamic rendering + sync2, VMA.
- A render graph: passes declare resource usage, the graph derives every barrier.
- Clustered-forward PBR, IBL, shadows (incl. contact + ray-traced).
- DDGI, voxel GI, SSGI, ReSTIR; GTAO, TAA, motion vectors, tonemap; MSAA + FXAA.
- Bindless textures + instanced draws; an übershader with a keyed PSO cache.
- entt ECS, registry-driven JSON scene/project format, glTF + OBJ import.
- A unix-socket control plane and `se` CLI that script the running editor.

## Build & run

Everything builds inside the `saffron-build` toolbox (see [`AGENTS.md`](AGENTS.md)):

```sh
toolbox run -c saffron-build bash -lc '
  cmake --preset debug
  cmake --build build/debug -j1
  ./build/debug/bin/SaffronEngine'   # the present-only host
```

The editor lives in `editor/`; it spawns and embeds the host:

```sh
cd editor && bun install && bun run tauri dev
```

Convenience targets wrap the above (run inside the toolbox; `make help` lists them): `make run` starts
the editor, `make run-engine` only the host, `make run-docs` the docs site, and `make format` /
`make lint` / `make prepare-for-commit` cover style (clang-format + clang-tidy for C++, oxfmt + oxlint
for TypeScript).

## More

- Concept-by-concept docs: [`docs/content/overview.md`](docs/content/overview.md) (Hugo site).
- Toolchain, architecture, conventions: [`AGENTS.md`](AGENTS.md).
- Code style: [`CONVENTIONS.md`](CONVENTIONS.md).
