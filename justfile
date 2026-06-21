# Saffron Anima task runner — the Rust front door.
#
# Drives the build/test/run flow through cargo, the xtask helper, and bun. The toolbox-bound
# recipes AUTO-ENTER the `saffron-build` container when run from the host (so `just lint`
# works the same from a host shell or inside the toolbox); recipes that don't need the
# toolchain (help/run-docs) run on the host directly.
#
# Set `SAFFRON_NO_TOOLBOX=true` to skip that auto-enter and run a recipe directly on the host
# (`SAFFRON_NO_TOOLBOX=true just <recipe>`) — this trusts the host to provide cargo plus the
# Vulkan/SDL/Slang toolchain itself.
#
# Env vars are set INSIDE each recipe, never as a host-side prefix — a host-side
# `ENV=… just …` would no-op across the `toolbox run` boundary. Each toolbox recipe is a
# single bash script that re-execs `just` inside the container when /run/.toolboxenv is
# absent (host invocation) and runs directly otherwise.

set shell := ["bash", "-uc"]

# Repo root (this justfile's dir) and the Cargo workspace under it.
repo := justfile_directory()
engine := repo / "engine"
editor := repo / "editor"
docs := repo / "docs"

# The toolbox + host bun (added to PATH on re-entry).
toolbox := "saffron-build"
bun_bin := "/var/home/saffronjam/.bun/bin"

# The present-only host binary produced by `cargo build` (saffron-host crate). The editor + e2e
# spawn whatever SAFFRON_ANIMA_BIN resolves to; the recipes below point it here.
engine_bin := engine / "target/debug/saffron-host"

# Prelude prepended to every toolbox-bound recipe: if not already inside the toolbox,
# re-exec the same recipe in the saffron-build container, then exit. Either way — re-exec
# from the host, or already inside the toolbox — it puts host bun on PATH, so bun recipes
# work both via `just <recipe>` on the host AND `toolbox run … bash -lc 'just <recipe>'`
# (the latter enters the toolbox first, so the re-exec branch is skipped).
reenter := '''
    if [ ! -f /run/.toolboxenv ] && [ -z "${SAFFRON_NO_TOOLBOX:-}" ]; then
      command -v toolbox >/dev/null || { echo "toolbox not found — install it, or run inside the saffron-build container" >&2; exit 1; }
      exec toolbox run -c saffron-build bash -lc 'export PATH="''' + bun_bin + ''':$PATH"; exec just --justfile "''' + justfile() + '''" "$@"' _ "$RECIPE" "$@"
    fi
    export PATH="''' + bun_bin + ''':$PATH"
'''

# Resolve the NVIDIA Vulkan ICD manifest (host copy under the toolbox, local on a bare host,
# empty when neither exists). VK_ADD_DRIVER_FILES *adds* it to the loader search so llvmpipe
# stays a fallback; device selection prefers the discrete GPU when it can present and falls
# back to software (e.g. headless) when it can't.
nvidia_icd := '''
    NVIDIA_ICD="$(ls /run/host/usr/share/vulkan/icd.d/nvidia_icd.x86_64.json /usr/share/vulkan/icd.d/nvidia_icd.x86_64.json 2>/dev/null | head -n1 || true)"
    [ -n "$NVIDIA_ICD" ] && export VK_ADD_DRIVER_FILES="$NVIDIA_ICD"
'''

# Resolve a default content project for the editor-less `run-engine` so the viewport shows a
# scene rather than an empty project. Points the engine at
# the repo-root appdata (the editor's project store) and picks the most-recently-opened project
# that has mesh entities; if none qualifies it leaves SAFFRON_PROJECT unset and the engine's own
# resolution applies. A SAFFRON_PROJECT already in the environment always wins (manual override).
default_project := '''
    export SAFFRON_APPDATA_DIR="''' + repo + '''/appdata"
    if [ -z "${SAFFRON_PROJECT:-}" ]; then
      SAFFRON_PROJECT="$(python3 - "$SAFFRON_APPDATA_DIR" <<'PY'
import json, os, sys, glob
appdata = sys.argv[1]
def has_meshes(pj):
    try:
        with open(pj) as f: doc = json.load(f)
    except Exception: return False
    ents = doc.get("scene", {}).get("entities", [])
    return any("Mesh" in e.get("components", {}) for e in ents)
# Prefer the most-recently-opened recent project that has meshes.
recents = os.path.join(appdata, "recent-projects.json")
ordered = []
try:
    with open(recents) as f: ordered = [p.get("path") for p in json.load(f).get("projects", [])]
except Exception: ordered = []
# Then any remaining content project under userdata, newest first.
extra = sorted(glob.glob(os.path.join(appdata, "userdata", "*", "project.json")),
               key=lambda p: os.path.getmtime(p), reverse=True)
for path in [p for p in ordered if p] + extra:
    if path and os.path.exists(path) and has_meshes(path):
        print(path); break
PY
)"
      [ -n "$SAFFRON_PROJECT" ] && export SAFFRON_PROJECT && echo "run-engine: default project $SAFFRON_PROJECT"
    fi
'''

[private]
default:
    @just --list

# list the available recipes (host; no toolbox)
help:
    @just --list

# init the theme submodule and serve the docs site (host hugo + git)
run-docs:
    git -C "{{repo}}" submodule update --init --depth 1 docs/themes/hugo-book
    cd "{{docs}}" && hugo server

# fetch on-demand external sources (pinned + checksummed Jolt) into the gitignored cache
fetch-deps:
    #!/usr/bin/env bash
    set -euo pipefail
    RECIPE=fetch-deps; {{reenter}}
    cd "{{engine}}"
    # build.rs owns the pinned+checksummed fetch; touch it so cargo re-runs the script even when
    # the crate is otherwise up to date (e.g. right after `just clean-deps`). On a fresh clone the
    # script runs anyway; either way this populates vendor/ without compiling the rest.
    touch crates/physics-sys/build.rs
    cargo build -p saffron-physics-sys --quiet

# remove the on-demand source cache so the next build re-fetches + re-verifies it
clean-deps:
    #!/usr/bin/env bash
    set -euo pipefail
    RECIPE=clean-deps; {{reenter}}
    rm -rf "{{engine}}/crates/physics-sys/vendor"

# build the Rust workspace + compile shaders next to the host binary
engine:
    #!/usr/bin/env bash
    set -euo pipefail
    RECIPE=engine; {{reenter}}
    cd "{{engine}}"
    cargo build --workspace
    cargo run -p xtask -- shaders

# gen @saffron/protocol + tsc + vite build of the frontend
editor:
    #!/usr/bin/env bash
    set -euo pipefail
    RECIPE=editor; {{reenter}}
    cd "{{editor}}" && bun run build

# control-schema contract test (live `sa` control output vs schemas/control)
schema:
    #!/usr/bin/env bash
    set -euo pipefail
    RECIPE=schema; {{reenter}}
    cd "{{repo}}/tools/check-control-schema" && bun run check.ts

# end-to-end tests driving a headless engine over the control plane (bun test)
e2e:
    #!/usr/bin/env bash
    set -euo pipefail
    RECIPE=e2e; {{reenter}}
    cd "{{repo}}/tests/e2e" && bun test

# run the Rust workspace unit + integration tests
test:
    #!/usr/bin/env bash
    set -euo pipefail
    RECIPE=test; {{reenter}}
    cd "{{engine}}" && cargo test --workspace

# start the Tauri editor (it spawns the engine host as a native child)
run:
    #!/usr/bin/env bash
    set -euo pipefail
    RECIPE=run; {{reenter}}
    cd "{{engine}}"
    cargo build --bin saffron-host
    cargo run -p xtask -- shaders
    {{nvidia_icd}}
    export SAFFRON_WEBVIEW_HW=1
    export SAFFRON_ANIMA_BIN="{{engine_bin}}"
    cd "{{editor}}" && bun run tauri dev

# like `run`, but with the editor's developer mode pre-enabled
run-debug:
    #!/usr/bin/env bash
    set -euo pipefail
    RECIPE=run-debug; {{reenter}}
    cd "{{engine}}"
    cargo build --bin saffron-host
    cargo run -p xtask -- shaders
    {{nvidia_icd}}
    export SAFFRON_WEBVIEW_HW=1 VITE_SAFFRON_DEV_MODE=1
    export SAFFRON_ANIMA_BIN="{{engine_bin}}"
    cd "{{editor}}" && bun run tauri dev

# run the editor on the llvmpipe software GPU (control case; no NVIDIA ICD)
run-software:
    #!/usr/bin/env bash
    set -euo pipefail
    RECIPE=run-software; {{reenter}}
    cd "{{engine}}"
    cargo build --bin saffron-host
    cargo run -p xtask -- shaders
    export SAFFRON_ANIMA_BIN="{{engine_bin}}"
    cd "{{editor}}" && bun run tauri dev

# start only the present-only host (loads a default content project so it shows a scene)
run-engine:
    #!/usr/bin/env bash
    set -euo pipefail
    RECIPE=run-engine; {{reenter}}
    cd "{{engine}}"
    cargo build --bin saffron-host
    cargo run -p xtask -- shaders
    {{nvidia_icd}}
    {{default_project}}
    exec "{{engine_bin}}"

# the present-only host forced onto llvmpipe (no NVIDIA ICD)
run-engine-software:
    #!/usr/bin/env bash
    set -euo pipefail
    RECIPE=run-engine-software; {{reenter}}
    cd "{{engine}}"
    cargo build --bin saffron-host
    cargo run -p xtask -- shaders
    {{default_project}}
    exec "{{engine_bin}}"

# boot the host headless for a bounded number of frames (the native-viewport driver the editor
# spawns: renders offscreen, no window or compositor needed)
run-engine-headless frames="5":
    #!/usr/bin/env bash
    set -euo pipefail
    RECIPE=run-engine-headless; {{reenter}}
    cd "{{engine}}"
    cargo build --bin saffron-host
    cargo run -p xtask -- shaders
    export SAFFRON_EDITOR_NATIVE_VIEWPORT=1
    SAFFRON_EXIT_AFTER_FRAMES={{frames}} SAFFRON_CONTROL_SOCK="/tmp/sa-just-$$.sock" "{{engine_bin}}"

# the host-runnable control CLI; `just sa ping`, `just sa help` (no engine dep)
sa *args:
    #!/usr/bin/env bash
    set -euo pipefail
    RECIPE=sa; {{reenter}}
    cd "{{engine}}" && cargo run --bin sa -- {{args}}

# cargo fmt the Rust workspace + oxfmt the editor TypeScript
format:
    #!/usr/bin/env bash
    set -euo pipefail
    RECIPE=format; {{reenter}}
    cd "{{engine}}" && cargo fmt
    cd "{{editor}}" && bun run format

# cargo fmt --check + clippy (deny warnings) on the workspace + oxlint the editor
lint:
    #!/usr/bin/env bash
    set -euo pipefail
    RECIPE=lint; {{reenter}}
    cd "{{engine}}" && cargo fmt --check
    cd "{{engine}}" && cargo clippy --workspace -- -D warnings
    cd "{{editor}}" && bun run lint

# format everything, then lint
prepare-for-commit:
    #!/usr/bin/env bash
    set -euo pipefail
    RECIPE=prepare-for-commit; {{reenter}}
    just --justfile "{{justfile()}}" format
    just --justfile "{{justfile()}}" lint

# the full reproducible gate (engine build + shaders, smoke, schema, frontend)
check:
    #!/usr/bin/env bash
    set -euo pipefail
    RECIPE=check; {{reenter}}
    "{{repo}}/tools/ci/check.sh"
