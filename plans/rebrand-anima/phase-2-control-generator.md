# Phase 2 — Control DTO generator & regeneration

**Status:** COMPLETED

`tools/gen-control-dto/gen.ts` is the single source of truth for the emitted C++ serde, the TS protocol
types, the OpenRPC doc, the command manifest, and the Lua component type defs. Update the emitters here
**before** the hand-written namespace pass (phase 3) so every generated artifact already carries `sa`
and a later regen never reverts phase 3/4.

## Emitter changes in `tools/gen-control-dto/gen.ts`

- The C++ serde emitters wrap output in `namespace sa` → emit `namespace sa` (so the regenerated
  `control_dto_serde.generated.cpp` and `scene_component_serde.generated.cpp` match phase 3).
  The emitted **module names stay** `Saffron.Control` / `Saffron.Scene` and `import Saffron.*`
  (Saffron is retained — do not touch these).
- The TS output target: `sa-types.ts` → `sa-types.ts` (the `tsOut = join(…, "sa-types.ts")` path).
- The Lua def emitter: `---@class sa.${name}`, `se.${t}`, `sa.Entity`, `sa.ComponentName`, and the
  `library/sa.lua` output path → `sa.` / `library/sa.lua`. (This feeds `script_component_defs.generated.hpp`.)
- The OpenRPC title `"Saffron control DTOs"` → `"Saffron Anima control DTOs"`.
- `tools/gen-control-dto/AGENTS.md`: the `sa-types.ts` reference → `sa-types.ts`.

## Regenerate + wire the rename

- Run the generator (per the editor flow, `cd editor && bun run check`, which regenerates
  `@saffron/protocol` from `control_dto.cppm` and typechecks; or invoke the gen script directly).
  This rewrites: `editor/src/protocol/sa-types.ts` (new name), `control_dto_serde.generated.cpp`,
  `scene_component_serde.generated.cpp`, `script_component_defs.generated.hpp`,
  `schemas/control/openrpc.generated.json`, and the command manifest.
- Delete the stale `editor/src/protocol/sa-types.ts` (the generator now writes `sa-types.ts`; no
  dual file — NO LEGACY).
- `editor/src/protocol/index.ts`: `from "./sa-types"` → `from "./sa-types"`.
- Keep the `@saffron/protocol` package name as-is (family brand).

## Notes

- The emitted C++ now says `namespace sa` while hand-written C++ still says `namespace sa` until
  phase 3 — that is expected and the tree will not fully build between phases 2 and 3. Treat phases
  2+3 as one buildable unit: regenerate (phase 2), then run the namespace pass (phase 3), then build.
- `schemas/control/envelope.schema.json` is hand-authored and has no brand tokens — leave it.

## Verify

After phase 3 lands (combined build), run the control-schema contract test (`make schema` /
`tools/check-control-schema`) and confirm the generated files are byte-fresh: a re-run of the
generator produces an empty `git diff`. The OpenRPC title and `sa-types.ts` are present; no
`sa-types.ts` remains.
