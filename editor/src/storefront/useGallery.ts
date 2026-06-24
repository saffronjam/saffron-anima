// Gallery state for a store result, split in two so a card and its expand modal can share
// one fetch but each keep its own current-slide index (navigating in the modal must not
// move the card behind it).
import { useCallback, useEffect, useMemo, useRef, useState } from "react";

import { storeAssetGallery, type GalleryImage, type StoreResult } from "./types";

// How the current slide changed: arrow nav slides, a thumbnail jump fades.
export type GalleryMode = "slide" | "fade";

export interface GalleryNav {
  index: number;
  dir: 1 | -1;
  mode: GalleryMode;
  /** Bumped on every navigation so a fade can be replayed even to the same image. */
  tick: number;
  next: () => void;
  prev: () => void;
  goTo: (i: number) => void;
}

/** Lazily fetches an asset's preview images; falls back to the card thumbnail on failure. */
export function useGallery(result: StoreResult, enabled: boolean): { images: GalleryImage[] } {
  const [images, setImages] = useState<GalleryImage[] | null>(null);
  const fetched = useRef(false);

  useEffect(() => {
    if (!enabled || fetched.current) return;
    fetched.current = true;
    // A gallery fetch failure is silent: the card already shows its thumbnail, so we fall
    // back to it rather than toasting on hover.
    storeAssetGallery(result)
      .then((imgs) => setImages(imgs.length > 0 ? imgs : [{ url: result.thumbnailUrl }]))
      .catch(() => setImages([{ url: result.thumbnailUrl }]));
  }, [enabled, result]);

  const list = useMemo(
    () => images ?? [{ url: result.thumbnailUrl }],
    [images, result.thumbnailUrl],
  );
  return { images: list };
}

/** Per-view navigation over `count` images: arrows slide, `goTo` fades. */
export function useGalleryNav(count: number): GalleryNav {
  const [state, setState] = useState({
    index: 0,
    dir: 1 as 1 | -1,
    mode: "slide" as GalleryMode,
    tick: 0,
  });

  const next = useCallback(() => {
    setState((s) => ({ index: (s.index + 1) % count, dir: 1, mode: "slide", tick: s.tick + 1 }));
  }, [count]);
  const prev = useCallback(() => {
    setState((s) => ({
      index: (s.index - 1 + count) % count,
      dir: -1,
      mode: "slide",
      tick: s.tick + 1,
    }));
  }, [count]);
  const goTo = useCallback((i: number) => {
    setState((s) => ({ index: i, dir: i >= s.index ? 1 : -1, mode: "fade", tick: s.tick + 1 }));
  }, []);

  const index = count > 0 ? Math.min(state.index, count - 1) : 0;
  return { index, dir: state.dir, mode: state.mode, tick: state.tick, next, prev, goTo };
}
