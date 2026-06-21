+++
title = 'Logging'
weight = 5
+++

# Logging

Logging is the act of writing a tagged diagnostic line to a stream so a running program reports
what it is doing. Saffron's logging is one free function plus three macros in `saffron-core` that
print to stdout: no logger object, no sinks, and one filter (the Vulkan messenger's noise filter,
below).

This is enough for an engine that does most of its real diagnosis elsewhere — through Vulkan
validation layers and the [`sa` control plane](../../tooling-and-control/control-plane-architecture/).

## The frozen line format

Every line has the shape

```
[saffron:<subsystem>] <message>
[saffron:<subsystem>] warn: <message>
[saffron:<subsystem>] error: <message>
```

where `subsystem` is the crate that spoke (`rendering`, `scene`, `assets`, `control`, …). The
prefix is the whole protocol: `grep '\[saffron'` finds all engine output, and the tag says which
area to look at. There is no level to mute and no timestamp. The format is grep-relied-upon — the
validation-clean-log gate parses it — so it is frozen.

## One function, three macros

The base emit takes the subsystem tag explicitly; the three macros derive it from the caller:

```rust
pub fn log(level: LogLevel, subsystem: &str, message: &str);

pub fn subsystem_of(module_path: &str) -> &str; // saffron_rendering::… → "rendering"
```

`subsystem_of` strips the leading `saffron_<area>` crate segment of the caller's `module_path!()`
down to `<area>` (a path that doesn't start with `saffron_` falls back to `engine`). The
`log_info!` / `log_warn!` / `log_error!` macros pass `module_path!()` through it, so call sites
never spell a tag:

```rust
log_info!("loaded {n} meshes");
log_error!("failed to create window: {err}");
```

Only a component speaking on someone else's behalf passes a subsystem explicitly — which is exactly
what the Vulkan debug messenger does.

## The Vulkan messenger funnels here too

Validation-layer and loader messages arrive through `ash`'s debug-utils callback (`debug_callback`
in `saffron-rendering`'s `device.rs`) and come out as one line in the same format, tagged `vulkan`.
A separate process-wide counter, `validation_issue_count`, tallies validation/performance messages
at warning-or-error severity; the validation-clean smoke reads it before and after a render and
asserts it did not move — that is the e2e oracle, parsed off the frozen log line.

> [!NOTE]
> The `[saffron:vulkan] error:` lines and the `validation_issue_count` tally are the two faces of
> the same gate. A run is clean when no validation error line appears and the counter does not move.

## Formatting at the call site

The macros forward to `format!`, so message formatting happens at the call site and the logging
surface stays a single `log` seam. It pairs naturally with a `Result` check on the failure path —
propagate the error, or log it and bail.

## In the code

| What | File | Symbols |
|---|---|---|
| The function + level | `engine/crates/core/src/log.rs` | `log`, `LogLevel` |
| Subsystem derivation | `engine/crates/core/src/log.rs` | `subsystem_of` |
| The macros | `engine/crates/core/src/log.rs` | `log_info!`, `log_warn!`, `log_error!` |
| The Vulkan funnel | `engine/crates/rendering/src/device.rs` | `debug_callback`, `validation_issue_count` |

## Related

- [Error handling](../error-handling/) — where a failed `Result` is logged before bailing
- [Control plane architecture](../../tooling-and-control/control-plane-architecture/) — the richer way to inspect a running editor
