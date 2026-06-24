// The Store main tab: a centered global search across the project's enabled connectors, with a
// provider-setup modal (gear) and a credits view (icon). Search fires only on Enter / chip-commit.
//
// Which connectors a project uses persists in project.json's `stores` block (shared with the
// team); each connector's secret lives only in the OS keyring (per machine). Opening the Store
// with nothing enabled auto-opens the provider modal ("Add your first provider").
import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { Award, Loader2, Settings } from "lucide-react";

import { Button } from "@/components/ui/button";
import { Tooltip, TooltipContent, TooltipTrigger } from "@/components/ui/tooltip";

import { AnimaSearchbar } from "../components/anima/AnimaSearchbar";
import type { ChipConfig, SearchState } from "../components/anima/chipSearch";
import { client } from "../control/client";
import { errorText, notifyError } from "../lib/flash";
import { ProviderModal } from "./ProviderModal";
import { StoreCredits } from "./StoreCredits";
import { StoreResultsGrid } from "./StoreResultsGrid";
import {
  storeListConnectors,
  storeSearchSession,
  type ConnectorInfo,
  type SearchQuery,
  type StoreKind,
} from "./types";

const KIND_OPTIONS: { value: StoreKind; label: string }[] = [
  { value: "model", label: "Model" },
  { value: "hdri", label: "HDRI" },
  { value: "material", label: "Material" },
  { value: "texture", label: "Texture" },
];

const EMPTY_SEARCH: SearchState = { chips: [], freeText: "" };

// AnimaSearchbar emits onChange on a debounce for free-text typing and immediately on a commit
// (Enter / chip). A very large debounce suppresses the typing emit, so onChange reaches us only
// on a committed search — the "search on Enter only" rule.
const COMMIT_ONLY_DEBOUNCE_MS = 600_000;

// `active` is false while the tab is mounted-but-hidden; we suppress the portaled provider
// modal then so it can't float over whichever tab is showing.
export function StoreWorkspace({ active }: { active: boolean }) {
  const [connectors, setConnectors] = useState<ConnectorInfo[]>([]);
  const [enabled, setEnabled] = useState<string[]>([]);
  const [loaded, setLoaded] = useState(false);
  const [providerModalOpen, setProviderModalOpen] = useState(false);
  const [showCredits, setShowCredits] = useState(false);
  const [search, setSearch] = useState<SearchState>(EMPTY_SEARCH);
  const [session, setSession] = useState<string | null>(null);
  const [searching, setSearching] = useState(false);
  const searchRef = useRef<HTMLInputElement | null>(null);

  // Auto-focus the searchbar when the tab opens or is returned to (but not behind the
  // provider modal, which owns focus). rAF so the just-unhidden input is focusable.
  useEffect(() => {
    if (!active || !loaded || providerModalOpen) return;
    const id = requestAnimationFrame(() => searchRef.current?.focus());
    return () => cancelAnimationFrame(id);
  }, [active, loaded, providerModalOpen]);

  useEffect(() => {
    Promise.all([storeListConnectors(), client.getStores()])
      .then(([list, stores]) => {
        setConnectors(list);
        setEnabled(stores.enabled ?? []);
        // First use: nothing enabled → prompt to add a provider.
        if ((stores.enabled ?? []).length === 0) setProviderModalOpen(true);
      })
      .catch((err: unknown) => notifyError(errorText(err)))
      .finally(() => setLoaded(true));
  }, []);

  const persistEnabled = useCallback((next: string[]) => {
    setEnabled(next);
    client
      .setStores(next)
      .then(() => client.saveProject())
      .catch((err: unknown) => notifyError(errorText(err)));
  }, []);

  const chips = useMemo<ChipConfig[]>(
    () => [
      {
        keyword: "provider",
        label: "Provider",
        options: (input) => {
          const needle = input.toLowerCase();
          return connectors
            .filter((c) => c.id.includes(needle) || c.displayName.toLowerCase().includes(needle))
            .map((c) => ({ value: c.id, label: c.displayName }));
        },
        resolveLabel: (value) => connectors.find((c) => c.id === value)?.displayName ?? null,
      },
      {
        keyword: "type",
        label: "Type",
        options: (input) => {
          const needle = input.toLowerCase();
          return KIND_OPTIONS.filter((o) => o.value.includes(needle)).map((o) => ({
            value: o.value,
            label: o.label,
          }));
        },
        resolveLabel: (value) => KIND_OPTIONS.find((o) => o.value === value)?.label ?? null,
      },
    ],
    [connectors],
  );

  const runSearch = (next: SearchState) => {
    setSearch(next);
    setShowCredits(false);
    const providerChips = next.chips.filter((c) => c.keyword === "provider").map((c) => c.value);
    const kindChip = next.chips.find((c) => c.keyword === "type");
    const query: SearchQuery = {
      text: next.freeText.trim(),
      kind: kindChip ? (kindChip.value as StoreKind) : undefined,
      // Default the scope to the project's enabled set; explicit provider chips override.
      providers: providerChips.length > 0 ? providerChips : enabled,
    };
    storeSearchSession(query)
      .then(setSession)
      .catch((err: unknown) => notifyError(errorText(err)));
  };

  // Landing page: with providers enabled but no search yet, run an empty query so the grid
  // is prefilled (Poly Haven returns all models, ambientCG popular materials). Runs once —
  // `session` is set afterwards, so the guard stops it repeating.
  useEffect(() => {
    if (active && loaded && enabled.length > 0 && session === null) {
      runSearch(EMPTY_SEARCH);
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [active, loaded, enabled, session]);

  if (!loaded) {
    return (
      <main className="flex min-h-0 flex-1 items-center justify-center bg-background">
        <Loader2 className="size-8 animate-spin text-muted-foreground" />
      </main>
    );
  }

  return (
    <main className="flex min-h-0 flex-1 flex-col bg-background">
      <div className="relative flex shrink-0 items-center border-b border-border p-2">
        <div className="mx-auto w-[60%]">
          <AnimaSearchbar
            value={search}
            onChange={runSearch}
            chips={chips}
            placeholder="Search assets — press Enter"
            debounceMs={COMMIT_ONLY_DEBOUNCE_MS}
            inputRef={searchRef}
            busy={searching}
          />
        </div>
        <div className="absolute top-1/2 right-2 flex -translate-y-1/2 items-center gap-1">
          <Tooltip>
            <TooltipTrigger asChild>
              <Button
                type="button"
                size="icon-sm"
                variant={showCredits ? "secondary" : "ghost"}
                onClick={() => setShowCredits((c) => !c)}
                aria-label="Credits"
              >
                <Award />
              </Button>
            </TooltipTrigger>
            <TooltipContent>Asset credits</TooltipContent>
          </Tooltip>
          <Tooltip>
            <TooltipTrigger asChild>
              <Button
                type="button"
                size="icon-sm"
                variant="ghost"
                onClick={() => setProviderModalOpen(true)}
                aria-label="Manage providers"
              >
                <Settings />
              </Button>
            </TooltipTrigger>
            <TooltipContent>Manage providers</TooltipContent>
          </Tooltip>
        </div>
      </div>

      {showCredits ? (
        <StoreCredits />
      ) : session === null ? (
        <div className="flex flex-1 items-center justify-center p-6 text-center text-sm text-muted-foreground italic">
          Nothing here — search for any asset.
        </div>
      ) : (
        <StoreResultsGrid session={session} active={active} onLoadingChange={setSearching} />
      )}

      <ProviderModal
        open={providerModalOpen && active}
        onOpenChange={setProviderModalOpen}
        connectors={connectors}
        enabled={enabled}
        onEnabledChange={persistEnabled}
      />
    </main>
  );
}
