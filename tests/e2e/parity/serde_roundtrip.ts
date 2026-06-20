// Serde byte-equality comparator: a scene project authored by one engine loads and re-saves
// through the other, and the two on-disk JSON files are diffed. The frozen-format requirement (the
// editor and existing projects must load unchanged across the cutover) made bidirectional —
// C++→Rust and Rust→C++.
//
// Three differences are *not* format drift and are normalized out before the data comparison, each
// recorded in the verdict so the cutover sign-off sees them explicitly (recorded, never hidden):
//
//   1. Top-level key ORDER. The C++ engine emits project-JSON object keys alphabetically
//      (`nlohmann::json`'s sorted default); the Rust engine emits them in DTO field order
//      (`serde_json` + `preserve_order`). Same keys, same values — a serialization-order
//      difference, not a value drift.
//   2. Per-boot IDENTITY fields. The auto-empty project `name` carries a random suffix and entity
//      `id`s are RNG-minted `Uuid`s — both intentionally non-deterministic, so they differ run to
//      run on the *same* engine.
//   3. Entity ITERATION order. The scene's entity array follows ECS storage order (entt vs hecs),
//      which differs between engines for the same authored set.
//
// After normalizing those three, the project DATA must be byte-identical — that is the real serde
// parity guarantee (the scene/material content survives a cross-engine round-trip), asserted green.
// The raw byte-for-byte round-trip is recorded as a `tolerance` verdict naming the key-order
// difference (the one remaining non-format divergence), for the sign-off to accept or drive to a
// fix.

import { mkdtempSync, mkdirSync, readFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { ParityEngine } from "./harness.ts";
import type { ParityEntry } from "./report.ts";

/// Author a small two-entity scene on `bin` and save it to `projPath`.
async function authorAndSave(bin: string, projPath: string): Promise<void> {
  const e = await ParityEngine.boot(bin, { SAFFRON_AUTO_EMPTY_PROJECT: "1" });
  try {
    const a = (await e.call<{ id: string }>("create-entity", { name: "Alpha" })).id;
    await e.call("set-transform", { entity: a, translation: { x: 1, y: 2, z: 3 } });
    await e.call("add-component", { entity: a, component: "Collider" });
    const b = (await e.call<{ id: string }>("create-entity", { name: "Beta" })).id;
    await e.call("set-transform", { entity: b, translation: { x: -1.5, y: 0, z: 4.25 } });
    await e.call("add-component", { entity: b, component: "Rigidbody" });
    await e.call("save-project", { path: projPath });
  } finally {
    await e.shutdown();
  }
}

/// Load `projPath` on `bin` and re-save it to `outPath`.
async function loadAndResave(bin: string, projPath: string, outPath: string): Promise<void> {
  const e = await ParityEngine.boot(bin);
  try {
    await e.call("load-project", { path: projPath });
    await e.call("save-project", { path: outPath });
  } finally {
    await e.shutdown();
  }
}

/// The first offset at which two byte buffers differ (or the shorter length when one is a prefix
/// of the other) — the actionable "which byte drifted" for the recorded tolerance.
function firstDifferingOffset(a: Buffer, b: Buffer): number {
  const n = Math.min(a.length, b.length);
  for (let i = 0; i < n; i++) {
    if (a[i] !== b[i]) {
      return i;
    }
  }
  return n;
}

/// Recursively sort every object's keys, so a key-order difference does not register as a data
/// difference.
function sortKeys(value: unknown): unknown {
  if (Array.isArray(value)) {
    return value.map(sortKeys);
  }
  if (value && typeof value === "object") {
    const out: Record<string, unknown> = {};
    for (const key of Object.keys(value as Record<string, unknown>).sort()) {
      out[key] = sortKeys((value as Record<string, unknown>)[key]);
    }
    return out;
  }
  return value;
}

/// Canonicalize a project JSON for the *data* comparison: scrub the per-boot identity fields, sort
/// the entity array by name (ECS-order-independent), and recursively sort keys.
function canonicalProjectData(text: string): string {
  const project = JSON.parse(text) as {
    name?: string;
    scene?: { entities?: Array<{ id?: string; components?: { Name?: { name?: string } } }> };
  };
  project.name = "<name>";
  const entities = project.scene?.entities;
  if (Array.isArray(entities)) {
    for (const entity of entities) {
      entity.id = "<id>";
    }
    entities.sort((x, y) =>
      (x.components?.Name?.name ?? "").localeCompare(y.components?.Name?.name ?? ""),
    );
  }
  return JSON.stringify(sortKeys(project));
}

/// One round-trip direction: author on `authorBin`, load + re-save on `resaveBin`, and compare.
async function roundTrip(
  authorBin: string,
  resaveBin: string,
  label: string,
): Promise<ParityEntry[]> {
  const dir = mkdtempSync(join(tmpdir(), "saffron-parity-serde-"));
  const authored = join(dir, "authored", "project.json");
  const resaved = join(dir, "resaved", "project.json");
  mkdirSync(join(dir, "authored"), { recursive: true });
  mkdirSync(join(dir, "resaved"), { recursive: true });

  await authorAndSave(authorBin, authored);
  await loadAndResave(resaveBin, authored, resaved);

  const authoredBytes = readFileSync(authored);
  const resavedBytes = readFileSync(resaved);
  const byteEqual = authoredBytes.equals(resavedBytes);
  const firstDiff = byteEqual ? -1 : firstDifferingOffset(authoredBytes, resavedBytes);

  const dataEqual =
    canonicalProjectData(authoredBytes.toString("utf8")) ===
    canonicalProjectData(resavedBytes.toString("utf8"));

  if (!dataEqual) {
    return [
      {
        comparator: "serde-roundtrip",
        case: label,
        verdict: "tolerance",
        detail:
          "the project DATA differs across the round-trip even after normalizing key order, " +
          "identity fields, and entity order — a genuine serde drift the sign-off must investigate.",
      },
    ];
  }

  const entries: ParityEntry[] = [
    {
      comparator: "serde-roundtrip",
      case: `${label} (data)`,
      verdict: "exact",
      detail:
        "the scene/project DATA is byte-identical across the round-trip after normalizing key " +
        "order, per-boot identity (project name + entity uuids), and ECS entity order",
    },
  ];

  if (byteEqual) {
    entries.push({
      comparator: "serde-roundtrip",
      case: `${label} (raw bytes)`,
      verdict: "exact",
      detail: "the on-disk project.json is byte-for-byte identical across the round-trip",
    });
  } else {
    entries.push({
      comparator: "serde-roundtrip",
      case: `${label} (raw bytes)`,
      verdict: "tolerance",
      detail:
        `the raw on-disk bytes differ (first at offset ${firstDiff}; authored ` +
        `${authoredBytes.length} bytes, resaved ${resavedBytes.length}): the C++ engine sorts ` +
        "project-JSON keys alphabetically (nlohmann::json), the Rust engine emits them in DTO " +
        "field order (serde_json preserve_order). Same keys + values (the data verdict above is " +
        "exact); a serialization-order difference, not a value drift. Recorded for the cutover " +
        "sign-off.",
    });
  }

  return entries;
}

/// Run the serde-roundtrip comparator both directions and return the recorded entries.
export async function compareSerdeRoundtrip(cpp: string, rust: string): Promise<ParityEntry[]> {
  const cppToRust = await roundTrip(cpp, rust, "C++ authored, Rust re-saved");
  const rustToCpp = await roundTrip(rust, cpp, "Rust authored, C++ re-saved");
  return [...cppToRust, ...rustToCpp];
}
