# Saffron Anima — engine (Rust)

The new Rust engine. **Work in progress — the rewrite is being *planned*, not yet built.**

- The workspace crate graph and module structure are *designed* in
  [`../plans/rust-rewrite/`](../plans/rust-rewrite/) — pre-plan **PP-1** (area `00-foundations/`) — not
  improvised here. `Cargo.toml` is a placeholder virtual workspace until that phase scaffolds the real
  crates; `cargo build` will not produce anything yet.
- The C++26 implementation this replaces lives in [`../engine-old/`](../engine-old/) — **reference
  only**, to be deleted after the cutover (NO LEGACY). Nothing in `engine/` depends on it. Per the
  migration strategy the editor keeps running the C++ `SaffronAnima` binary until the Rust binary
  passes the migration gate.

Start here: [`../plans/rust-rewrite-pre-planning.md`](../plans/rust-rewrite-pre-planning.md).
