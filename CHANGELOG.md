# Changelog

## 0.1.0 (unreleased)

- The v2 interface begins beside the frozen v1 interface
  ([ADR 0017](docs/decisions/0017-v1-frozen-under-legacy.md)).
- The shader ABI module `Shaders/RojoRHI/RojoRHI.slang`, with compile-only conformance shaders for
  MSL, SPIR-V and HLSL ([ADR 0018](docs/decisions/0018-root-srv-and-reserved-bindings.md)).
- The public headers `<rojoRHI/Addr.h>` and `<rojoRHI/IndirectArgs.h>`.
- A build-time check of shared struct layouts against Slang reflection on every target.
- The Null backend: devices with frame slots and deferred destruction, generation-checked handles,
  capabilities, format capabilities, limits and the message callback, over a backend-neutral core
  ([architecture](docs/architecture/null-backend.md)).
- The Debug-tier `ROJORHI_VU` check, death tests for the valid-usage rules of those entries and
  `Tools/check_vu_coverage.py`, which requires a death test or a pending entry for every rule.
- Buffers and addresses on the Null backend: `Device::createBuffer`, `address` and `cpuPointer`,
  with `Addr` arithmetic checked against `maxAddrOffset`.
- Heaps and placement: `Device::createHeap`, `placementRequirements` and placed buffers and
  textures, checked against the heap's size, alignment and memory class.
- Textures and views: `Device::createTexture` with initial data, `createView` and `ViewIndex`.
- Samplers and slot ranges: `Device::createSampler`, `reserveViews`, `reserveSamplers`,
  `writeView`, `writeSampler` and release of slots, with reuse deferred until the releasing frame
  retires.
- Queues and fences: `Device::queue`, `createFence`, `Queue::submit`, `completedValue` and `wait`;
  a Null submission completes inside the call.
- API reference changes in those chapters: `VU-rojo-buffer-0010` is retired in favour of
  `VU-rojo-buffer-0019`, which is limited to work recorded with an address and is checked by the
  validation tracker; `VU-rojo-texture-0039` (layer limit), `VU-rojo-texture-0040` (no initial data
  for `D32FloatS8`) and `VU-rojo-texture-0041` (a range starts inside the texture) are added, and
  the stride rules of `TextureData` state the size `bytes` must hold; `VU-rojo-queue-0004` and
  `VU-rojo-queue-0006` are retired in favour of `VU-rojo-queue-0007` and `VU-rojo-queue-0008`, which
  compare against the greatest value submitted; `VU-rojo-texture-0031` is a Debug-tier rule, and a
  released `IndexRange` equal to a later reservation of the same slots cannot be told apart.
- `Tools/check_vu_coverage.py` counts check sites per identifier, requires a death test for each,
  and rejects `ROJORHI_VU` under `Backends/`.
