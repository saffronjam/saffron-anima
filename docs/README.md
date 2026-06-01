# SaffronEngine documentation site

A [Hugo](https://gohugo.io/) site that explains how the engine's 3D rendering works,
concept by concept, and links each explanation back to the code that implements it.
It uses the [Book](https://github.com/alex-shpak/hugo-book) theme and is organised by the
[Diátaxis](https://diataxis.fr/) framework (explanation, how-to, reference, tutorial).

## Prerequisites

- **Hugo extended** (the Book theme compiles SCSS, so the extended build is required).
  There is no Hugo on the host or in the `saffron-build` toolbox; a copy is installed at
  `~/.local/bin/hugo`. To reinstall, grab the `hugo_extended` linux-amd64 tarball from the
  [Hugo releases](https://github.com/gohugoio/hugo/releases) and drop the binary on your PATH.
- The theme is a git submodule. After a fresh clone:
  ```sh
  git submodule update --init --depth 1 docs/themes/hugo-book
  ```

## Build and serve

```sh
cd docs
hugo server            # live preview, http://localhost:1313/saffron-engine/
hugo --minify          # static build into docs/public/
```

The preview lives under the `/saffron-engine/` path because `baseURL` targets GitHub Pages.
To serve at the root locally instead: `hugo server --baseURL http://localhost:1313/`.

## Writing pages

- One concept per page. Group related pages in a subfolder under `content/explanations/`.
- Front matter is TOML (`+++ … +++`). Start from an archetype:
  ```sh
  hugo new explanations/lighting-and-brdf/some-concept.md --kind explanation
  ```
- **Title**: a short noun phrase, sentence case, no leading "The", no `-ing`, no code/parens
  (bare-infinitive verb for how-to/tutorial tasks). Keep the front-matter `title` and the body
  `# H1` identical — the Book theme does not render the title itself, so each page needs its H1.
- **No status badge.** Done is the default and gets nothing. Flag only genuinely-unfinished work,
  as a one-line `> [!NOTE]`.
- **Concept first.** Lead with the idea and why, not "file X does Y" or quoting comments. Short
  paragraphs, active voice. Put code pointers in a slim "In the code" table (`What | File |
  Symbols`) — symbols are the durable anchor; don't pin line numbers.
- **Math**: LaTeX with `$…$` / `$$…$$` and `math = true` in front matter. KaTeX (vendored) loads
  via `layouts/_partials/docs/inject/head.html`, configured for single-`$` inline.
- **Diagrams**: fenced ` ```mermaid ` blocks. **Callouts**: GitHub alerts (`> [!NOTE]` etc.).
- **Voice**: plain and direct. Run prose through the `humanizer` pass (no inflated intros, no
  rule-of-three filler, sparing em dashes). Style follows Google's dev-docs headings guide + Diátaxis.

## Theme customisation

- `assets/_custom.scss` — the `theme-saffron` mixin (a dark, low-colour variant; selected via
  `BookTheme = 'saffron'` in `hugo.toml`) plus the Roboto / Roboto Mono font-family overrides.
- `layouts/_partials/docs/inject/head.html` — loads the Roboto web fonts and, on `math` pages,
  KaTeX.

## Layout

```
content/
  _index.md            landing
  overview.md          one-screen tour of the whole engine
  explanations/        the bulk — how/why each subsystem works (15 subfolders)
  how-to/              task recipes
  reference/           type / API / command catalog
  tutorials/           guided, end-to-end walkthroughs
```
