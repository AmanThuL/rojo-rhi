# The D3D12 hardware markers the new cases settle

**Status**: Accepted (2026-11-13)

[The first hardware run](2026-11-12-d3d12-hardware-run.md) settled the markers of the API
reference that its T1 run could observe. Items 7 and 17 of [the WARP
evidence](2026-10-21-d3d12-warp-evidence.md) needed a case first, items 13 and 18 and the FSR
markers A and B had none, and the FSR input scale range of `Capabilities::upscalers` was taken from
FidelityFX's documentation (issue #22). This note records the cases added for them and their run
on the same GPU, following section 4 of [the hardware run guide](../guides/d3d12-hardware-run.md).
It is correctness evidence on one GPU of one vendor, never a measurement ([ADR
0014](../decisions/0014-overhead-measurement.md)).

## The run

The machine of [the hardware run](2026-11-12-d3d12-hardware-run.md#the-machine): an NVIDIA GeForce
RTX 3080 Ti on driver 621.14 (32.0.16.2114), Windows 11 build 26300, with the same toolchain, the
hardware adapter and no overlay running. The entries ran with this change's cases on `main` at
6dfeb6b, in Debug under the debug layer and GPU-based validation and in Release; the new cases
passed again under GPU-based validation after the change moved onto `main` at 6192341:

| Entry | Debug, GPU-based validation | Release |
| --- | --- | --- |
| `[conformance]` | 200 cases: 191 passed, 9 skipped | 200 cases: 191 passed, 9 skipped |
| `[d3d12]` | 263 cases: 261 passed, 2 skipped | — |
| `abi` | 13 cases: 11 passed, 2 skipped | — |

Every count is the hardware run's plus the new cases: nine `[conformance]` cases over its 191 and
one `[d3d12]` case over its 262. The debug layer and GPU-based validation reported no message in
any conformance run; the `[d3d12]` run printed one warning, STATE_CREATION 1317, which a pipeline
case of `D3D12PipelineTests.cpp` provokes with a `Min` alpha blend. The cases of items 13 and 17
fail, so they are not part of these counts: they wait in [pull request
#178](https://github.com/AmanThuL/rojo-rhi/pull/178), and their runs are recorded below. The Metal 4
run of the new `[conformance]` cases is still owed.

## The markers

Numbered as in [the WARP evidence](2026-10-21-d3d12-warp-evidence.md#markers):

- **Item 7, `NonUniformResourceIndex` on a device.** Six `[conformance]` cases, "AbiNonUniform
  loadMain loads through a buffer that differs between the threads of a wave" and its siblings for
  `storeMain`, `atomicMain`, `textureMain`, `wTextureMain` and `samplerMain`, run the kernels of
  `Shaders/Tests/AbiNonUniform.slang` that `Tools/check_dxil.py` checks, as one group of 64
  threads: two waves of 32 on this GPU, each holding even and odd threads, which index different
  buffers, views and samplers. Every thread loaded, stored, added to, fetched and sampled through
  its own descriptor. Whether a divergent index reaches the right descriptor is the driver's, so
  chapter 05 part 2 says NVIDIA's driver does it and keeps the marker for other vendors.
- **Item 13, depth bias across `SetPipelineState`.** The `[d3d12]` case "the native list keeps a
  dynamic depth bias across SetPipelineState when nothing re-issues it" sets a bias of 4 on a
  `depthBias` pipeline and draws, then calls the native `SetPipelineState` with another `depthBias`
  pipeline's PSO, and then with a PSO without the dynamic flag followed by a third `depthBias` PSO,
  and draws after each without `RSSetDepthBias`. The first draw stores 0.5 + 4 × 2^-24; both later
  draws store 0.5 exactly, the zero bias each PSO is created with, in Debug and in Release. The
  driver does not keep a dynamic depth bias across `SetPipelineState`, even between two PSOs that
  carry the flag. The case expected it kept and fails, so it is not merged. The backend re-issues
  the bias at each `setPipeline` of a `depthBias` pipeline, which this result makes necessary on
  NVIDIA; the owner decides the reference's wording, and the marker stays.
- **Item 17, a barrier from `Undefined` waits for its `SyncBefore` stages.** The `[conformance]`
  case "a barrier from Undefined waits for the writes of its source stage before a later write"
  fills a 512 × 512 `RGBA8Unorm` storage texture red after 2^20 spin steps per thread, discards it
  from `Undefined` with the compute stage as its source and no access, fills it green with no spin,
  and reads it back, four rounds in a process. In Debug under GPU-based validation it passes. In
  Release it fails in all five processes, in three or four of the four rounds of each, with 74,720
  to 158,784 of the 262,144 texels left red. Hidden controls, run in Release on the same
  texture: with no barrier the fills race, so the case can fail; a barrier that stays in
  `ShaderWrite` and names `ShaderWrite` as its source access orders them, and so does one with no
  source access that changes no layout; a discard from `Undefined` whose source is every stage does
  not. In Debug, GPU-based validation serializes the two dispatches, so even no barrier passes. The
  backend lowers the discard to `LayoutBefore` `UNDEFINED`, `AccessBefore` `NO_ACCESS` and
  `D3D12_TEXTURE_BARRIER_FLAG_DISCARD` with the source stages in `SyncBefore`; on this driver that
  barrier does not wait for them. It is a backend defect under the guide's rule, the owner decides
  the lowering, and the marker of chapter 09 part 2 stays.
- **Item 18, a discard from `Undefined` on another queue.** The `[conformance]` case "the Compute
  queue discards a render target the Graphics queue left in ColorAttachment and writes it" clears
  a render target on the Graphics queue and leaves it in `ColorAttachment`, a layout the Compute
  queue does not accept; the Compute queue's first barrier discards it from `Undefined`, a
  dispatch writes every texel and a copy reads them back, with the validation tracker on in Debug.
  Every texel holds the written value, and GPU-based validation, which enforces the layouts a queue
  accepts, reports nothing. Chapter 09 part 2 drops its marker.
- **A, a `D32FloatS8` depth to FSR.** The `[conformance]` case "a D32FloatS8 depth upscales as the
  same D32Float depth does, frame after frame" upscales four frames of a checkerboard whose phase
  flips each frame, over a depth drawn at 0.5 in one half and cleared to 0.8 in the other and a
  motion that differs between the halves, once with each depth format. The outputs are equal byte
  for byte in every frame, and the same frames with the other half nearer differ in three of
  them, so the comparison sees the depth. GPU-based validation reports nothing for the
  `R32_FLOAT_X8X24_TYPELESS` view FidelityFX makes. Chapter 12 drops the marker.
- **B, the `jitterOffset` sign.** The `[conformance]` case "a positive jitter moves the
  reconstructed edge of a still frame left and up" upscales a frame that is dark before input texel
  32 and light from it, with a jitter of -0.375, 0 and +0.375 input texels along the edge's axis.
  The edge lies at 64.77, 64.00 and 63.30 output texels across a vertical edge, and the same across
  a horizontal one, against 64.75, 64 and 63.25: FidelityFX takes `jitterX` and `jitterY` as the
  offset by which the projection moved the scene, y down, as chapter 12 and the upscaling example
  pass them. Chapter 12 drops the marker and states the sign.
- **The FSR input scale range.** The `[d3d12]` case "the FSR input scale range the device lists is
  the one an FSR context reports for its quality modes" queries a live FSR context with
  `ffxQueryDescUpscaleGetUpscaleRatioFromQualityMode` for each quality mode: 1, 1.5, 1.7, 2 and 3,
  so 1/3 and 1, the range the device lists. Chapter 12 drops the marker; the backend still lists
  the range without querying, and the case fails if a FidelityFX update changes it.

## After the run

Two follow-ups of the same day are recorded here; the run above stands as it was.

- **Item 13.** The probe case is not kept: it observes the driver, not RojoRHI, whose re-issue at
  each `setPipeline` of a `depthBias` pipeline its existing cases check. Chapter 06 part 2 now
  states that NVIDIA's driver drops the bias at `SetPipelineState`, also between two `depthBias`
  pipelines, and keeps other vendors unverified.
- **Item 17.** The lowering of a texture barrier from `Undefined` now adds, in the same `Barrier`
  call, a global barrier from the write accesses its source stages perform to the barrier's
  destination, as the Vulkan lowering widens `srcAccessMask`. Before settling on it, two variants
  were tried in Release on the same case: an execution-only global barrier in the same call still
  failed in all five processes (35,648 to 74,784 texels red), and one carrying `ShaderWrite` on
  both sides passed, in its own call and in the same call. With the change the case passes in all
  five Release processes and under GPU-based validation, and chapter 09 part 2 states the lowering
  and why. Evidence: `rojo-rhi-evidence/d3d12-discard-fix/2026-11-13/`, `SHA256SUMS` SHA-256
  `4bf6c5533c77c909949607da8e941a288e613fe6da37195ee0785b0dbbd1dfb5` (the Release runs at the
  fix's commit; the Debug runs under GPU-based validation at the same backend code before its
  rebase onto the cases above).

## Evidence

`rojo-rhi-evidence/d3d12-markers/2026-11-13/` holds the machine record, the Debug and Release
logs of the entries above, the new cases alone with their printed values, and the runs of the cases
of items 13 and 17 with the controls; `SHA256SUMS` SHA-256
`1acf8211ee390781885aca933ce83eb5f6c6929ac97b6d131ae2b7258d09ba3a`.
