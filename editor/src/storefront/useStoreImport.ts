// Import state + actions for one store result, shared by the result card and the expand
// modal so both drive the same whole-asset / per-part import path.
import { useCallback, useState } from "react";

import { errorText, notify, notifyError } from "../lib/flash";
import { useEditorStore } from "../state/store";
import {
  storeAssetParts,
  storeImport,
  storeImportPart,
  type AssetPart,
  type StoreResult,
} from "./types";

export function useStoreImport(result: StoreResult) {
  const [importing, setImporting] = useState(false);
  // Download progress (0–1) while importing; null when idle.
  const [progress, setProgress] = useState<number | null>(null);
  const [parts, setParts] = useState<AssetPart[] | null>(null);
  const [partsLoading, setPartsLoading] = useState(false);
  const refreshAssets = useEditorStore((s) => s.refreshAssets);

  const importWhole = useCallback(
    (resolution?: string) => {
      setImporting(true);
      setProgress(0);
      storeImport(result, resolution, (f) => setProgress(f))
        .then((asset) => {
          notify(`Imported “${asset.name || result.name}” from ${result.store.displayName}`);
          void refreshAssets();
        })
        .catch((err: unknown) => notifyError(errorText(err)))
        .finally(() => {
          setImporting(false);
          setProgress(null);
        });
    },
    [result, refreshAssets],
  );

  // Parts are fetched lazily the first time the dropdown opens.
  const loadParts = useCallback(
    (open: boolean) => {
      if (!open || parts !== null || partsLoading) return;
      setPartsLoading(true);
      storeAssetParts(result)
        .then(setParts)
        .catch((err: unknown) => {
          notifyError(errorText(err));
          setParts([]);
        })
        .finally(() => setPartsLoading(false));
    },
    [result, parts, partsLoading],
  );

  const importPart = useCallback(
    (part: AssetPart, resolution?: string) => {
      storeImportPart(result, part, resolution)
        .then((asset) => {
          notify(`Imported “${asset.name}” from ${result.store.displayName}`);
          void refreshAssets();
        })
        .catch((err: unknown) => notifyError(errorText(err)));
    },
    [result, refreshAssets],
  );

  return { importing, progress, parts, partsLoading, importWhole, loadParts, importPart };
}
