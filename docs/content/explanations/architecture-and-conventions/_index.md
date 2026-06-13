+++
title = 'Architecture & conventions'
weight = 19
bookCollapseSection = true
+++

# Architecture & conventions

The engine's architecture and conventions are the structure and rules that hold the codebase
together. They cover the C++26 named-module DAG, the toolbox build, the shader pipeline, and
the Go-flavored style the whole codebase follows.

## Pages

| Page | Covers | Code |
|---|---|---|
| `go-flavored-cpp` | data structs + free functions, struct-of-closures itables, errors as values | `CONVENTIONS.md` |
| `cxx26-modules` | named modules, `import std`, the libc++ std-module gate and its gotchas | `CMakeLists.txt`; `AGENTS.md` |
| `module-partitions` | interface partition + `.cpp` impl units, the BMI ICE workaround | `engine/CMakeLists.txt` |
| `module-dag` | the dependency graph, the `:RenderGraph` partition, `Saffron.Host` | `AGENTS.md` |
| `build-environment` | Fedora Silverblue + the `saffron-build` toolbox, `-j1`, never `rm -rf build` | `AGENTS.md` |
| `shader-compilation` | Slang → SPIR-V compiled in CMake | `cmake/`; `engine/assets/shaders/` |
| `dependencies` | vendored deps via FetchContent, system SDL3/Vulkan | `cmake/Dependencies.cmake` |
