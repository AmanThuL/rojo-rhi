# The first D3D12 run on hardware

**Status**: Accepted (2026-11-12)

The D3D12 backend had run on WARP alone ([the WARP evidence](2026-10-21-d3d12-warp-evidence.md)).
This note records its first run on a GPU, following [the hardware run
guide](../guides/d3d12-hardware-run.md): the floor, T1 under the debug layer and GPU-based
validation and in Release ([ADR 0009](../decisions/0009-conformance-and-valid-usage.md)), and the
markers of the API reference the run settles. It is correctness evidence on one GPU of one vendor.
Spike 1 is recorded in [its own note](2026-10-20-spike-1-d3d12-addr-dereference.md#results); the
T2 round has its own pre-registration ([the overhead note](2026-10-20-d3d12-overhead.md)).

## The machine

| Item | Value |
| --- | --- |
| GPU | NVIDIA GeForce RTX 3080 Ti (vendor `0x10DE`, device `0x2208`), driver 621.14 (32.0.16.2114) |
| CPU, OS | Intel Core i7-8700K; Windows 11 build 26300, Graphics Tools installed |
| Toolchain | MSVC 19.52.37029 (Visual Studio 18), xmake 3.1.2, Agility SDK 1.619.6, DXC 1.9.2607, Slang 2026.14.1 |
| Floor | resource binding tier 3, resource heap tier 2, tiled resources tier 4, `TypedUAVLoadAdditionalFormats`, enhanced barriers, `DynamicDepthBiasSupported`, shader model 6.9 |
| `OPTIONS19` | `MaxViewDescriptorHeapSize` 1,000,000; `MaxSamplerDescriptorHeapSize` 4,080 |
| View heap | 1,000,000 descriptors, `maxViews` 996,926 at a `framesInFlight` of 3 |

## T1

At 9d60f85, `main` with [ADR 0030](../decisions/0030-view-floor-is-tier-3-heap-less-reservation.md)
and the sRGB filtering bound applied, both merged since, with the hardware adapter and no overlay
running:

| Entry | Debug, GPU-based validation | Release |
| --- | --- | --- |
| `[d3d12]` | 262 cases: 260 passed, 2 skipped | — |
| `abi` | 13 cases: 11 passed, 2 skipped | — |
| `contract` | 603 cases: 600 passed, 3 skipped | 603 cases: 501 passed, 102 skipped |
| `conformance` | 191 cases: 182 passed, 9 skipped, in five runs | 191 cases: 182 passed, 9 skipped |
| `imgui` | 15 cases, all passed | — |
| `death` | 557 cases: 553 passed, 2 skipped, 2 failed as expected | — |

Every skip is a device that does not report a format capability, a Debug-only validation case in
Release, a case that needs the debug layer, a Metal library case on a host without the Metal
toolchain, or a case its own text skips on purpose (a `maxCount` of 0 that a Debug build refuses,
`maxDrawIndirectCount` records that exceed `maxBufferSize`, a debug-layer abort by design). The two
expected failures are the death harness's own `[!shouldfail]` cases. The debug layer reported no
message of any severity in any conformance run. Each count is above the floor the guide sets (180,
255, 878 and 12): 191, 262, 898 and 15. One earlier Debug `contract` run hung with no CPU or GPU
activity after 48 minutes and was stopped; the cases passed one per process and in two later
complete runs, so the hang is not reproduced and its case is unknown.

`[precision]` returned the exact 0 on hardware in both builds, as [the WARP precision
note](2026-10-26-warp-precision-runtime.md) expected of a GPU. The GPU-based-validation reports of
issues #122 and #152, seen on WARP, did not occur in five conformance runs.

## What the run changed

- **The view floor.** The GPU reports a view heap of exactly tier 3's 1,000,000, which the backend
  refused because it added its 3,074 reserved slots on top. [ADR
  0030](../decisions/0030-view-floor-is-tier-3-heap-less-reservation.md) sets the floor at 996,926.
- **sRGB filtering.** The GPU's filtered sRGB means sit on a grid of 1/512, 0.00245 from the exact
  linear mean for one channel, outside the half step the format case allowed; the case and chapter
  15 now allow one 8-bit step.
- **The death entry's bound.** The 557 death cases take about ten minutes under GPU-based
  validation on hardware, so the entry's bound is thirty minutes.
- **The machine.** Without the Graphics Tools optional feature the debug layer is refused with
  `DXGI_ERROR_SDK_COMPONENT_MISSING`, and with RivaTuner Statistics Server running its `Present`
  hook keeps every swapchain alive, so a Debug `destroyDevice` aborts and a two-swapchain present
  removes the device. The guide's section 1 now requires the feature and no such overlay.

## The markers it settles

Items of [the WARP evidence](2026-10-21-d3d12-warp-evidence.md), numbered as there:

- **Item 1, `DynamicDepthBiasSupported`.** Reported, and the depth-bias cases pass. Chapters 02 and
  06 part 2 say NVIDIA's driver honours it; other vendors stay unverified.
- **Item 2, `OPTIONS19`.** 1,000,000 views and 4,080 samplers on this GPU; chapters 02 and 15 part 2
  record it, other shipping values stay unverified.
- **Item 3, resource heap tier 2.** Reported, and the buffer and texture cases pass; chapter 03
  part 2 records it.
- **Item 21, limits and formats.** The limit and format-table `[d3d12]` cases and every
  `FormatConformanceTests.cpp` case pass; chapter 15's guaranteed-format paragraph names the GPU.
- **Issue #90.** The layer-0 view case passes with no dimension-mismatch message.
- **Swapchain release (issue #139).** A native probe that destroys the window before the swapchain's
  last release, as a host may, reports no message and leaks no reference.

Items 7 and 17 still need a case, item 20 (DLSS and XeSS) a backend that integrates them, and the
capture item PIX on the machine; the pipeline cache's cross-adapter paths (#94) are unchecked.

## Evidence

`rojo-rhi-evidence/d3d12-floor-hw/2026-11-12/` holds the machine record, the device capabilities
(`caps.txt`), the view-heap case, the `[d3d12]` runs and the native probes with their sources;
`SHA256SUMS` SHA-256 `452b3cce4a8bcd826dd598bce31dd0d0803e693c4c4b493a85ef78a2fcb0bd94`.
`rojo-rhi-evidence/d3d12-t1/2026-11-12/` holds every T1 log, including the runs before each fix;
`SHA256SUMS` SHA-256 `31f99b642b49d8e0774f594531c7c7ebad48051d95f668892be70d38c8f7537c`.
