+++
title = 'No-exceptions Vulkan-Hpp'
weight = 1
+++

# No-exceptions Vulkan-Hpp

Vulkan-Hpp is the official C++ binding for Vulkan, and its no-exceptions mode is a compile-time switch that makes every fallible call return a result code rather than throw. The renderer uses this mode throughout. Errors stay values rather than throws, so every Vulkan failure becomes a [`Result<T>`](../../core-and-conventions/error-handling/) at the call site, exactly like a file-parse or JSON error.

## How it works

Three defines at the top of every renderer translation unit select the mode:

```cpp
#define VULKAN_HPP_NO_EXCEPTIONS
#define VULKAN_HPP_NO_SMART_HANDLE
#include <vulkan/vulkan.hpp>
```

`VULKAN_HPP_NO_EXCEPTIONS` changes what a call returns. A value-producing call gives back a `vk::ResultValue<T>` — a `{ result, value }` pair — instead of throwing, and a void-producing call returns a bare `vk::Result`. `VULKAN_HPP_NO_SMART_HANDLE` turns off the RAII `vk::UniqueXxx` handles, which depend on the same exception machinery; the engine owns handles with its own [move-only wrappers](../meta-layer-resources/) instead.

## The checked() conversion

Two `checked()` overloads collapse a per-call result check into one `Result`-returning expression:

```cpp
template <typename T>
auto checked(vk::ResultValue<T> rv, std::string_view what) -> Result<T>
{
    if (rv.result != vk::Result::eSuccess)
        return Err(std::format("{}: {}", what, vk::to_string(rv.result)));
    return std::move(rv.value);
}

auto checked(vk::Result result, std::string_view what) -> Result<void>;
```

On success the value comes out; any other result becomes an `Err` whose message is the `what` label plus `vk::to_string` of the code. A fallible Vulkan call then reads like every other fallible call in the engine, and the message a caller sees is a chain of `what` labels — enough to locate a failure without an error enum.

## Where checked() doesn't apply

Command recording (`beginRendering`, `bindPipeline`, `dispatch`) returns void and never fails. A handful of calls whose status the engine intentionally ignores — `waitForFences`, `resetFences`, `commandBuffer.reset`, `device.waitIdle` — are wrapped in `static_cast<void>(...)` so the discard is explicit, not accidental.

A few flows need the result value even on a non-success code. `acquireNextImageKHR` and `presentKHR` return `eErrorOutOfDateKHR` or `eSuboptimalKHR`, which mean "rebuild the swapchain," not "fail." Those sites branch on the result directly (see [frame sync](../frame-sync-and-resize/)).

## Why not vk::raii

`vk::raii` is the idiomatic way to own Vulkan handles, but it throws, so a no-exceptions engine cannot adopt it. Turning exceptions off makes handle ownership the engine's job, paid by the move-only wrappers that free their `vk::` handles in their destructors. See [meta-layer resources](../meta-layer-resources/).

## In the code

| What | File | Symbols |
|---|---|---|
| No-exceptions defines | `renderer.cppm` | `VULKAN_HPP_NO_EXCEPTIONS`, `VULKAN_HPP_NO_SMART_HANDLE` |
| The conversion | `renderer_detail.cppm` | `checked` (value + void) |
| Result / Err | `core.cppm` | `Result`, `Err` |
| Acquire/present (result-not-failure) | `renderer.cppm` | `beginFrame`, `endFrame` |

## Related

- [Error handling](../../core-and-conventions/error-handling/) — the `Result<T>` scheme `checked` feeds into
- [Meta-layer resources](../meta-layer-resources/) — what owns handles now that smart handles are off
- [Frame sync](../frame-sync-and-resize/) — where acquire/present results are not errors
