# Changelog

## 0.1.0 (unreleased)

- The v2 interface begins beside the frozen v1 interface
  ([ADR 0017](docs/decisions/0017-v1-frozen-under-legacy.md)).
- The shader ABI module `Shaders/RojoRHI/RojoRHI.slang`, with compile-only conformance shaders for
  MSL, SPIR-V and HLSL ([ADR 0018](docs/decisions/0018-root-srv-and-reserved-bindings.md)).
- The public types `Addr` and `IndirectArgs`, shared with the shader ABI module.
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
- Command lists and frame data on the Null backend: `Device::acquireCommandList`,
  `CommandList::end`, `uploadFrameData` and the list rules of `Queue::submit`, with per-slot list
  pools and a frame arena whose pages a list claims whole
  ([recording](docs/architecture/null-backend-recording.md)).
- Shader libraries and pipelines: `Device::loadShaderLibrary` over a text format with an ABI
  version, `createPipeline` for graphics, mesh and compute, and `PipelineCache` with
  `pipelineCacheData`, which round-trips through `DeviceDesc::pipelineCache`.
- Passes and dynamic state: `beginRenderPass`, `endRenderPass`, scopes, `setPipeline`, viewport,
  scissor, stencil reference, blend constants, topology and depth bias.
- Draws, dispatches and indirect commands: `setRoot`, `pushRoot`, `draw`, `drawIndexed`,
  `drawMeshTasks`, `dispatch` and the four indirect forms, with one address resolution site.
- Copies and clears: buffer, buffer-texture and texture copies, `fillBuffer` and `clearTexture`.
- Barriers: the Debug-tier rules of `CommandList::barrier`; the rules that follow resource state
  belong to the validation tracker.
- Recording takes no locks in Release builds except when a list claims a new frame-arena page, and
  a Debug-tier check may take the lock of the pool it reads
  ([ADR 0019](docs/decisions/0019-recording-locks.md), which supersedes that clause of ADR 0013);
  chapters 01 and 07 state the rule.
- API reference changes in those chapters: `VU-rojo-cmd-0063` is added (a command is recorded on a
  list that has not ended), and `acquireCommandList` says a list handle is valid until its frame
  retires; `VU-rojo-buffer-0018` is retired in favour of `VU-rojo-buffer-0020`, the recording-time
  form, whose limits chapter 03 states; frame data may serve as indirect arguments;
  `VU-rojo-pipeline-0014` is retired and becomes a requirement of `DeviceDesc::pipelineCache`;
  `VU-rojo-pipeline-0004`, `0005` and `0012` are retired in favour of `VU-rojo-format-0003`, and
  `VU-rojo-cmd-0006` is retired and re-issued as `VU-rojo-cmd-0071` without the resolve capability,
  which `VU-rojo-format-0003` owns; `VU-rojo-cmd-0017` is retired, since each command it named has
  its own outside-pass rule; `VU-rojo-indirect-0021` to `0023` require a viewport and scissor for
  the indirect draws; `VU-rojo-cmd-0045` is retired in favour of `VU-rojo-buffer-0001` and `0002`,
  and `VU-rojo-cmd-0047` and `0050` are retired and split into `VU-rojo-cmd-0064` and `0065`
  (bounds) and the buffer rules; `VU-rojo-cmd-0048` and `0051` are retired and split into
  `VU-rojo-cmd-0066` and `0068` (one sample, Debug tier) and `VU-rojo-cmd-0067` and `0069`
  (layouts, validation tracker); `VU-rojo-cmd-0062` is retired and re-issued as `VU-rojo-cmd-0070`,
  its layout half; `VU-rojo-sync-0012` is retired in favour of `VU-rojo-texture-0016`, `0017` and
  `0041`; chapter 07 counts color bytes per sample as the sum of each format's bytes per texel.
- Swapchains and presentation on the Null backend: `Device::createSwapchain`, `Swapchain::acquire`
  and `resize`, and `Queue::present`, with a fresh image record for each acquire
  ([edges](docs/architecture/null-backend-edges.md)).
- Upscalers: `Device::createUpscaler` for every kind the device reports and `CommandList::upscale`.
- Sparse resources: `Device::tileShape` over the D3D12 standard tile shapes and `Queue::mapTiles`,
  with the mapping kept per resource and checked by `VU-rojo-device-0009` at heap destruction.
- Diagnostics: `CommandList::declareAccess`, `Device::scopeTimings` (labels of scopes, render
  passes and upscales, with zero durations on Null), `beginCapture` and `endCapture` (Null has no
  capture tool and reports `Unsupported`) and `Device::counters`.
- Scope labels are copied into the frame arena while recording, so a list may claim a frame-arena
  page for a label as well as for `uploadFrameData`
  ([ADR 0020](docs/decisions/0020-recording-locks-scope-labels.md), which supersedes ADR 0019);
  chapters 01 and 07 state both causes.
- API reference changes in chapters 11 to 14 and 02: `VU-rojo-sparse-0011` is retired, since each
  element of a `mapTiles` call reports its own rule; `VU-rojo-sparse-0004` states a nonzero extent
  and a layer range inside the texture, and `VU-rojo-sparse-0005` a non-empty layer range inside
  the texture; `VU-rojo-present-0012` counts the resolve target of a color attachment as an
  allowed use; destroying a sparse texture or buffer unmaps all its tiles, which chapters 13 and
  02 state.
- The validation tracker, layout part: on a device created with `DeviceDesc::validation`, Debug
  builds log each recording entry's uses and barriers and replay them at `Queue::submit` under one
  device mutex, with one layout per texture subresource. It checks `VU-rojo-cmd-0007`, `0010`,
  `0054`, `0067`, `0069` and `0070`, `VU-rojo-sync-0004`, `0006` and `0013`,
  `VU-rojo-present-0014` and `0018` and `VU-rojo-upscale-0016`, each reported under the most
  specific identifier it breaks
  ([ADR 0021](docs/decisions/0021-validation-tracker-replays-at-submission.md),
  [architecture](docs/architecture/null-backend-edges.md#the-validation-tracker)); Release
  recording is unchanged.
- API reference changes for the tracker: `VU-rojo-sync-0022` is retired, since read literally it
  rejected a chain of transitions that `VU-rojo-sync-0013` permits; the claim that the tracker
  checks heap-slot typing is removed from chapters 01, 02, 04 and 14, since no rule states it and a
  shader's reads through a heap index are invisible to the CPU; chapter 12 states that `upscale`
  reads and writes mip level 0 of array layer 0 of each texture.
- The validation tracker, hazard, aliasing and lifetime part: on a validating Debug device the
  Null backend checks same-queue hazards and barrier coverage (`VU-rojo-sync-0021` and
  `VU-rojo-sync-0024`), indirect reads of argument and count bytes (`VU-rojo-indirect-0005`,
  `0011`, `0017` and `0020`), placed resources that alias memory (`VU-rojo-sync-0009` and `0017`)
  and the references of unsubmitted lists (`VU-rojo-abi-0003`, `buffer-0019`, `device-0007` and
  `present-0019`)
  ([architecture](docs/architecture/validation-tracker.md)).
- `VU-rojo-sync-0023` is retired for `VU-rojo-sync-0024`: a source covers a use by its stages and
  the use's write accesses, so reads and a barrier from `Undefined` need no source access, a
  destination covers a use by its stages and, after a write, its accesses, a layout transition is
  a write, and a later barrier whose source stage shares a stage with the destination stage of an
  earlier barrier that orders a use extends that execution chain, for barriers and transitions
  alike.
- Chapter 09 states how a barrier from `Undefined` lowers: Vulkan widens `srcAccessMask` to the
  write accesses of its source stages, and D3D12 pairs `D3D12_BARRIER_ACCESS_NO_ACCESS` with the
  source stages, which is unverified.
- The validation tracker, cross-queue part: on a validating Debug device the Null backend orders
  the queues with vector clocks over fences, the host clock and `beginFrame`, so a fence wait is a
  full dependency and a pair the later use's clock covers needs no barrier. It checks hazards
  between queues (`VU-rojo-queue-0005`, transitions and aliased heap bytes included), the layout
  each queue sees and its handoff (`VU-rojo-sync-0025`), `VU-rojo-present-0018` from the graphics
  queue's view, placed-texture transitions and discards as heap writes, frame joins
  (`VU-rojo-device-0006`) and tile-mapping order (`VU-rojo-sparse-0013`)
  ([architecture](docs/architecture/validation-tracker-queues.md)).
- `VU-rojo-sync-0018` is retired for `VU-rojo-sync-0025`: a discard from `Undefined` on the
  acquiring queue needs no accepted layout.
- API reference clarifications that keep their identifiers: ordering through an earlier submission
  on the same queue, and completion the CPU observed through a fence, count as orderings by a
  fence for `VU-rojo-queue-0005` and `VU-rojo-sparse-0013`, and uses of the same heap bytes by two
  placed resources are uses of the same memory; `VU-rojo-device-0006` counts the graphics-queue
  submissions of a frame together.
- Every valid-usage rule of the API reference now has a death test, and `Tools/vu_pending.txt` is
  empty.
- The v2 public interface is the single header `<rojoRHI/RHI.h>`, in sections by API-reference
  chapter; the 77 per-type headers it used to include, `<rojoRHI/Addr.h>` and
  `<rojoRHI/IndirectArgs.h>` among them, are gone, and an include of one of them becomes an include
  of `<rojoRHI/RHI.h>`. The declarations are unchanged; the header keeps only short notes, and the
  API reference holds the documentation. The v1 headers under `Legacy/` keep their own layout.
- ADR 0022 supersedes ADR 0015: the version macros are defined in `<rojoRHI/RHI.h>`, and the
  rest of the versioning decision is unchanged.
