---
name: docs-page
description: Write or update SaffronEngine docs pages under docs/ in the house style. Use when a change adds or alters an engine concept (the AGENTS.md keep-docs-current rule), or when asked to "document X", "add a docs page", "update the docs", "explain X in the docs", or to review docs for style. Covers page structure, titles, voice, hub rows, and the build + link-check verify loop.
---

# Writing SaffronEngine docs pages

The docs are a Hugo site (`docs/`, hugo-book theme, Diátaxis layout) where every engine concept
gets one explanation page that links to the implementing code. AGENTS.md ("Keep current") makes
this part of "done": a change that adds or alters a concept updates the matching page and its hub
row in the same change.

## Map

```
docs/content/
  _index.md        landing          overview.md   one-screen engine tour
  explanations/    the bulk — 16 subsystem subfolders, each with a hub _index.md
  how-to/          task recipes     reference/    terse lookup tables
  tutorials/       guided walkthroughs
```

Each subfolder hub `_index.md` holds a `| Page | Covers | Code |` table — the authoritative page
list for that subsystem. Build artifacts (`docs/public/`, `docs/resources/`) are gitignored.

## Workflow

1. **Ground in source first.** Read the actual implementing code before writing. Anchor on
   symbol names, never line numbers (they drift). If a hub row cites a symbol, open the file and
   confirm it exists before repeating the claim.
2. **Write or update the leaf page** (template below). New page: copy
   `docs/archetypes/explanation.md` or run `hugo new explanations/<sub>/<slug>.md --kind explanation`.
3. **Update the hub row** in the subfolder's `_index.md` (and `explanations/_index.md` +
   `overview.md` subsystem tables if a whole new subfolder appears).
4. **Verify** (section below). Never ship without the build + link check.

## Page template

```markdown
+++
title = 'Short noun phrase'   # MUST equal the body H1
weight = N                    # row position in the hub table
math = true                   # ONLY if the page uses $…$ / $$…$$
+++

# Short noun phrase

<1–2 sentences: the concept and why it exists — not what some file does>

## How it works
<concept-first prose; 2–3 sentence paragraphs; small code excerpt only when it
clarifies; ```mermaid for flow/structure; $$…$$ for math>

## In the code
| What | File | Symbols |
|---|---|---|
| ... | `short_filename.ext` | `symbolA`, `symbolB` |

## Related
- [Sibling](../slug/) — one-line why
```

## Titles

- Short noun phrase, sentence case. **No leading "The/A/An"**, no `-ing` opener, no code or
  parentheses ("Main loop", not "The main loop and run()").
- How-to / tutorial titles are tasks: bare-infinitive verb ("Import a model").
- Front-matter `title` and body `# H1` stay identical — hugo-book does not render the title, so
  the H1 is required; a second H1 means a doubled heading.
- Retitling: change `title` + H1 only. **Never rename the file/slug or change `weight`** — links
  resolve by slug.

## Voice and status

- **Concept first.** Lead with the idea and the why; never "file X does Y" narration or quoting
  code comments. The "In the code" table carries the pointers.
- **No status badges.** Done is the default and gets nothing. Only genuinely unfinished or
  hardware-gated behaviour gets a one-line `> [!NOTE]`.
- Humanizer rules: use is/are/has (never "serves as"), no rule-of-three filler, no inflated
  intros or "-ing" fake-depth clauses, sparing em dashes, vary sentence length.
- Callouts: GitHub alerts (`> [!NOTE]` / `[!TIP]` / `[!WARNING]`). Math: `$…$`/`$$…$$` with
  `math = true` (loads KaTeX; configured for single-`$` inline). Diagrams: ```mermaid fences.
- Keep provenance facts intact when versions move (e.g. dynamic rendering is *1.3 core*
  regardless of what version the engine *targets*) — update target claims only.

## Verify

```sh
cd docs && hugo --gc                       # must exit 0, no ERROR (hugo on PATH, ~/.local/bin)
python3 <skill-dir>/scripts/check_links.py docs/public   # from repo root; expect "BROKEN LINKS: none"
make run-docs                              # live preview at http://localhost:1313/saffron-engine/
```

Hugo does NOT validate plain markdown links — the checker is the only gate against 404s. If the
page uses math or mermaid, load it in the preview and confirm both render.

## Commit

Docs-only commits use the `agent-commit --guide` format: `docs: <what>` (lowercase after colon,
< 72 chars), factual bullets, plain words, **no co-author or AI-attribution lines**.
