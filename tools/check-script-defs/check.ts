#!/usr/bin/env bun
// Script-API drift tripwire. The Lua surface is bound imperatively in C++ (`.addFunction("name", ...)`
// plus a few prelude `rawset(sa, "name", ...)`), and documented for VS Code in the `SaLuaDefs` ---@meta
// string in assets.cppm. Those two must not drift: a new binding without a matching def-file entry
// leaves users without autocomplete and silently rots the docs. This fails the gate when a live name is
// absent from SaLuaDefs. It checks names only (presence), not signatures — that is enough to catch the
// drift that matters and needs no running VM. Run from repo root; wired into tools/ci/check.sh.

import { readFileSync } from "node:fs";

const repoRoot = new URL("../..", import.meta.url).pathname;
const read = (rel: string) => readFileSync(repoRoot + rel, "utf8");

const runtime = read("/engine/source/saffron/script/script_runtime.cpp");
const scriptMod = read("/engine/source/saffron/script/script.cppm");
const assets = read("/engine/source/saffron/assets/assets.cppm");
const componentDefs = read("/engine/source/saffron/assets/script_component_defs.generated.hpp");
const components = read("/engine/source/saffron/sceneedit/scene_edit_components.cpp");

// Every name bound via .addFunction("name", ...) — the first string arg, allowing a newline before it
// (raycast/spherecast wrap their name onto its own line). Metamethods (__add, __mul, …) are documented
// as ---@operator overloads, not named functions, so they are excluded.
const liveNames = new Set<string>();
for (const m of runtime.matchAll(/\.addFunction\(\s*"([a-z_]+)"/g)) liveNames.add(m[1]);
for (const m of scriptMod.matchAll(/\.addFunction\(\s*\n?\s*"([a-z_]+)"/g)) liveNames.add(m[1]);
// sa.log is added directly on the global table in newScriptVm.
for (const m of scriptMod.matchAll(/"([a-z_]+)",\s*\+\[\]\([^)]*\)\s*\{\s*logInfo/g)) liveNames.add(m[1]);
// The coroutine prelude adds wait/delay/spawn_task via rawset.
for (const m of runtime.matchAll(/rawset\(sa,\s*"([a-z_]+)"/g)) liveNames.add(m[1]);

// SaLuaDefs is a raw-string constexpr; pull its body out of assets.cppm.
const defsMatch = assets.match(/SaLuaDefs\s*=\s*\n?\s*R"\(([\s\S]*?)\)";/);
if (!defsMatch) {
  console.error("check-script-defs: could not locate the SaLuaDefs ---@meta string in assets.cppm");
  process.exit(2);
}
// library/sa.lua is SaLuaDefs (hand-written) followed by SaComponentDefs (the generated component-types
// header), so a binding is "documented" if it appears as `:name(` / `.name(` in either — get_component
// lives in the generated tail.
const defs = defsMatch[1] + componentDefs;
const documented = new Set<string>();
for (const m of defs.matchAll(/[.:]([a-z_]+)\(/g)) documented.add(m[1]);

const missing = [...liveNames].filter((n) => !n.startsWith("__") && !documented.has(n)).sort();
if (missing.length > 0) {
  console.error("check-script-defs: live Lua bindings missing from library/sa.lua (SaLuaDefs):");
  for (const n of missing) console.error("  - " + n);
  console.error("\nAdd them to the SaLuaDefs ---@meta string in assets.cppm.");
  process.exit(1);
}

// The `sa.ComponentName` alias is the typed name set for get/set/add/remove/has_component. It must cover
// every registered component (registerComponent<…>(reg, "Name", …) in scene_edit_components.cpp), or a new
// component is unspellable/untyped in scripts. The runtime resolves names by string, so a gap is silent.
const registered = new Set<string>();
for (const m of components.matchAll(/registerComponent<[^>]+>\(\s*\n?\s*reg,\s*"([A-Za-z]+)"/g)) registered.add(m[1]);
const aliasMatch = defs.match(/---@alias sa\.ComponentName\s+([^\n]+)/);
const aliased = new Set([...(aliasMatch?.[1].matchAll(/"([A-Za-z]+)"/g) ?? [])].map((m) => m[1]));
const missingComponents = [...registered].filter((n) => !aliased.has(n)).sort();
if (missingComponents.length > 0) {
  console.error("check-script-defs: registered components missing from the sa.ComponentName alias:");
  for (const n of missingComponents) console.error("  - " + n);
  console.error("\nAdd them to the ---@alias sa.ComponentName line in assets.cppm (SaLuaDefs).");
  process.exit(1);
}

console.log(
  `check-script-defs: ok (${liveNames.size} live bindings + ${registered.size} components, all documented)`,
);
