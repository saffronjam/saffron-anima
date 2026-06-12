# Phase 02 — MetadataChunk schema + prefix-read

**Status:** COMPLETED
**Depends on:** 01

## Goal

Define the JSON `MetadataChunk` schema — the header-first record a catalog scan and a deterministic
reimport need — and a cheap `readContainerMetadata(path)` that does the **prefix read** (the 64-B header
+ the metadata chunk only) without touching any payload bytes. The metadata carries: the model id/name,
the import recipe (source path + hash + options), the flat `subAssets` array, the `materials` array, the
`nodes` hierarchy, an optional `skin`, and the `remap` table. Defers: writing real metadata at bake (05),
building catalog rows from it (03/09), extraction/remap behavior (12).

## Why

The whole "scan the filesystem cheaply" property (phase 09) and the "skip reimport if unchanged" property
(phase 13) depend on the metadata being parseable from a small prefix, never the full file — the UE Asset
Registry model (catalog-searchable tags baked at a fixed offset, read by seek-and-read). It also makes the
container self-describing, which is what lets `.smodel` **supersede the `editor-view` `.srig` sidecar**:
`nodes` + `skin` here is exactly the rig payload, embedded.

## The MetadataChunk schema (JSON, `ChunkKind::Meta`)

```jsonc
{
  "schema": 1,
  "model":  { "id": "<decimal-uuid>", "name": "town", "sourceFormat": "gltf" },

  "import": {                              // the deterministic reimport recipe (UE UInterchangeAssetImportData)
    "sourcePath": "raw/town.glb",          // project-relative
    "sourceHash": "<xxh3/blake3 of source bytes>",   // content hash, NOT mtime
    "importerVersion": 1,
    "options": { "scale": 1.0, "axis": "y-up", "genTangents": true, "embedTextures": true }
  },

  "subAssets": [                           // the catalog rows this container contributes (flat)
    { "subId": "<uuid>", "type": "mesh",      "name": "town_mesh", "chunk": 0 },
    { "subId": "<uuid>", "type": "texture",   "name": "town_albedo", "chunk": 3, "colorspace": "srgb" },
    { "subId": "<uuid>", "type": "material",  "name": "stone", "chunk": 5 },
    { "subId": "<uuid>", "type": "animation", "name": "walk", "chunk": 7, "duration": 1.2 }
  ],

  "materials": [                           // index → material subId + factor summary (for prefix display)
    { "subId": "<uuid>", "baseColor": [1,1,1,1], "metallic": 0.0, "roughness": 1.0 }
  ],

  "nodes": [                               // glTF nodes-block shape; index-referenced
    { "name": "root", "parent": -1, "t": [0,0,0], "r": [1,0,0,0], "s": [1,1,1],
      "mesh": 0, "materials": [0,1] }      // r is quaternion (w,x,y,z); mesh/material are indices
  ],

  "skin": {                                // optional (skinned models only)
    "joints": [/* node indices */], "inverseBind": [/* 16-float matrices */],
    "skeletonRoot": 0, "meshNode": 1
  },

  "remap": {                               // extraction overrides (phase 12); empty at bake
    "<subId>": { "external": "materials/<uuid>.smat" }
  }
}
```

Conventions reused from the scene/material serde: uuids are **decimal strings**, `"0"` = none, quaternion
order is `(w,x,y,z)` (match `MaterialAsset` and the scene serde so the contract test stays consistent).
The chunk is `nlohmann::json` serialized to bytes (the module already links it).

## The prefix-read API

```cpp
struct ContainerMetadata {           // the parsed MetadataChunk
    Uuid modelId; std::string name, sourceFormat;
    struct Import { std::string sourcePath, sourceHash; u32 importerVersion; nlohmann::json options; } import;
    struct SubAsset { Uuid subId; AssetType type; std::string name; u32 chunk; std::string colorspace; f32 duration; };
    std::vector<SubAsset> subAssets;
    nlohmann::json materials, nodes, skin, remap;   // kept as json for the hierarchy/skin (parsed on instantiate)
};

// Cheap: reads only [0, 64) then [metaOffset, metaOffset+metaLength); parses the META JSON. No payloads.
std::expected<ContainerMetadata, std::string>
readContainerMetadata(const std::filesystem::path& path);

// Build the META bytes from a populated ContainerMetadata (used by the writer in phase 05).
std::vector<std::byte> encodeContainerMetadata(const ContainerMetadata&);
```

`readContainerMetadata` calls `readContainerHeader` (phase 01) for `metaOffset`/`metaLength`, reads exactly
that range, and parses it. The `nodes`/`skin`/`materials`/`remap` blobs stay as `json` here; phase 07
parses `nodes`/`skin` into the spawn structures on demand.

## Files to touch

- `engine/source/saffron/geometry/geometry.cppm` — `ContainerMetadata`, `readContainerMetadata`,
  `encodeContainerMetadata`. (Geometry already avoids `import std`; use the global-fragment `#include`
  for `nlohmann/json` as the material/scene serde does, or keep encode/decode in `Saffron.Assets` if the
  json include belongs there — see note below.)
- `engine/source/saffron/assets/assets.cppm` — if json lives more naturally here, host
  `encode/decodeContainerMetadata` in `Saffron.Assets` and keep `geometry.cppm` to the raw byte framing.

> Module note: `Saffron.Json` wraps nlohmann; `Saffron.Geometry` already parses glTF JSON, so json is
> available there. Pick one home and keep the prefix-read function callable from `Saffron.Assets`.

## Steps

1. Define `ContainerMetadata` + the `SubAsset` sub-struct (reuse `AssetType` from `scene.cppm:331` once
   phase 03 adds `Model`; until then map type strings directly).
2. Implement `encodeContainerMetadata` (struct → json → bytes) and the inverse parse, with the
   decimal-string-uuid + `(w,x,y,z)` conventions.
3. Implement `readContainerMetadata` on top of `readContainerHeader`; validate `metaLength` against the
   file and that `metaOffset` is inside the file.
4. Self-test: hand-build a `ContainerMetadata`, encode it as a META chunk, `writeContainer` it (phase 01),
   `readContainerMetadata` it back, assert field-exact equality (ids, subAssets, nodes count, skin
   presence).

## Gate / done

- `make engine` clean; self-test round-trips a metadata chunk field-exact and rejects a truncated META;
  `make prepare-for-commit` clean.
- No payload bytes are read by `readContainerMetadata` (assert via a chunk large enough that reading it
  would be observable, or by construction).

## Risks

- **Schema churn vs `schemaVersion`:** the schema will grow (phase 05 fills materials, 12 fills remap).
  Gate on `schema`/`schemaVersion` and keep parsing forward-compatible (ignore unknown keys) so a v1
  reader doesn't choke on a later field.
- **Determinism for hashing (phase 13):** the encoded JSON must be stable (sorted keys / fixed float
  formatting) or `sourceHash`-adjacent comparisons and the contract test churn. Decide key ordering here.
- **Keeping `nodes`/`skin` as `json`:** convenient now, but phase 07 must parse them robustly; document
  the exact shape so the spawn code and the writer agree.
