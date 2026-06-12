# Phase 15 — Clean/orphan tooling (deliberate)

**Status:** NOT STARTED
**Depends on:** 14

## Goal

Implement reachability-from-roots cleanup that classifies candidates into **unused** / **orphaned-file** /
**broken-reference** / **indirect-review**, exposes `clean-assets {dry-run, exclude[]}` (a categorized
report) and `delete-unused {confirm}` (deletes only after explicit confirm, then re-scans for cascade).
**Never automatic.** Defers: the editor review modal (16).

## Why

Three distinct problems that the user conflated earlier must be reported separately (the Unity
Broken/Missing/Unused split): an *orphaned file* (on disk, not in the catalog — phase 09 largely prevents
this now), an *unused asset* (indexed but unreachable from roots), and a *broken reference* (a scene points
at a sub-id no scan resolves). Built on the dependency graph (14), cleanup is reachability from a protected
root set — never reference-count==0 — with an explicit, reviewable, confirm-then-delete UX (UE
ProjectCleaner). The cardinal rule across all engines: **never auto-delete**.

## Classification

```cpp
enum class CleanCategory { Unused, OrphanedFile, BrokenReference, IndirectReview };
struct CleanCandidate { Uuid id; std::string path; CleanCategory category; u64 bytes; std::string reason; };
struct CleanReport { std::vector<CleanCandidate> candidates; u64 reclaimableBytes; };

// Roots = the active scene(s) + explicitly-kept ids. Walk buildDependencyGraph (14) from roots.
CleanReport analyzeClean(const Scene&, const AssetCatalog&, AssetServer&, std::span<const Uuid> exclude);
```

- **Unused:** an asset (model or standalone) unreachable from any root and not referenced indirectly.
- **OrphanedFile:** a recognized file on disk with no catalog row (a scan failure or a stray file).
- **BrokenReference:** a scene/material edge whose target sub-id resolves to nothing (diagnose; never
  auto-drop the *referrer*).
- **IndirectReview:** referenced only by something a static scan can't prove (script/path-loaded) — **always
  "review", never "unused"** (the Godot Orphan Resource Explorer cautionary tale: it misses AnimationPlayer
  clips, base-class scripts, script-referenced images).

## Commands

- `clean-assets {dry-run=true, exclude[]}` → `CleanReport`. Default is dry-run; it never deletes.
- `delete-unused {confirm=true, ids[]}` → deletes only the explicitly-listed ids (must be `Unused`), then
  re-runs the scan to surface any newly-unreferenced cascade. Refuses without `confirm`.

UX contract (mirrored in the editor, phase 16): scan → categorized list → per-item exclude (persisted) →
explicit confirm → delete → re-scan. Recommend a VCS commit before deleting (the engine can warn).

## Files to touch

- `engine/source/saffron/assets/assets.cppm` — `analyzeClean`, the categories, the reachability walk on
  the phase-14 graph, the guarded delete.
- `engine/source/saffron/control/control_dto.cppm` + `control_commands_asset.cpp` + `gen.ts` —
  `clean-assets` + `delete-unused` commands + DTOs + regen.

## Steps

1. Implement `analyzeClean`: roots from the scene + `exclude`; reachability over the dependency graph;
   classify each catalog row + each on-disk file.
2. Add the IndirectReview heuristic (any asset referenced by a `ScriptComponent` field, or path-loaded) →
   never Unused.
3. Implement `delete-unused` with the `confirm` guard + post-delete rescan for cascade.
4. Commands + DTOs + `gen.ts` + fixtures.
5. e2e: build a scene that uses model A, import an unused model B → `clean-assets` flags B as Unused, A as
   kept; delete a chunk's extracted file → flagged BrokenReference; a script-referenced texture → flagged
   IndirectReview (not Unused); `delete-unused` removes B only after confirm and the catalog reflects it.

## Gate / done

- `make engine` clean; the cleanup e2e proves correct classification (Unused vs kept vs Broken vs Indirect)
  and confirm-gated deletion; `make e2e` + contract test pass; `make prepare-for-commit` clean.
- No code path auto-deletes (the report is dry-run by default; delete requires `confirm`).

## Risks

- **False-positive deletion of script-referenced assets:** the dominant risk. Lua `ScriptComponent` refs are
  invisible to a static walk; the IndirectReview category + never-auto-delete + explicit confirm are the
  three guards. The e2e must include a script-referenced asset.
- **Root-set completeness:** if a valid root (a scene not currently open, a build manifest) is omitted,
  legitimate assets look unused. Make the root set explicit and conservative; default to "only the active
  scene is a root, everything else is review."
- **Cascade surprises:** deleting a model can orphan a previously-shared texture; the post-delete rescan
  must re-present the cascade rather than delete it implicitly.
- **Destructive command safety:** `delete-unused` is outward-facing and irreversible; require `confirm`,
  log every deletion, and surface the VCS-commit recommendation.
