// Controlled chip-search bar: `value`/`onChange` over a SearchState, configured by `chips` (typed verbs).
// Renders the chip-input field + a clear button.
import * as React from "react";
import { useEffect, useMemo, useRef, useState } from "react";
import { Loader2, X } from "lucide-react";

import { Button } from "@/components/ui/button";
import { cn } from "@/lib/utils";

import { AnimaSearchField } from "./AnimaSearchField";
import {
  stateToTokens,
  tokensToState,
  type ChipConfig,
  type SearchState,
  type Token,
} from "./chipSearch";

const NO_CHIPS: ChipConfig[] = [];

interface AnimaSearchbarProps {
  value: SearchState;
  onChange: (next: SearchState) => void;
  chips?: ChipConfig[];
  placeholder?: string;
  className?: string;
  /** When > 0, free-text typing is debounced this many ms before onChange fires. Chip commits are immediate. */
  debounceMs?: number;
  commitOnBlur?: boolean;
  /** Optional external ref to the live input (e.g. so a parent can focus it on Ctrl+F). */
  inputRef?: React.RefObject<HTMLInputElement | null>;
  /** Show the built-in clear (X) button when there is content. Off when the host owns a close button. */
  showClear?: boolean;
  /** Show a circular spinner at the right edge (e.g. a search request is in flight). */
  busy?: boolean;
}

export function AnimaSearchbar({
  value,
  onChange,
  chips = NO_CHIPS,
  placeholder = "Search...",
  className,
  debounceMs = 0,
  commitOnBlur = false,
  inputRef,
  showClear = true,
  busy = false,
}: AnimaSearchbarProps) {
  const chipKeywords = useMemo(() => chips.map((c) => c.keyword), [chips]);
  const internalRef = useRef<HTMLInputElement | null>(null);
  const liveInput = inputRef ?? internalRef;

  const [tokens, setTokens] = useState<Token[]>(() =>
    stateToTokens(value).map((text) => ({ text })),
  );
  const lastEmitted = useRef<string>(JSON.stringify(value));
  const debounceTimer = useRef<ReturnType<typeof setTimeout> | null>(null);

  // Re-hydrate when the parent's value changes out from under us (not from our own emit).
  useEffect(() => {
    const key = JSON.stringify(value);
    if (key !== lastEmitted.current) {
      setTokens(stateToTokens(value).map((text) => ({ text })));
      lastEmitted.current = key;
    }
  }, [value]);

  const liveText = tokens[tokens.length - 1]?.text ?? "";
  const committedCount = tokens.length - 1;
  const hasContent = tokens.length > 1 || liveText !== "";

  const isIncompleteLive = (text: string): boolean => {
    if (text.startsWith(":")) return true;
    if (text === "") return false;
    return chipKeywords.some((kw) => text === `${kw}:`);
  };

  const clearDebounce = () => {
    if (debounceTimer.current !== null) {
      clearTimeout(debounceTimer.current);
      debounceTimer.current = null;
    }
  };

  const emitFromTokens = (nextTokens: Token[]) => {
    const texts = nextTokens.map((t) => t.text);
    const last = texts.length - 1;
    const emitTexts = last >= 0 && isIncompleteLive(texts[last]) ? texts.slice(0, -1) : texts;
    const state = tokensToState(emitTexts, chipKeywords);
    const key = JSON.stringify(state);
    if (key === lastEmitted.current) return;
    lastEmitted.current = key;
    onChange(state);
  };

  // A commit passes the post-commit tokens explicitly: our `tokens` state is still the
  // pre-commit value here (setTokens hasn't applied), and reading it would drop the just-
  // committed chip from the emitted query.
  const emitNow = (next?: Token[]) => {
    clearDebounce();
    emitFromTokens(next ?? tokens);
  };

  // Debounced emit for live-text edits (typing without committing).
  useEffect(() => {
    if (debounceMs <= 0) {
      emitFromTokens(tokens);
      return;
    }
    clearDebounce();
    debounceTimer.current = setTimeout(() => {
      debounceTimer.current = null;
      emitFromTokens(tokens);
    }, debounceMs);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [liveText]);

  useEffect(() => clearDebounce, []);

  const clearAll = () => {
    setTokens([{ text: "" }]);
    lastEmitted.current = JSON.stringify({ chips: [], freeText: "" });
    onChange({ chips: [], freeText: "" });
    requestAnimationFrame(() => liveInput.current?.focus());
  };

  return (
    <div className={cn("relative", className)}>
      <div
        className="flex min-h-9 flex-wrap items-center gap-1 rounded-md border border-input bg-background px-2 py-1 text-sm focus-within:border-ring focus-within:ring-2 focus-within:ring-ring/30"
        onClick={(e) => {
          if (e.target === e.currentTarget) liveInput.current?.focus();
        }}
      >
        <AnimaSearchField
          tokens={tokens}
          setTokens={setTokens}
          chipConfigs={chips}
          placeholder={committedCount === 0 ? placeholder : ""}
          inputRef={liveInput}
          className="flex-1"
          onChipCommit={emitNow}
          onFreeTextCommit={emitNow}
          onBlur={() => commitOnBlur && emitNow()}
        />
        {hasContent && showClear && !busy && (
          <Button
            type="button"
            variant="ghost"
            size="icon"
            className="ml-auto h-6 w-6 shrink-0"
            onClick={clearAll}
            aria-label="Clear search"
          >
            <X className="size-4" />
          </Button>
        )}
        {busy && (
          <Loader2
            className="ml-auto size-4 shrink-0 animate-spin text-muted-foreground"
            aria-label="Searching"
          />
        )}
      </div>
    </div>
  );
}
