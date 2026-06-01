# Phase 1 — Pre-flight: confirm 1.4 device support

**Status:** NOT STARTED

Decide whether requiring Vulkan 1.4 is safe on the hardware the engine actually runs on. This
is a gate, not a code change. Requiring 1.4 makes `vkb::InstanceBuilder` /
`PhysicalDeviceSelector` reject any device that reports only 1.3, so confirm support before
phase 2 touches anything.

## Steps

1. **Dev device (llvmpipe).** Already known 1.4-conformant (Mesa software Vulkan 1.4), but
   confirm in the toolbox:
   ```sh
   toolbox run -c saffron-build bash -lc 'vulkaninfo | grep -iE "apiVersion|deviceName" | head'
   ```
   Expect `apiVersion` ≥ `1.4.x`.
2. **Real target GPU.** On whatever non-llvmpipe GPU the engine is meant to run on, check the
   reported `apiVersion` the same way (or read it from the renderer's existing device-info log
   on startup). Needs ≥ 1.4. If the driver is older, either update it or hold this plan.
3. **Decide.** Go if every target device reports ≥ 1.4. Otherwise stop here and leave the
   engine at 1.3 — the bump buys nothing today and isn't worth dropping a device.

## Done when

- The dev device and every intended target GPU report Vulkan ≥ 1.4, recorded here, and the
  go/no-go is decided. On "go", proceed to phase 2.

## Notes

- This only gates the *core version floor*. The KHR ray-tracing stack is already negotiated as
  optional and is independent of the 1.3-vs-1.4 floor (`renderer.cppm` device bring-up,
  `rtSupported`), so it is not part of this decision.
