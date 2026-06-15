# Phase 3 — bugprone hand-reviews + a deterministic gate

**Status:** NOT STARTED

The last ~16 distinct warnings are `bugprone-*` and `misc-unused-parameters` — these are *not* blindly
auto-fixed, because a `bugprone` hit can be either a real latent defect or a deliberate, correct pattern.
Each is read and fixed (or justified with a narrow, reasoned `// NOLINT`). Then make `make lint`
deterministic so it stays green.

## Hand-review each bugprone hit

| Check | Distinct | What to check |
|---|---:|---|
| `bugprone-misplaced-widening-cast` | 4 | `(int64)(a * b)` where `a*b` already overflowed in 32-bit — widen the operands, not the result |
| `bugprone-exception-escape` | 3 | an exception can leave a path expected not to throw; the engine is no-exceptions (`std::expected`), so this usually means a `noexcept` / destructor / main-loop boundary needs a guard |
| `bugprone-narrowing-conversions` | 2 | implicit `int`↔`float`/`size_t` narrowing — make it an explicit, intended cast |
| `bugprone-incorrect-roundings` | 2 | `(int)(x + 0.5)` rounds wrong for negatives — use `std::lround` / `std::round` |
| `bugprone-implicit-widening-of-multiplication-result` | 2 | `size_t n = a * b` where `a*b` is computed in a narrower type first |
| `bugprone-branch-clone` | 1 | two branches with identical bodies — collapse, or make the intended difference explicit |
| `misc-unused-parameters` | 2 | remove, or `[[maybe_unused]]` if part of a required signature (callbacks, overrides) |

For each: open the site, decide real-bug vs intended, fix accordingly, and add an `e2e`/unit assertion if
a fix changes behavior (the rounding/widening ones can be observable). These are the only warnings where
the conclusion is a judgment, so record the reasoning in the commit message.

## Make the gate deterministic

The current `make lint` is unreliable for two reasons; both are fixed here so a clean tree stays provably
clean.

1. **BMI freshness (C++20 modules).** clang-tidy needs every module's BMI built, or it silently skips
   that TU and the run looks cleaner than it is. The `lint` target already requires
   `build/debug/compile_commands.json`; tighten it to depend on a completed `make engine` (or document
   and enforce "lint only after a full build"). Without this, "lint passed" is not trustworthy.

2. **The GLM fix-overlap exit.** `run-clang-tidy -p build/debug -quiet …` exits non-zero from
   `Fix conflicts with existing fix! … glm/detail/type_vec4.hpp` — overlapping FixIt ranges in a shared
   third-party header analyzed by many TUs. Options, in order of preference:
   - Confirm `HeaderFilterRegex: '(engine/source|tools/sa)'` also suppresses *fix computation* for
     third-party headers (it governs diagnostics; verify it also drops their FixIts). If not, add a
     `.clang-tidy` `ExcludeHeaderFilterRegex` (clang-tidy ≥ 19) for `_deps/`.
   - Pin the lint invocation so it never aggregates fixes (it should not with no `--fix`; investigate why
     a no-`--fix` run still reports a fix conflict — likely a check emitting a hard FixIt during
     diagnostics on the GLM header).
   - As a last resort, scope `run-clang-tidy` to a file list that excludes the third-party-heavy TUs from
     fix computation while still diagnosing engine code.

3. **Lint changed files in CI.** Once the backlog is zero, keep it zero cheaply: have CI lint the diff's
   touched TUs (and their module) rather than the whole tree every time. The full-tree run stays available
   for periodic audits. This is the durable guard against regression without the multi-minute full sweep
   on every change.

## Done when

`make engine` then `make lint` on a freshly built tree exits **0**, no `engine/source` warnings remain in
any phase's checks, generated outputs are byte-fresh, and `make check` + `make e2e` are green — with no
check disabled and no blanket suppression. The README's verification gate is satisfied.
