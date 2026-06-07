+++
title = 'Reference'
weight = 30
bookCollapseSection = true
+++

# Reference

Reference pages catalogue the engine's exact surface: type signatures, data shapes, enum values, and the full control command list. Each page covers one module boundary and is meant for lookup, not reading end to end.

## Pages

| Page | Covers |
|---|---|
| `core-types` | `u8…f64`, `Result<T>`, `Err`, `Ref<T>`, `Uuid`, `TimeSpan` |
| `event-signals` | `SubscriberList`, `SubscriptionId`, the `Window` signals |
| `render-graph-api` | `RgUsage`, `RgPass`, `RgAttachment`, `importImage`/`importBuffer`/`addPass` |
| `renderer-api` | `newRenderer`, `beginFrame`/`endFrame`, `submit`, `requestMeshPipeline` |
| `components` | every built-in component and its fields |
| `shader-descriptor-sets` | sets 0–4: bindless, lighting, instances, IBL, screen-space |
| `control-commands` | every registered `se` command, its params and output |
