+++
title = 'Logging'
weight = 5
+++

# Logging

Logging is the act of writing a tagged diagnostic line to a stream so a running program reports
what it is doing. Saffron's logging is a handful of free functions in `Saffron.Core` that print
to stdout: no logger object, no sinks, and one filter (the Vulkan messenger's noise filter,
below).

This is enough for an engine that does most of its real diagnosis elsewhere — through Vulkan
validation layers and the [`sa` control plane](../../tooling-and-control/control-plane-architecture/).

## How it works

Every line has the shape

```
[saffron:<subsystem>] <message>
[saffron:<subsystem>] warn: <message>
[saffron:<subsystem>] error: <message>
```

where `subsystem` is the engine module that spoke (`rendering`, `scene`, `assets`, `control`, …).
The prefix is the whole protocol: `grep '\[saffron'` finds all engine output, and the tag says
which module to look at. There is no level to mute and no timestamp.

The base emit takes the tag explicitly; the three leveled wrappers derive it from the caller:

```cpp
void log(LogLevel level, std::string_view subsystem, std::string_view message);

void logInfo(std::string_view message,
             std::source_location location = std::source_location::current());
// logWarn / logError likewise
```

The defaulted `std::source_location` is evaluated at the call site, so the wrapper maps the
caller's path under `source/saffron/` to its module directory — call sites never spell a tag.
Only a component speaking on someone else's behalf passes one explicitly, which is exactly what
the Vulkan debug messenger does.

## The Vulkan messenger funnels here too

Validation-layer and loader messages arrive through a debug callback (`onVulkanMessage` in
`Saffron.Rendering`) and come out as one line in the same format, tagged `vulkan`:

```
[saffron:vulkan] error: [validation] VUID-vkCmdDraw-…: <what the layer said>
[saffron:vulkan] warn: [performance] <id>: <what the layer said>
```

The bracketed word is the messenger's message type (`validation`, `performance`, `general`).
The e2e harness's `validationErrors()` oracle greps for `[saffron:vulkan] error: [validation]` —
a test run is clean when no such line appears.

Two classes of messages are dropped because they are never actionable here:

- **General-type warnings** — Vulkan loader chatter, such as an ICD that fails to initialize
  and is skipped. General-type *errors* still come through.
- **`OutputNotConsumed` performance warnings** — depth-only and sky pipelines bind the full
  mesh vertex layout by design, so "vertex attribute not consumed" fires on every such PSO.

Set `SAFFRON_VK_VERBOSE=1` to bypass the filter and see everything the messenger emits.

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

A heavier system — per-category mute switches, async sinks, structured fields — would be
infrastructure the engine does not currently need. Validation layers catch the Vulkan mistakes,
the control plane makes the running editor inspectable from the CLI, and tagged stdout covers
the rest. The free functions are a seam: every call site already funnels through `log`.

## In the code

| What | File | Symbols |
|---|---|---|
| The functions | `core.cppm` | `log`, `LogLevel`, `logInfo`, `logWarn`, `logError`, `logSubsystem` |
| The Vulkan funnel | `renderer.cppm` | `onVulkanMessage` |
| A real error path | `app.cppm` | `run` — `logError` on a failed `Result` |
| The e2e oracle | `tests/e2e/harness.ts` | `Engine.validationErrors` |

## Related

- [Error handling](../error-handling/) — where `logError` reports a failed `Result`
- [Control plane architecture](../../tooling-and-control/control-plane-architecture/) — the richer way to inspect a running editor
