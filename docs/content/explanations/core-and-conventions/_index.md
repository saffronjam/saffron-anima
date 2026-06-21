+++
title = 'Core & conventions'
weight = 1
bookCollapseSection = true
+++

# Core & conventions

`saffron-core` is the foundation crate every other crate depends on: the typed error model, the
core primitives, the ownership rules, the signal/slot system, the JSON gateway, and the Rust house
style the whole workspace follows. These conventions reappear on every later page, so this section
comes first.

## Pages

| Page | Covers | Code |
|---|---|---|
| [go-flavored-design](go-flavored-design/) | the Rust house style — idiomatic Rust, clippy-is-law, `thiserror`, `Arc` | `engine/Cargo.toml` · workspace lints |
| [error-handling](error-handling/) | typed per-crate errors with `thiserror`, the `?` operator, no panics | `core/src/error.rs` · `Error`, `Result` |
| [type-aliases-and-primitives](type-aliases-and-primitives/) | `Uuid`, `TimeSpan`, `Ref`, `base64_encode`, engine identity | `core/src/*` · primitives |
| [ownership-and-raii](ownership-and-raii/) | Rust ownership, `Drop`, `Arc<T>` handles, teardown order | `core/src/lib.rs` · `Ref`; `app/src/lib.rs` · `wait_gpu_idle` |
| [logging](logging/) | `log_info!` / `log_warn!` / `log_error!`, subsystem-tagged stdout | `core/src/log.rs` · `log`, macros |
| [signals-and-slots](signals-and-slots/) | `SubscriberList<Args>`, `subscribe`, `SubscriptionId`, stop-propagation | `signal/src/lib.rs` |
| [json-gateway](json-gateway/) | typed JSON over `serde_json`, the decimal-string-`u64` wire encoding | `json/src/lib.rs` |
