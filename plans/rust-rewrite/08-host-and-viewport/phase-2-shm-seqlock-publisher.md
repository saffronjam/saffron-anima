# Phase 2 — the POSIX-shm seqlock publisher (host side of the FROZEN frame transport)

**Status:** COMPLETED

**Depends on:** 08-host-and-viewport:phase-1-app-crate-run-loop-and-layer, 06-rendering:phase-3-gpu-resources

## Goal

Port the shm-publish host wiring from `renderer_capture.cpp`: create/recreate a per-view POSIX-shm
segment (`shm_open`/`ftruncate`/`mmap` via rustix), initialize the FROZEN 32-byte header, and on each
publish memcpy the staging pixels into the ring slot and bump the seq under a release fence. This is the
producer half of the byte-exact ABI; phase-3 validates it against the unchanged `wayland_viewport.rs`
reader. No editor presenter, no overlay — this phase produces a correct segment and a synthetic test
proves the bytes, with the real GPU-fed publish wired through 06-rendering's `recordShmPublishCopy` seam.

## Why this shape (NO LEGACY)

- **The ABI is frozen; we reproduce, not redesign.** The header layout, magic, ring-slot count,
  4K-floored capacity, BGRA8 byte order, seq-0-means-no-frame, and the `next = seq+1` slot index
  (`slot = next % ring_slots`, so the first frame lands in **slot 1**) are all dictated by
  `renderer_capture.cpp` + `wayland_viewport.rs`. No field is reordered, renamed, or "improved".
- **rustix, not nix or libc directly** (PP-2): `rustix::shm`/`rustix::mm::mmap`/`rustix::fs::ftruncate`
  for `shm_open(O_CREAT|O_RDWR, 0600)`, `mmap(MAP_SHARED, PROT_READ|PROT_WRITE)`, `shm_unlink`. The shm
  seam is one of the three `#![allow(unsafe_code)]` crates (`saffron-host`); the mmap'd pointer writes
  are the unsafe, with a top-of-file justification naming the shm seam.
- **The seqlock is a `fence(Release)`, not an atomic field.** C++ writes the pixels + w/h with plain
  stores, then `std::atomic_thread_fence(memory_order_release)`, then the seq store. Rust:
  `core::sync::atomic::fence(Ordering::Release)` between the non-atomic header/pixel writes and the seq
  store (a `write_volatile` or a `*mut u32` store). The release fence guarantees a reader observing the
  new seq sees the matching w/h + pixels — exactly the contract `step_view` relies on
  (`ptr::read_volatile(header)` for magic, `header.add(3)` for seq, then reads w/h/slots/capacity).
- **Grow-only segment, both views created at startup.** `enable_viewport_shm_publish` creates the
  segment *now* (not lazily) at `MinShmSlotCapacity` so the presenter's blocking open succeeds for a
  view that hasn't rendered yet; seq stays 0 so the reader shows nothing. `publish` recreates only when
  a frame outgrows the slot (`pixel_bytes > slot_capacity`), at `max(pixel_bytes, MinShmSlotCapacity)`.
  Recreate `shm_unlink`s the old name so the reader's inode-change probe (`stat_shm`) remaps. This
  startup-create-both is load-bearing and ported verbatim.
- **No `Arc`/`Mutex` on the shm itself.** The publisher runs on the render/main thread only (publish
  fires in `begin_frame` after the frame fence wait — `renderer.cppm:962`); the reader is a separate
  process. The segment state (`fd`, `base`, `mapped_size`, `slot_capacity`, `seq`, `name`) is a plain
  owned `ShmPublish` struct per view, with `Drop` doing `munmap`/`close`/`shm_unlink` (the C++
  `destroyShmPublishSlots` order).
- **The renderer-side record stays in 06-rendering.** `record_shm_publish_copy` (offscreen → BGRA8 blit
  → staging copy → host barrier) and `ensure_shm_publish_slot` (the per-frame VMA staging image+buffer)
  belong to 06-rendering phase-16; this phase owns only `recreate_shm_segment` /
  `enable_viewport_shm_publish` / `publish_shm_publish_slot` / `destroy_shm_publish` — the mmap + memcpy
  + seqlock. The division mirrors the C++ split between `renderer_capture.cpp` and `renderer.cppm`.

## Grounding (real files/symbols)

- `engine-old/source/saffron/rendering/renderer_capture.cpp`: `ShmMagic = 0x53465632`,
  `ShmHeaderBytes = 32`, `ShmRingSlots = 4`, `MinShmSlotCapacity = 3840*2160*4`; `recreateShmSegment`
  (the create/mmap/header-init, lines 146-191 — header `[magic,0,0,0,ringSlots,capacity,0,0]`);
  `enableViewportShmPublish` (create-now, both views, lines 193-208); `publishShmPublishSlot` (memcpy +
  `header[1]=width`/`header[2]=height`, `seq=next`, `atomic_thread_fence(release)`, `header[3]=next`,
  lines 270-304); `destroyShmPublishSlots`/`destroyShmPublish` (munmap/close/shm_unlink order).
- `engine-old/source/saffron/rendering/renderer_types.cppm`: `ShmPublish` (`enabled`, `name`, `slots`,
  `fd`, `base`, `mappedSize`, `slotCapacity`, `seq`), `ShmPublishSlot`, `activeShmPublish`, `activeView`.
- `engine-old/source/saffron/rendering/renderer.cppm`: `recordShmPublishCopy` (lines 2389-2449, the
  record side — referenced, lives in 06), the `beginFrame` shm publish branch (962-972, publish after
  the frame-fence wait).
- `editor/src-tauri/src/wayland_viewport.rs` (the oracle): `SHM_MAGIC = 0x5346_5632`,
  `SHM_HEADER_BYTES = 32`, `step_view` (reads `header` magic, `header.add(3)` seq, `add(1)`/`add(2)`
  w/h, `add(4)` slots, `add(5)` capacity; slot = `seq % slots`; rejects if `pixel_bytes > capacity` or
  the ring overflows `total`), `open_shm`/`stat_shm` (the inode/size remap probe).

## Acceptance gate

- Cargo workspace compiles; `cargo build -p saffron-host`; `saffron-host` root carries
  `#![allow(unsafe_code)]` + a justification naming the shm seam; `cargo clippy` clean.
- Unit `#[test]`s on a real `ShmPublish` against a freshly mmap'd segment:
  - `header_is_byte_identical`: after `enable_viewport_shm_publish`, the eight header `u32`s read back
    `[0x5346_5632, 0, 0, 0, 4, MinShmSlotCapacity as u32, 0, 0]` (magic, no-frame, ring=4, capacity).
  - `publish_seqlock_first_frame_lands_in_slot_1`: publishing a known WxH buffer sets `header[3]==1`
    and writes the pixels at offset `HEADER + 1*slot_capacity` (NOT slot 0) — reproducing `next = seq+1`.
  - `seqlock_read_back_is_consistent`: a same-process reader mirroring `step_view` (read magic+seq,
    copy `seq % slots`, re-read seq) sees the published w/h + a byte-identical pixel copy after the
    release fence; a torn read is impossible for a settled frame.
  - `grow_recreates_and_unlinks`: a publish larger than `slot_capacity` recreates the segment at the
    larger capacity, the old name is `shm_unlink`'d, header re-inits with the new capacity, seq resets
    to 0 then 1 on the next publish.
  - `drop_munmaps_and_unlinks`: dropping the `ShmPublish` `munmap`s, `close`s, and `shm_unlink`s (probe
    `stat_shm` returns `None` after drop).
- A `bytemuck`/`#[repr(C)]` const assert pins the 32-byte header (8 × `u32`) and `MinShmSlotCapacity`.

## Where it landed (reconciliation, NO LEGACY)

The byte-exact producer is `saffron_rendering::ShmPublish`
(`crates/rendering/src/shm_publish.rs`), not a copy in `saffron-host`: 06-rendering
phase-16 (COMPLETED) already ported `recreateShmSegment`/`publish`/`Drop` there as a
renderer type, matching the C++ where `ShmPublish` lives in `renderer_capture.cpp` and the
renderer's frame loop publishes — and avoiding a host→rendering→host dependency cycle.
There is exactly one publisher and one code path. This phase (1) strengthened that single
producer to fully cover the gate (`first_frame_lands_in_slot_1_not_slot_0`,
`drop_munmaps_and_unlinks_the_segment`, a `const _` layout assert pinning the 32-byte
header + `MIN_SHM_SLOT_CAPACITY` + magic + ring), and (2) landed the host-side *wiring*
in `saffron-host` (the README §4 host responsibility): `crates/host/src/viewport_shm.rs`
ports `host.cppm:1043-1052` — the env-driven per-view selection (`configs_from_env`) and a
`ViewportShmPublisher` holding one `ShmPublish` per enabled view, keyed on the FROZEN wire
tokens (`"scene"`/`"assetPreview"`). `saffron-host` is now a lib+bin carrying
`#![allow(unsafe_code)]` with a justification naming the shm seam.
