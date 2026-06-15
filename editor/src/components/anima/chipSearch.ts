// Chip-search model: a query is a set of typed-verb chips (`keyword:value`) plus free text. Ported from
// saffron-hive's HiveSearchbar — pure, framework-agnostic logic shared by AnimaSearchField/AnimaSearchbar.
import type { VariantProps } from "class-variance-authority";

import type { badgeVariants } from "@/components/ui/badge";

export type ChipVariant = NonNullable<VariantProps<typeof badgeVariants>["variant"]>;

export interface ChipOption {
  value: string;
  label: string;
}

export interface ChipConfig {
  keyword: string;
  label: string;
  variant?: ChipVariant;
  options: (input: string) => ChipOption[];
  /**
   * Given a committed chip's raw value (e.g. an entity id), return the label to display on the chip.
   * Return null to fall back to the raw value. Useful when option values are ids and labels are names.
   */
  resolveLabel?: (value: string) => string | null;
}

export interface SearchChip {
  keyword: string;
  value: string;
}

export interface SearchState {
  chips: SearchChip[];
  freeText: string;
}

/**
 * The last token is always the live (currently-edited) token; earlier tokens are committed chips or
 * committed free-text fragments. Exported so both components can type against it.
 */
export interface Token {
  text: string;
}

export function emptySearchState(): SearchState {
  return { chips: [], freeText: "" };
}

/** If `text` looks like `<keyword>:...` and `<keyword>` is in `keywords`, return the keyword; else null. */
export function matchChipKeyword(text: string, keywords: readonly string[]): string | null {
  const idx = text.indexOf(":");
  if (idx <= 0) return null;
  const kw = text.slice(0, idx);
  return keywords.includes(kw) ? kw : null;
}

/** Turn a SearchState into the flat, space-joined raw query string. */
export function serialize(state: SearchState): string {
  const parts: string[] = [];
  for (const c of state.chips) parts.push(`${c.keyword}:${c.value}`);
  if (state.freeText) parts.push(state.freeText);
  return parts.join(" ");
}

/**
 * Parse a raw (space-separated) query into a SearchState, consulting the chip keywords. Tokens matching
 * `<keyword>:...` where `<keyword>` is configured become chips; everything else is free text. Unknown
 * keywords fall through to free text. Best-effort for single-word values — multi-word values cannot
 * round-trip through this function (space is the delimiter).
 */
export function parseQuery(query: string, keywords: readonly string[]): SearchState {
  const chips: SearchChip[] = [];
  const free: string[] = [];
  for (const raw of query.split(" ")) {
    if (!raw) continue;
    const kw = matchChipKeyword(raw, keywords);
    if (kw !== null) {
      chips.push({ keyword: kw, value: raw.slice(kw.length + 1) });
    } else {
      free.push(raw);
    }
  }
  return { chips, freeText: free.join(" ") };
}

/**
 * Build the internal token list from a SearchState. Multi-word values are preserved verbatim in their
 * own token. The trailing empty string represents the live (currently-edited) token.
 */
export function stateToTokens(state: SearchState): string[] {
  const out: string[] = [];
  for (const c of state.chips) out.push(`${c.keyword}:${c.value}`);
  if (state.freeText) out.push(state.freeText);
  out.push("");
  return out;
}

/**
 * Collapse committed token texts back into a SearchState. Chip tokens are extracted in order; all
 * free-text tokens are joined with a space into the single `freeText` field.
 */
export function tokensToState(tokens: readonly string[], keywords: readonly string[]): SearchState {
  const chips: SearchChip[] = [];
  const free: string[] = [];
  for (const text of tokens) {
    if (!text) continue;
    const kw = matchChipKeyword(text, keywords);
    if (kw !== null) {
      chips.push({ keyword: kw, value: text.slice(kw.length + 1) });
    } else {
      free.push(text);
    }
  }
  return { chips, freeText: free.join(" ") };
}
