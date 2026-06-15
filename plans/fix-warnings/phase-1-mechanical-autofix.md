# Phase 1 — mechanical auto-fix sweep

**Status:** NOT STARTED

The high-volume, low-risk bulk: checks clang-tidy can `--fix` mechanically, none of which change a
signature or linkage. ~780 of the 1037 distinct warnings. Do it module-by-module (`rendering` 396 and
`control` 327 hold most of it), rebuilding and re-linting between batches.

## Checks in this phase

| Check | Distinct | Transformation |
|---|---:|---|
| `misc-const-correctness` | 295 | add `const` to locals never mutated after init |
| `modernize-use-designated-initializers` | 240 | `T{a, b}` → `T{.x = a, .y = b}` — already the house init style (`CONVENTIONS.md`) |
| `performance-move-const-arg` | 158 | drop `std::move` on a const/trivially-copyable arg, or fix the moved-from type |
| `modernize-use-auto` | 44 | `auto x = makeThing()` for call-result locals (`CONVENTIONS.md:67`) |
| `modernize-return-braced-init-list` | 12 | `return T{a, b}` → `return {a, b}` where the type is fixed |
| `modernize-use-scoped-lock` | 12 | `std::lock_guard<…>` → `std::scoped_lock` |
| `modernize-use-integer-sign-comparison` | 9 | mixed-sign `<`/`==` → `std::cmp_less` / `std::cmp_equal` |
| `modernize-use-ranges` | 7 | `std::sort(v.begin(), v.end())` → `std::ranges::sort(v)` |
| `modernize-use-emplace` | 5 | `push_back(T{…})` → `emplace_back(…)` |

## Procedure

For each module (start with `rendering`, then `control` non-generated, then the rest):

1. List the module's TUs (`.cpp` and `.cppm`).
2. For each check, per TU:
   ```sh
   toolbox run -c saffron-build bash -lc '
     clang-tidy -p build/debug --checks="-*,misc-const-correctness" --fix engine/source/saffron/<module>/<file>'
   ```
   One check at a time avoids overlapping FixIt ranges (the cross-fix conflict that breaks
   `run-clang-tidy --fix` on shared GLM headers).
3. `make engine` — the batch must compile. `import std` + modules mean a bad fix shows up fast.
4. `make format` (clang-format owns layout; the fixes may need reflowing).
5. Re-lint the module to confirm the check is at zero there.
6. Commit per module + check group so each diff is reviewable.

After all modules: `make e2e` (or `make check`) green, and a full `make lint` on a freshly built tree
shows these nine checks at zero.

## Watch-outs

- **`performance-move-const-arg` is not always a pure delete.** Sometimes the right fix is making the
  parameter a value/`&&` so the move is meaningful, not removing the `std::move`. Read each — but they are
  few and obvious.
- **`modernize-use-auto`** can reduce readability when the call-result type is not obvious; `CONVENTIONS.md`
  scopes it to call-result locals, which is exactly what the check targets. Leave explicit types where the
  type is a deliberate narrowing.
- **Designated initializers across aggregates with base classes or arrays** occasionally won't auto-fix;
  finish those by hand.
- Re-run after a rebuild — fixing one check can reveal another on the same line (e.g. a now-`const` local
  that also wants `auto`).
