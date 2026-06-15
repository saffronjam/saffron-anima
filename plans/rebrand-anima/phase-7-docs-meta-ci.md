# Phase 7 — Docs, meta, CI, repo URL + final gate

**Status:** COMPLETED

The prose pass: rename the product in docs, README, the agent/convention files, and CI comments;
update the repo URL and Hugo base path. Then run the full cross-cutting gate that the per-phase
engine builds didn't cover.

## Docs site (`docs/`)

- `docs/hugo.toml`: `baseURL` `…/saffron-anima/` → `…/saffron-anima/`; site `title`
  `saffron-anima` → `saffron-anima`; `BookRepo` `…/saffron-anima` → `…/saffron-anima`.
  (The `BookTheme` style token — leave unless it's a brand string.)
- `docs/content/`: ~74 of 198 pages mention the brand. Rename **product** mentions
  "SaffronAnima" / "Saffron Anima" → "Saffron Anima" in titles, H1s, prose, and the `sa` CLI
  references → `sa`. Keep module names `Saffron.<Area>`, paths `engine/source/saffron/…`, the
  `saffron-build` toolbox, `SAFFRON_*` env vars, and `.smesh`/`.smat` — those are retained.
  Remember the docs rule: front-matter `title` must equal the body `# H1`. Update the relevant hub
  `_index.md` rows. Run the `humanizer` pass on rewritten prose.
- The scripting docs: update `se.*` examples → `sa.*` to match phase 4.

## Meta / project files

- `README.md`: `<h1>SaffronAnima</h1>` → `Saffron Anima`; badge + repo links `saffron-anima` →
  `saffron-anima`; the binary path / product prose → `SaffronAnima` / "Saffron Anima". Keep the
  `saffron-build` toolbox mention.
- `AGENTS.md`: the title, the architecture paragraph (`Saffron::Anima` → `Saffron::Anima`,
  `SaffronAnima` exe → `SaffronAnima`), binary paths, `SAFFRON_ANIMA_BIN` → `SAFFRON_ANIMA_BIN`,
  the `sa` CLI → `sa`. Keep `Saffron.<Area>`, `saffron/` paths, `saffron-build`, `@saffron/protocol`,
  `SAFFRON_*` (the retained set). `CLAUDE.md` just `@AGENTS.md` — no change.
- `CONVENTIONS.md`: title/opening "Saffron …" — keep "Saffron" (family); no "engine" tokens to change.
  The module-naming row stays `Saffron.<Area>`.

## CI / infra

- `.github/workflows/ci.yml`: the header comments naming "SaffronAnima" → "Saffron Anima". Keep the
  `runs-on: [self-hosted, saffron-build]` runner label.
- `tools/ci/check.sh`, `tools/check-control-schema/package.json`, `tools/check-projects/check.sh`,
  `tools/ci/README.md`: the "SaffronAnima" prose → "Saffron Anima". Binary path + `se`→`sa` socket
  were handled in phases 1/5; keep `saffron-build`.

## Final gate (the whole rebrand is done when)

```sh
make engine && make prepare-for-commit     # clean build, format + lint green
make check                                  # build → present-only smoke → schema contract → editor build
make e2e                                    # headless engine driven over the control plane, validation-clean
cd docs && hugo --minify                    # docs build clean on the new base path
```
Plus: a headless `build/debug/bin/SaffronAnima` exits clean; a `sa.*` Lua script runs; the editor
window reads "Saffron Anima"; and a tree-wide grep for `SaffronAnima`, `Saffron::Anima`, `\bse::`,
`sa-types`, ` se\.` (Lua), `tools/sa`, `SAFFRON_ANIMA_BIN` returns **only** intentionally-retained
matches (none, for these). Then update the README/plan `Status:` lines to COMPLETED.

## External follow-up (user action, not in-tree)

- Rename the GitHub repo `saffron-anima` → `saffron-anima` and update `git remote set-url` (a git
  write — leave for the user). Confirm GitHub Pages serves the new `…/saffron-anima/` base path.
