+++
title = 'Logging'
weight = 5
+++

# Logging

Logging is the act of writing a tagged diagnostic line to a stream so a running program reports
what it is doing. Saffron's logging is three free functions in `Saffron.Core` that print to
stdout: no logger object, no severity filtering, no sinks.

This is enough for an engine that does most of its real diagnosis elsewhere — through Vulkan
validation layers and the [`se` control plane](../../tooling-and-control/control-plane-architecture/).

## How it works

The three functions cover the conventional severities:

```cpp
void logInfo(std::string_view m)  { std::println("[saffron] {}", m); }
void logWarn(std::string_view m)  { std::println("[saffron] warn: {}", m); }
void logError(std::string_view m) { std::println("[saffron] error: {}", m); }
```

Each takes a `std::string_view`, prefixes `[saffron]` — with `warn:` or `error:` for the
non-info levels — and prints with `std::println`. There is no level to mute and no timestamp.
The prefix is the whole protocol, which makes engine output trivially `grep`-able.

## Formatting at the call site

The functions take a finished string, so formatting happens at the call site with `std::format`.
That keeps the logging surface small and puts the message where its context lives. It pairs
naturally with a `Result` check:

```cpp
if (!windowResult)
{
    logError(std::format("failed to create window: {}", windowResult.error()));
    return 1;
}
```

This follows the shape on the [error-handling page](../error-handling/): a failed `Result`
carries a string message, and `logError` surfaces it before the function bails.

## Why it stays small

A heavier system — categories, levels, async sinks — would be infrastructure the engine does not
currently need. Validation layers catch the Vulkan mistakes, the control plane makes the running
editor inspectable from the CLI, and prefixed stdout covers the rest. The three functions are a
seam: if structured logging is ever wanted, the call sites already funnel through them.

## In the code

| What | File | Symbols |
|---|---|---|
| The functions | `core.cppm` | `logInfo`, `logWarn`, `logError` |
| A real error path | `app.cppm` | `run` — `logError` on a failed `Result` |

## Related

- [Error handling](../error-handling/) — where `logError` reports a failed `Result`
- [Control plane architecture](../../tooling-and-control/control-plane-architecture/) — the richer way to inspect a running editor
