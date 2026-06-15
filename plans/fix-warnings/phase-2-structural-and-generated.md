# Phase 2 — structural fixes + the generated emitters

**Status:** NOT STARTED

Checks that change linkage, types, or signatures (so they need a glance, not blind `--fix`), plus the
warnings that live in **generated** files and must be fixed in the `gen.ts` emitters. ~205 hand-side
sites + the generator work.

## Structural checks

| Check | Distinct | Fix |
|---|---:|---|
| `misc-use-internal-linkage` | 127 | give a file-local free function `static` (or move it into an anonymous namespace) |
| `performance-enum-size` | 38 | add an explicit `: std::uint8_t` (or smallest fitting) base to small enums |
| `modernize-avoid-c-arrays` | 15 | `T arr[N]` → `std::array<T, N>` |
| `misc-use-anonymous-namespace` | 11 | prefer an anonymous namespace over `static` where the file already uses one |
| `performance-unnecessary-value-param` | 8 | pass-by-`const&` for non-trivial params taken by value and not moved |
| `performance-no-automatic-move` | 3 | drop `const` on a local that is then returned, so NRVO/move applies |
| `performance-inefficient-vector-operation` | 1 | `reserve()` before a known-size `push_back` loop |

Notes:
- **`misc-use-internal-linkage` vs the module boundary.** Many engine free functions are module-internal
  already; the check wants them `static`/anon-namespace so they don't get external linkage. Confirm the
  symbol is not part of the module's exported surface before making it `static` — exported declarations
  (in the `export` block / interface partition) are not candidates.
- **`performance-enum-size`** pairs with `CONVENTIONS.md` (PascalCase enum values, unaffected); only the
  base type changes. Check no code relies on the enum's `int` width (serialization, bit-packing).
- **`misc-use-anonymous-namespace` vs `misc-use-internal-linkage`** can both fire on the same symbol —
  pick one form per file and stay consistent.

Procedure mirrors phase 1 (one check, per file, rebuild, format, re-lint, commit per module), but **read
each fix** — these touch linkage and signatures, where an auto-fix can change an overload set or an ABI
detail. Run `make e2e` after the linkage changes.

## Generated files

Warnings in generated outputs are **not** edited in place — `make format` and the contract gate treat
them as owned by the generator. Fix them in the emitter and regenerate.

| Generated file | Warnings | Emitter (`tools/gen-control-dto/gen.ts`) |
|---|---:|---|
| `control/control_dto_serde.generated.cpp` | 161 | `emitCpp` |
| `scene/scene_component_serde.generated.cpp` | 4 | `emitSceneSerde` |

The third generated output, `assets/script_component_defs.generated.hpp` (`emitScriptComponentDefs`),
already lints clean — use it as the reference for what the others should produce.

Steps:
1. Identify the warning categories in each generated file (mostly the phase-1 set:
   `misc-const-correctness`, `modernize-use-designated-initializers`, possibly `modernize-use-auto`).
2. Update the **emitter templates** in `gen.ts` so the emitted C++ is already compliant — e.g. emit
   `const` on locals, designated initializers for the DTO structs, `auto` for call-result locals.
3. `bun run tools/gen-control-dto/gen.ts`, then `make engine`.
4. The `git diff --exit-code` freshness gate in `tools/ci/check.sh` proves the regenerated files match;
   re-lint to confirm they reach zero warnings.

Because `emitSceneSerde` is a hand-written template literal (not derived from the DTOs — see
`tools/gen-control-dto/AGENTS.md`), its four warnings are edited directly in that template string.
