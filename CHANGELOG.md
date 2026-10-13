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
