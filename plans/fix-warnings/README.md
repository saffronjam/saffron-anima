# clang-tidy warning cleanup — bring the tree to full compliance

**Status:** NOT STARTED

`make lint` runs `run-clang-tidy` over the whole `engine/source` tree against `.clang-tidy`, and the tree
does not pass: there are **1037 distinct** warnings. The goal of this plan is to fix them **in the code**
so `make lint` is reliably green — no disabling checks, no blanket `// NOLINT`, no per-file opt-outs. The
codebase should satisfy the linter it ships, as best practice.

## What the numbers actually are

A full run prints a frightening tally; almost all of it is duplication, not distinct problems:

| Number | Meaning |
|---|---|
| **294911** | clang-tidy's raw internal count — every warning across all ~50 translation units *before* filtering, dominated by GLM / EnTT / nlohmann template headers re-instantiated in every TU |
| **11462** | what prints after `HeaderFilterRegex: '(engine/source\|tools/se)'` drops third-party headers |
| **1037** | **distinct** warnings (unique file + line + message) in our own code — the real work list |

Two multipliers: **C++20 modules** (every TU that `import`s a module re-analyzes its interface, so one
warning in a widely-imported `.cppm` is re-counted by each consumer — ~10× on average) and **GLM**
(template instantiations in nearly every TU inflate the pre-filter counter). Neither reflects distinct
defects.

Every distinct warning is a **style / modernization / minor-performance** suggestion. The sweep found
**no correctness bugs** — the handful of `bugprone-*` hits are narrow and reviewed by hand in phase 3.

## The exit code is a separate, tooling issue

`.clang-tidy` has `WarningsAsErrors: ''`, so warnings never fail the run. The non-zero exit comes from a
`run-clang-tidy` message — `Fix conflicts with existing fix! The new replacement overlaps …
glm/detail/type_vec4.hpp` — two TUs proposing overlapping auto-fixes to the same shared GLM header. It is
a tooling artifact of analyzing third-party headers many times, not a defect in our code. Making the gate
deterministic is part of phase 3.

> The gate is also **flaky by build state**: clang-tidy on C++20 modules needs fresh BMIs. A stale or
> partial `build/debug` makes it silently skip module TUs and appear clean; a full `make engine`
> immediately before linting is what makes it analyze everything. Always lint on a freshly built tree.

## The work list — distinct counts per check

| Check | Distinct | Nature | Phase |
|---|---:|---|:--:|
| `misc-const-correctness` | 295 | declare unmutated locals `const` | 1 |
| `modernize-use-designated-initializers` | 240 | `T{a, b}` → `T{.x = a, .y = b}` (house style) | 1 |
| `performance-move-const-arg` | 158 | useless / pessimizing `std::move` | 1 |
| `misc-use-internal-linkage` | 127 | file-local free functions → `static` / anon namespace | 2 |
| `modernize-use-auto` | 44 | `auto` for call-result locals (house style) | 1 |
| `performance-enum-size` | 38 | give enums a `std::uint8_t` base | 2 |
| `modernize-avoid-c-arrays` | 15 | `T[]` → `std::array` | 2 |
| `modernize-use-scoped-lock` | 12 | `std::lock_guard` → `std::scoped_lock` | 1 |
| `modernize-return-braced-init-list` | 12 | `return T{…}` cleanup | 1 |
| `misc-use-anonymous-namespace` | 11 | `static` → anon namespace where preferred | 2 |
| `modernize-use-integer-sign-comparison` | 9 | `std::cmp_*` for mixed-sign compares | 1 |
| `performance-unnecessary-value-param` | 8 | pass-by-`const&` | 2 |
| `modernize-use-ranges` | 7 | `std::ranges::` algorithms | 1 |
| `modernize-use-emplace` | 5 | `push_back` → `emplace_back` | 1 |
| `bugprone-misplaced-widening-cast` | 4 | **review** — widening after a narrow op | 3 |
| `performance-no-automatic-move` | 3 | `const` local blocks the return move | 2 |
| `bugprone-exception-escape` | 3 | **review** — exception out of `noexcept`-ish path | 3 |
| `misc-unused-parameters` | 2 | drop / `[[maybe_unused]]` | 3 |
| `bugprone-narrowing-conversions` | 2 | **review** — implicit narrowing | 3 |
| `bugprone-incorrect-roundings` | 2 | **review** — `(int)(x + 0.5)` rounding | 3 |
| `bugprone-implicit-widening-of-multiplication-result` | 2 | **review** — `a * b` widened after overflow | 3 |
| `performance-inefficient-vector-operation` | 1 | `reserve` before a push loop | 2 |
| `bugprone-branch-clone` | 1 | **review** — duplicated branch bodies | 3 |

## Per-module distribution (where the work is)

```
396 rendering    327 control     79 assets      64 scene       43 host
39 geometry     23 sceneedit    23 animation   16 json        11 app
10 signal        8 physics       4 script       3 core          1 window
```

`rendering` and `control` hold ~70%. Much of `control`'s share is in **generated** files
(`control_dto_serde.generated.cpp` 161, `scene_component_serde.generated.cpp` 4) — those are **fixed in
the `gen.ts` emitters** (`emitCpp`, `emitSceneSerde`), never by hand, then regenerated. The
`script_component_defs.generated.hpp` emitter already produces zero warnings; the others should match it.

## Phasing

| # | File | Scope |
|---|---|---|
| 1 | `phase-1-mechanical-autofix.md` | the high-volume, `--fix`-able style/perf checks (~780 sites) |
| 2 | `phase-2-structural-and-generated.md` | linkage / enum-size / arrays / value-params + the generated-file emitters |
| 3 | `phase-3-bugprone-and-gate.md` | the `bugprone-*` hand reviews + making `make lint` deterministic |

Order is by safety and volume: phase 1 is mechanical and reversible, phase 2 touches signatures/linkage,
phase 3 needs judgment. Do them module-by-module within each phase (`rendering` and `control` first) so
each change set is reviewable and rebuilds cleanly.

## Approach (applies to every phase)

- **One check at a time, per file.** Run `clang-tidy -p build/debug --checks='-*,<one-check>' --fix
  <file>` on individual TUs rather than `run-clang-tidy --fix` across the tree — the cross-TU fix
  aggregation is what trips the GLM overlap. Rebuild (`make engine`) and re-lint after each batch.
- **Generated files are fixed in the emitter**, then `bun run tools/gen-control-dto/gen.ts`; the
  `git diff` freshness gate then proves the hand and generated forms agree.
- **No suppressions.** A `// NOLINT` is allowed only where a check is provably a false positive on a
  specific line, with a one-line reason — never as a way to clear the backlog.
- **Verify per batch:** `make engine` clean, `make e2e` / `make check` green, and the touched files drop
  to zero warnings. The clean-build lint rule above keeps the measurement honest.

## Verification gate (plan is done when)

`make engine` then `make lint` on a freshly built tree exits **0** with no `engine/source` warnings, all
generated outputs are byte-fresh, and `make check` + `make e2e` stay green. No check was disabled and no
blanket suppression was added to get there.

## Risks

- **Auto-fix correctness.** clang-tidy `--fix` is usually safe but can mis-edit around macros/modules;
  every batch must compile and pass e2e before moving on.
- **Module rebuild cost.** Each batch needs a rebuild for the next lint to see fresh BMIs; budget for it.
- **Churn vs. concurrent work.** This touches many files; land it in focused, module-scoped commits and
  coordinate so it does not collide with in-flight feature branches.
- **Convention check.** None of the listed checks conflict with `CONVENTIONS.md` (designated initializers
  and `auto`-for-call-results are house style). If a future check does, that is the one case to discuss
  rather than blindly fix.
