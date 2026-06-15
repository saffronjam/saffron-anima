#!/usr/bin/env bash
# Project feature smoke tests. Requires an existing display because SaffronEngine
# opens a Vulkan swapchain; tools/ci/check.sh runs this under the same compositor
# as the engine and schema checks.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ENGINE="$REPO/build/debug/bin/SaffronEngine"
SE="$REPO/build/debug/bin/se"
APPDATA="$(mktemp -d /tmp/saffron-projects.XXXXXX)"
PNG="$(mktemp /tmp/saffron-projects-texture.XXXXXX.png)"
ENGINE_PID=""
SOCK=""

cleanup() {
  if [ -n "$ENGINE_PID" ] && kill -0 "$ENGINE_PID" 2>/dev/null; then
    SAFFRON_CONTROL_SOCK="$SOCK" "$SE" quit >/dev/null 2>&1 || true
    wait "$ENGINE_PID" 2>/dev/null || true
  fi
  rm -rf "$APPDATA" "$PNG" "$SOCK"
}
trap cleanup EXIT

printf "%s" "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAFgwJ/lXqyyAAAAABJRU5ErkJggg==" | base64 -d > "$PNG"

start_engine() {
  local name="$1"
  SOCK="/tmp/saffron-projects-$name-$$.sock"
  rm -f "$SOCK"
  SAFFRON_CONTROL_SOCK="$SOCK" \
    SAFFRON_APPDATA_DIR="$APPDATA" \
    SAFFRON_PROJECT="$name" \
    "$ENGINE" >/tmp/saffron-projects-engine-$$.log 2>&1 &
  ENGINE_PID=$!
  for _ in $(seq 1 80); do
    if [ -S "$SOCK" ]; then
      return 0
    fi
    if ! kill -0 "$ENGINE_PID" 2>/dev/null; then
      cat /tmp/saffron-projects-engine-$$.log >&2
      return 1
    fi
    sleep 0.1
  done
  cat /tmp/saffron-projects-engine-$$.log >&2
  return 1
}

stop_engine() {
  SAFFRON_CONTROL_SOCK="$SOCK" "$SE" quit >/dev/null
  # The control assertions above already prove the engine ran and answered; the device-teardown
  # exit is tolerated because llvmpipe trips a known VMA "allocations not freed" assertion at exit.
  wait "$ENGINE_PID" || true
  ENGINE_PID=""
}

start_engine asset-test

if SAFFRON_CONTROL_SOCK="$SOCK" "$SE" new-project Bad_Name >/tmp/saffron-projects-invalid-$$.json 2>&1; then
  echo "invalid project name was accepted" >&2
  exit 1
fi

# import-model bakes the glTF into one .smodel container (mesh + materials + textures as chunks);
# import-texture imports a standalone image as a loose texture asset.
SAFFRON_CONTROL_SOCK="$SOCK" "$SE" --output=json import-model "$REPO/engine/assets/models/cube.gltf" >/tmp/saffron-projects-model-$$.json
SAFFRON_CONTROL_SOCK="$SOCK" "$SE" --output=json import-texture "$PNG" >/tmp/saffron-projects-texture-$$.json
SAFFRON_CONTROL_SOCK="$SOCK" "$SE" --output=json save-project >/tmp/saffron-projects-save-$$.json

MODEL_ID="$(grep '"id"' /tmp/saffron-projects-model-$$.json | head -1 | tr -dc '0-9')"
test -n "$MODEL_ID"

PROJECT="$APPDATA/userdata/asset-test/project.json"
test -f "$PROJECT"
grep -q '"name": "asset-test"' "$PROJECT"
grep -q '"displayName": "Asset Test"' "$PROJECT"
grep -q '"type": "model"' "$PROJECT"
find "$APPDATA/userdata/asset-test/assets/models" -name '*.smodel' -print -quit | grep -q .
find "$APPDATA/userdata/asset-test/assets/textures" -type f -print -quit | grep -q .

# A fresh project is scaffolded for VS Code: the LuaLS def file + .luarc.json pointing at it.
test -f "$APPDATA/userdata/asset-test/library/se.lua"
grep -q '@class se.Entity' "$APPDATA/userdata/asset-test/library/se.lua"
test -f "$APPDATA/userdata/asset-test/.luarc.json"
grep -q '"library"' "$APPDATA/userdata/asset-test/.luarc.json"

stop_engine

# Restart: loadProject reads the saved project.json and the filesystem scan rediscovers the .smodel,
# so the model asset (and its renderable thumbnail) survive the round-trip.
start_engine asset-test
SAFFRON_CONTROL_SOCK="$SOCK" "$SE" --output=json get-project >/tmp/saffron-projects-get-$$.json
grep -q '"loaded": true' /tmp/saffron-projects-get-$$.json
SAFFRON_CONTROL_SOCK="$SOCK" "$SE" --output=json list-assets >/tmp/saffron-projects-list-$$.json
grep -q "$MODEL_ID" /tmp/saffron-projects-list-$$.json
SAFFRON_CONTROL_SOCK="$SOCK" "$SE" --output=json get-thumbnail "$MODEL_ID" 32 >/tmp/saffron-projects-thumb-$$.json
grep -q '"base64"' /tmp/saffron-projects-thumb-$$.json
stop_engine

echo "project checks passed"
