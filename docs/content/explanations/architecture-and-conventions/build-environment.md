+++
title = 'Build environment'
weight = 5
+++

# Build environment

The build environment is a single container that holds the entire C++ toolchain. The host runs no
compiler, so all building, testing, and running happens inside that container.

A toolbox is a Fedora development container with the home directory shared host-side. It isolates
the toolchain from an immutable host while leaving project files editable from either side.

## The toolbox

The dev machine is Fedora **Silverblue 43**, ostree-booted, with home under `/var/home`. It ships
no `g++`/`cmake`/clang/Vulkan SDK on the host. Everything builds inside the **`saffron-build`**
container, created from `fedora-toolbox:43`. The home directory is shared host-to-toolbox, so files
edited on the host are visible inside the container immediately.

Every build, test, or run command goes through the toolbox:

```sh
toolbox run -c saffron-build bash -lc '
  cd /var/home/saffronjam/repos/SaffronEngine
  cmake --preset debug            # first time / after CMake changes
  cmake --build build/debug -j1   # -j1 on purpose, see below
  ./build/debug/bin/SaffronEngine
'
```

The container carries the full toolchain: Clang/Clang++ **21.1.8** with **libc++ 21**, which ships
the `std` module that `import std` needs, CMake **3.31.11**, Ninja, lld, and Vulkan
headers/loader/validation-layers/tools **1.4.341**, plus SDL3-devel **3.4.8**. The prebuilt Slang
compiler lives under `~/.cache/saffron-slang/`. The `debug`/`release` presets pin `clang++`,
`-stdlib=libc++`, and `-fuse-ld=lld` with Ninja.

The GPU inside the toolbox is **llvmpipe**, Mesa's software Vulkan, which is sufficient for
correctness and validation. Hardware acceleration needs `mesa-vulkan-drivers` installed in the
container.

For headless or automated verification, bound the run so it exits on its own:

```sh
SAFFRON_EXIT_AFTER_FRAMES=5 ./build/debug/bin/SaffronEngine
```

## Why -j1

Parallel builds in the toolbox intermittently `SIGBUS` on the Clang 21 + libc++ `import std`
BMI-serialization ICE described in [module partitions](../module-partitions/). The fault lands on a
random module TU and is non-deterministic. `-j1` serializes the module builds and is reliable.

> [!WARNING]
> Build with `-j1`. Parallel module builds in the toolbox intermittently `SIGBUS` on a Clang +
> libc++ `import std` ICE. `-j1` is reliable.

> [!WARNING]
> Never `rm -rf build/debug`. The build tree holds runtime-imported assets (baked `.smesh` and
> textures under `bin/assets/`) that are **not** in git. Wiping it loses them. Reconfigure in
> place instead.

> [!WARNING]
> Do not `set(CMAKE_CXX_EXTENSIONS OFF)`. The std module builds as `gnu++26`; a `c++26` consumer
> rejects its BMI. See [C++26 modules](../cxx26-modules/).

## In the code

| What | File | Symbols |
|---|---|---|
| Toolbox + build recipe | `AGENTS.md` | `toolbox run -c saffron-build`, `-j1`, exit-after-frames |
| Compiler + linker pins | `CMakePresets.json` | `clang-libcxx`, `-stdlib=libc++`, `-fuse-ld=lld`, Ninja |
| Import-std gate | `CMakeLists.txt` | `CMAKE_EXPERIMENTAL_CXX_IMPORT_STD`, extensions left on |
| Shader toolchain | `cmake/Slang.cmake` | `slangc` from PATH/cache or fetched prebuilt |

## Related
- [Module partitions](../module-partitions/) — the ICE that `-j1` works around
- [C++26 modules](../cxx26-modules/) — the libc++ std module the toolbox provides
- [Shader compilation](../shader-compilation/) — where `slangc` fits in the build
