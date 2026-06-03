+++
title = 'Theme & fonts'
weight = 10
+++

# Theme & fonts

A theme is the set of colors and fonts that gives every editor surface one consistent look. The editor's React UI is built with shadcn/ui (Radix primitives copied into the repo) on Tailwind CSS v4. The theme keeps the normal shadcn token shape, with neutral colors and system fonts as the default.

## The palette as shadcn tokens

shadcn styles every component from a small set of CSS custom properties (`--background`, `--primary`, `--border`, …). The editor sets those variables in `styles.css`, and the rest of the UI consumes semantic Tailwind utilities such as `bg-background`, `text-muted-foreground`, and `border-border`:

```css
:root {
  --background: oklch(1 0 0);
  --foreground: oklch(0.145 0 0);
  --primary: oklch(0.205 0 0);
  --border: oklch(0.922 0 0);
  --radius: 0.5rem;
}

.dark {
  --background: oklch(0.145 0 0);
  --foreground: oklch(0.985 0 0);
  --primary: oklch(0.922 0 0);
  --border: oklch(1 0 0 / 10%);
}
```

`index.html` pins `class="dark"` on `<html>`, so the editor starts in dark mode. An `@theme inline` block re-exports each variable as a Tailwind color (`--color-background: var(--background)`), so utilities like `bg-background` and `border-border` resolve through the theme tokens instead of hard-coded colors.

## Fonts

The theme defaults to system sans and monospace stacks. Roboto and Roboto Mono remain bundled and declared with `@font-face` so they are available without a font CDN if a component later opts into them:

```css
@font-face { font-family: "Roboto";      src: url("./assets/fonts/Roboto-Regular.ttf") format("truetype"); }
@font-face { font-family: "Roboto Mono";  src: url("./assets/fonts/RobotoMono-Regular.ttf") format("truetype"); }
```

`--font-sans` and `--font-mono` are exported through `@theme inline`, which lets Tailwind utilities use the app-level font choices consistently. Data-heavy fields can still use `font-mono` for aligned numbers and ids.

## Layout: a resizable dock

The dock is reproduced with `react-resizable-panels` (shadcn's `resizable`): Hierarchy plus a tabbed Inspector/Environment/Stats column on the left, Assets along the bottom, [Viewport](../viewport-panel/) in the center. The split ratios are left 0.20, bottom 0.28, and a 0.45/0.55 split within the left column. Render Stats is tabbed next to Inspector and Environment, which keeps every panel in a non-viewport region.

> [!NOTE]
> Every panel, handle, and Radix portal lives outside the viewport rect on purpose. The reparented native window always paints over its rect, so the dock arrangement is also the occlusion strategy — see [the native bridge](../tauri-editor-and-x11-bridge/) page.

## In the code

| What | File | Symbols |
|---|---|---|
| shadcn tokens | `editor/src/styles.css` | `:root` / `.dark` vars, `@theme inline` |
| Font defaults | `editor/src/styles.css` | `@font-face`, `--font-sans` / `--font-mono` |
| The dock layout | `editor/src/app/Layout.tsx` | `Layout`, `LeftBottomTabs`, the panel split sizes |
| Layout-settled bus | `editor/src/app/layoutBus.ts` | `emitLayoutSettled`, `onLayoutSettled` |

## Related

- [Tauri editor and the X11 bridge](../tauri-editor-and-x11-bridge/) — the shell the theme dresses + the occlusion rule
- [Viewport panel](../viewport-panel/) — the panel that fills the dock center
- [Inspector](../inspector/) — uses the mono font for its data fields
