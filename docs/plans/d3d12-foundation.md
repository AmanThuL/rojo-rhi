# D3D12 foundation (device and resources): implementation plan

**Status**: Proposed

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking. Per-task detail lives in the workspace
> `.superpowers/sdd/d3d12-foundation/` (`common.md`, `common-review.md`, `task-N-brief.md`); the
> orchestrator follows `.superpowers/p5/part-orchestrator.md`.

**Goal:** part P5.1b of the D3D12 backend: a real device on the Agility runtime (WARP on CI),
its capabilities, limits and format table, debug-layer routing and device-removal reports, the
two shader-visible descriptor heaps, the isolated `Addr` module and its heap directory, the
global root signature and command signatures, buffers, heaps, placement, textures, views and
samplers, staged initial data, queues, fences and frame pacing, with command lists that are the
core's state only. Exit: the 18 contract files of chapters 01–04, 10, 15 and 07's acquisition,
`end` and frame data pass on WARP under the debug layer and GPU-based validation, the first T1
cases pass there, and the Release contract entry passes.

**Architecture:** the first task builds the device and a skeleton in which every component has
its final header and a stub body, and moves each stubbed entry into the file of the task that
owns it. Later tasks then edit disjoint files and run in parallel waves on their own branches,
each verified by its own Windows CI run, and are cherry-picked onto the part branch in plan order.

**Tech stack:** C++23 (MSVC `-std:c++23preview`, `/W4 /WX`), Agility SDK 1.619.6
(`D3D12SDKVersion` 619), WARP 1.0.21, xmake, Catch2 3, Python 3, GitHub Actions `windows-latest`.

**Spec:** `.superpowers/p5/d3d12-backend-design.md` §3, §5.1–5.8, §5.15, §5.21–5.23, §7.1, §7.3,
§7.4, §8 row P5.1b (scope and exit, authoritative except where the user's split below moves
items out), §9, §10 (D2, D4, D5, D7, D8, D10–D12, D14, D15, D29); digests `spec-d3d12.md`,
`backend-shape.md` in the same directory; the API reference chapters 01–04, 03p2, 04p2, 10, 14,
15, 15p2 with their D3D12 mapping rows. AGENTS.md hard rules override everything.

## Global Constraints

- Floor (ADR 0010, checked at creation, never lowered): Windows 11 kernel line (`RtlGetVersion`
  build ≥ 22000), FL 12_2, SM ≥ 6.6, `ResourceBindingTier` 3, `TypedUAVLoadAdditionalFormats`,
  `EnhancedBarriersSupported`, `DynamicDepthBiasSupported`, and `ResourceHeapTier` ≥ 2 (D12).
  Failure is `Unsupported` naming the first missing item (02:72-76).
- Interfaces held: `ID3D12Device12`, `ID3D12GraphicsCommandList9` (D11). No legacy
  `ResourceBarrier` anywhere; every barrier is `ID3D12GraphicsCommandList7::Barrier`.
- `maxAddrOffset` = 2^29 − 1 (D9). A raw view is `DXGI_FORMAT_R32_TYPELESS`,
  `D3D12_BUFFER_SRV_FLAG_RAW` / `D3D12_BUFFER_UAV_FLAG_RAW`, `FirstElement` 0,
  `NumElements = min(Width, 2^29) / 4`; buffer `Width` is the size rounded up to a multiple of 4.
- `Addr.lo` = heap index of the buffer's raw SRV; `Addr.hi` = 32-bit byte offset; the UAV is at
  `lo + 1`; heap slot 0 is held for the device's life and holds a null SRV (03:177).
- View heap `H` = `min(OPTIONS19::MaxViewDescriptorHeapSize, 2^21)` when reported, else
  1,000,000; `maxViews` = `H`; below 1,000,000 is `Unsupported`. Sampler heap 2,048 (B11).
  Transient ring = `framesInFlight` × 1,024 slots with a CPU-only twin heap.
- Global root signature (version 1.1, flags `CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
  SAMPLER_HEAP_DIRECTLY_INDEXED`): param 0 root SRV `t0` space 0 `DATA_VOLATILE`; param 1 one
  32-bit root constant `b1` space 0. Kernel root signature: 4 root constants `b0` + the
  heap-indexed flag. Command signatures: draw [CONSTANT p1, DRAW] stride 20; indexed [CONSTANT,
  DRAW_INDEXED] 24; mesh [CONSTANT, DISPATCH_MESH] 16; dispatch [DISPATCH] 12, no root signature.
- Every check lives in `Source/Core`; no `ROJORHI_VU` under `Backends/`; library invariants use
  `ROJORHI_INTERNAL_ASSERT`. Release recording takes no lock except a frame-arena page claim
  (ADR 0020). RojoRHI creates no thread (ADR 0013) and reads no environment variable.
- Every native object gets `SetName` from its label (`rojorhi.` default), one UTF-16 conversion.
- No Windows, D3D12 or vendor type in `Include/`; nothing under `Legacy/`; no "v2" in any name.
- P4.6's code is merged (PR #70, 2026-10-20; no `m4o/*` branch remains), so its avoid list no
  longer binds and no task waits on it; only its timed round-2 run still uses this Mac (the
  timing guard of `common.md` stays, run before every local build or test). This part plans no `Source/Core` change; a task that finds one necessary
  reports NEEDS_CONTEXT and the orchestrator decides and records it. Outside this part's scope:
  `Tests/Metal4/`, `Benchmarks/`, `Tools/bench.py`, `docs/research/`, `README.md`, and the
  overview/testing/AGENTS.md D3D12 lines (P5.6, ledger entry 2). D9 is already on `main` (45a0190
  "rhi: lower the portable Addr reach to 2^29 - 1 bytes": 15p2:55 and 83-89, 02's Metal 4
  Floor-check cell, 03:166-171, `NullDevice.{h,cpp}`, `Metal4Capabilities.cpp`, `DeviceTests.cpp`,
  `NullCapabilitiesTests.cpp`, `NullBufferTests.cpp`, the Null and Metal 4 architecture pages,
  `CHANGELOG.md`); this part re-edits none of it. Every open item of the separate prework stream is
  excluded: D27 (`clearTexture`, PR #81), #25 (Windows tracker stress, PR #79), #73 and the DXIL
  non-uniform check (PR #80).
- Documents say "implemented, verified on WARP only, not on hardware" (D29); a WARP run is never
  called T1 acceptance (D2).

## Review Focus

1. **Several devices at once, and a device after another is destroyed** (DeviceTests "several
   devices coexist"; death children each make one): each device owns its factory and debug
   configuration; nothing process-wide leaks or double-enables. Owner: Task 1
   ("two devices coexist and a third is created after both are destroyed").
2. **Buffer sizes that are not a multiple of 4, and buffers above 512 MiB**: `Width` rounds up,
   the raw view stops at 2^27 elements, creation succeeds with no debug-layer message, and
   `address` still reaches the first 2^29 − 1 bytes. Owner: Task 8 ("a 3-byte buffer and a
   (2^29 + 4)-byte buffer get raw views with no debug-layer message").
3. **Device removal**: `GetCompletedValue` returns `UINT64_MAX` on a removed device; that value
   must never reach `core::completeFence`, and DRED is reported as `Error` before the entry fails.
   Owner: Task 3 (`reportRemoval`), Task 6 (the check in `completedValue`) and Task 10
   ("completedValue on a removed device reports removal and aborts", a `[d3d12][death]` case
   that needs both).
4. **A destroyed buffer's descriptors stay untouched until its frame retires**: a buffer created
   in the same or the next frame never receives the destroyed buffer's heap indices, and the
   directory no longer finds them after retirement. Owner: Task 8 ("a destroyed buffer's raw view
   pair is not reused before its frame retires").
5. **Concurrent creation**: buffers created from several threads at once (directory chunk
   allocation, slot allocator, stager mutex) get distinct heap indices that each decode to their
   own buffer. Owner: Task 8 ("buffers created on four threads decode to their own slots").

---

## Assumptions about P5.1a (`feat/d3d12-wiring`, PR #71) — the orchestrator verifies after merge

Read from its worktree and ledger on 2026-10-20 (HEAD 093d7ff); **verified 2026-10-20 against
`origin/main` 495c65c** (PR #71 rebase-merged as 14523f6, 93d15a8, 7831f97, 495c65c, on top of
D9's 45a0190). A1–A7 hold as written; A8 differs only in the page's length. Corrections are
folded in below.

- **A1 Options.** `rojorhi_backend=d3d12` (Windows only; Windows default stays `null`);
  `rojorhi_d3d12_adapter` = `hardware|warp`; `rojorhi_d3d12_debug` = `mode|off|layer|gbv`
  (`mode`, not `auto`: xmake reads `auto` as unset). The RojoRHI target defines
  `ROJORHI_D3D12_ADAPTER_{HARDWARE,WARP}` and `ROJORHI_D3D12_DEBUG_{MODE,OFF,LAYER,GBV}`
  privately, `WIN32_LEAN_AND_MEAN` and `NOMINMAX`, adds the Agility include as a system include
  and links `d3d12`, `dxgi`, `dxguid` publicly. `mode` = debug layer + DRED in Debug, nothing in
  Release.
- **A2 Runtime.** `rojorhi.d3d12_runtime` copies `D3D12/D3D12Core.dll`,
  `D3D12/d3d12SDKLayers.dll` and `d3d10warp.dll` beside `RojoRHITests` and `RojoRHILtoProbe`
  (FidelityFX only when present). ThirdParty: `agility/{include,bin}`, `warp/bin`, `dxc/bin/x64`.
- **A3 Stubs.** `Backends/D3D12/Source/` = `D3D12Common.{h,cpp}` (`d3d12::notImplemented`,
  `d3d12::unsupported`), `D3D12Device.cpp` (every `Device` entry, `createDevice` runs
  `core::validateDeviceDesc` then returns `Unsupported`), `D3D12CommandList.cpp`,
  `D3D12Queue.cpp`, `D3D12Swapchain.cpp`, `D3D12Addr.cpp`. No `DeviceImpl`, no Windows header.
- **A4 Tests.** `Tests/Support/D3D12/D3D12Backend.cpp` fails every seam but `runsOnGpu() = true`.
  `RojoRHITests` on `d3d12` compiles `Tests/D3D12/*.cpp` when present, adds
  `Backends/D3D12/Source` as a private include and the runtime rule; `d3d12_pending` lists all 55
  contract files (checked in `on_config`); entries `contract` =
  `[contract]~[death],[d3d12]~[death]` and `abi` (no `death` or `conformance` entry: a filter
  matching no case fails Catch2); `unreachable_after_stubs()` adds `/wd4702 /wd4722` to
  `RojoRHITests` and `RojoRHILtoProbe` on d3d12 Release. Beside it, `main`'s
  `warn_unreachable()` (351c892) adds clang's `-Wunreachable-code`/`-Wunreachable-code-return` to
  `RojoRHI`, `RojoRHITests` and the probes; MSVC ignores it, so it only bites shared code a Mac
  build compiles. `d3d12_pending` is still a packed table (Task 1 reformats it, B17).
- **A5 CI.** The `d3d12` job: setup, Debug configure `--rojorhi_d3d12_adapter=warp
  --rojorhi_d3d12_debug=gbv`, both probes (gate), `RojoRHITests` build, DXIL check, runtime-DLL
  check, `abi`, `contract`, toolchain record, clangd, compile database, layout check `--only
  Backends/D3D12 --only Tests/Support/D3D12 --only Tools/D3D12Probe`, Release configure
  `--rojorhi_d3d12_debug=mode`, build, `contract`, `RojoRHILtoProbe` build (no
  `check_lto_inline.py`). The macOS `format` job excludes the same directories. The probe gate
  requires the factory variant; the exports variant is informational.
- **A6 Agility (D4).** The stop gate chose the **device factory**:
  `ID3D12SDKConfiguration1::CreateDeviceFactory(619, "<absolute exe dir>\D3D12\")` with
  app-local WARP; debug layer and DRED through `GetConfigurationInterface`; GBV confirmed by
  layer message 1016. The P5.0 run shows the classic exports also work; the C4 rule still picks
  the factory. Verified: the merged `d3d12-backend.md` (Agility loading) records the factory and
  names the exports only as a fallback the backend does not use, so no task adds export sources
  or D28's snippet.
- **A7 Gate findings.** WARP 1.0.21 on Server 2025 (build 26100): FL 12_2, SM 6.9, tier 3,
  `OPTIONS19` heap sizes 2,097,152, largest view heap created 2,097,152, hidden-window
  presentation works, a raw SRV of 2^27 + 1 elements raises no debug-layer message.
- **A8** Commit scope `d3d12:` exists. `docs/architecture/d3d12-backend.md` exists (150 lines:
  Build options, Pins and the runtime layout, Agility loading, The floor probe, Toolchain, Tests
  and CI). Toolchain: MSVC 14.51, xmake passes `-std:c++23preview`.

## Decisions for this part

- **B1 Entries of later parts** keep P5.1a's stubs (`Unsupported` for `Result` entries,
  `d3d12::notImplemented` otherwise). Entries the core serves whole call the core:
  `scopeTimings`, `beginCapture` (no capture hook, so `Unsupported`), `endCapture`, both
  `tileShape`, `destroy(Pipeline|ShaderLibrary|Upscaler)`. `DeviceDesc::enableCapture` and
  `pipelineCache` are ignored until P5.4 and P5.3.
- **B2 Skeleton first.** Task 1 creates every component header of the table below with its final
  interface and stub bodies, defines `DeviceImpl` with every member, writes `createDevice` and
  `destroyDevice` in their final order, and moves each stubbed entry into the file its owner task
  edits. Later tasks edit only the files they own, their new test files and their own lines of
  the pending lists.
- **B3 Lists are core state only** (P4.1 F2): acquisition, `end` and `uploadFrameData` work;
  a list encodes no native command, so `submit` executes no command list. Allocators and native
  lists arrive with recording (P5.2).
- **B4 Submission.** `submitLists`; under `lockTracker`: `checkSubmit`, `replayLists`,
  `recordSignals(…, SignalCompletion::Later)`, one `Wait` per wait value, one `Signal` per signal
  value, then the queue's progress signal (B9); `releaseSubmitted` after the lock. `completedValue`
  reads `GetCompletedValue` and calls `completeFence` first; `Device::wait` runs `checkWait`, waits
  natively in slices of at most 10 s (timeout rounded up to milliseconds, event per call),
  `completeFence`, `checkWait` again (P4.1 F3).
- **B5 Native payloads.** `BufferRecord.native` = `d3d12::BufferPayload*` (owns one reference on
  its resource, the mapped pointer, its view indices; deleted by the retire hook);
  `HeapRecord.native` = `ID3D12Heap*`; `TextureRecord.native` = `d3d12::TexturePayload*`;
  `FenceRecord.native` = `ID3D12Fence*`; a view slot holds its texture's `ID3D12Resource*` with a
  reference of its own (the `view` hook releases it); a sampler slot holds null. Each hook
  tolerates a null payload.
- **B6 One view allocator (D8).** Slot 0: `core.views().allocate(H, 0)` at creation, never
  released. Buffer views: `allocateRange(H, 1 or 2, 0)` (a range even for one, so their release
  is not counted as a view retirement), taken before `core::addBuffer`; `Readback` buffers take
  none. The buffer retire hook clears the directory entry, then `releaseRange(base, count,
  releaseFrame)`, so a pair is reused only after a later frame retires. The ring is one
  `allocateRange` at creation. Owner word 0 for every backend slot.
- **B7 Heap directory (D10).** `d3d12::HeapDirectory` wraps a `core::NativeTable` of capacity `H`
  whose entry for index `x` is the `BufferPayload*` of the buffer whose SRV is `x` (§5.6's
  `RawBufferEntry` is the payload itself). `publish`/`clear` take a leaf mutex (creation and retire
  hooks only); `find` is one acquire load. Published before the handle is returned.
- **B8 Arena pages** are `Upload` buffers with one raw SRV, labelled `rojorhi.frame-arena`;
  `ArenaPage.address` = 0. `uploadFrameData` returns `Addr{page payload's viewIndex, offset}`,
  reading the payload with `pool<Buffer>().native(page)` (lock-free).
- **B9 Fence lifetime** follows Metal 4: each queue owns a progress fence signalled after a
  compute or copy submission that no frame joins; a destroyed fence's `ID3D12Fence` is kept until
  every queue's progress passes the point it was destroyed at.
- **B10 Honest capabilities** (P4.1 F12): `placementSparse` false and no tile extents,
  `upscalers` empty, `conservativeRaster` false, no capture hook, until the parts that implement
  them; `meshTasksIndirect` true (02:168); `uploadInDeviceMemory` = `GPUUploadHeapSupported`;
  `floorName` = `"FL 12_2, SM 6.<n>"` with the highest SM; format table with every sparse bit
  clear.
- **B11 Heap sizes.** View heap `H` as in Global Constraints; sampler heap always 2,048
  (the 15p2 D3D12 value), so `maxSamplers` = 2,048.
- **B12 Messages (§5.21).** InfoQueue1 callback when the layer is on: `INFO`/`MESSAGE` → `Info`,
  `WARNING` → `Warning`, `ERROR`/`CORRUPTION` → `Error`, then abort after an `ERROR` or
  `CORRUPTION`. The filtered-ID list in `D3D12Debug.cpp` starts empty; each entry carries a
  one-line reason. On `DXGI_ERROR_DEVICE_REMOVED`/`_RESET`/`_HUNG` the backend reports the
  removal reason, DRED breadcrumbs and page-fault data as `Error`, then fails as the entry's
  contract says (`Result` error, else abort).
- **B13 Factory per device.** Each `DeviceImpl` owns its `ID3D12DeviceFactory`, DXGI factory,
  adapter and device. In Debug builds `destroyDevice` asserts the device's last `Release` returns
  0; if CI shows a reference held outside RojoRHI (named from the layer's live-object report),
  the assertion becomes a Debug warning naming the count and the decision is recorded here.
- **B14 Counting.** `descriptorWrites` +1 per `Create*View`/`CreateSampler` the backend issues
  (raw views, slot 0's null SRV, RTVs/DSVs, the core's own counts for caller views and samplers
  stay the core's); `nativeCreateCalls` +1 per staging buffer and per internal object made after
  creation; `nativeSubmitCalls` +1 per `ExecuteCommandLists`, `Wait` and `Signal`;
  `nativeHostCalls` +1 per `GetCompletedValue`, `SetEventOnCompletion` and `Map`.
- **B15 Tests.** `Tests/D3D12/*.cpp`, tag `[d3d12]`, may include backend headers and reach
  `DeviceImpl::of(device)`; native readbacks go through `Tests/Support/D3D12/D3D12Readback.h`.
  Contract cases are never edited in this part (D9's `DeviceTests.cpp` change is already on `main`).
- **B16 Conformance pending by file.** `d3d12_conformance_pending` lists the
  `Tests/Conformance/` files a `d3d12` build leaves out, the mechanism of `d3d12_pending`
  (compile-time, checked in `on_config`). It departs from §7.4's case-name list: a 150-name
  runargs filter is unmaintainable and invites splitting files. A later part that needs to hold
  back one case of a file that otherwise passes adds its name to the entry's runargs then.
- **B17 Pending lists hold one file name per line**, so parallel tasks' removals cherry-pick
  with at most a trivial conflict, which the orchestrator resolves as the union of removals.
- **B18 Parallel waves.** Each task after Task 1 runs in its own worktree
  `../rojo-rhi-worktrees/p5-d3d12-foundation-tN` on branch `d3d12f/tN-<slug>` from the part head
  of the wave's start, with a draft PR whose base is `feat/d3d12-foundation` (so `pull_request`
  CI runs); the orchestrator may cancel that PR's macOS `legacy`/`metal4` jobs to save runners.
  After green CI and review, the commit (fixups squashed) is cherry-picked onto the part branch
  in task order, the task PR is closed unmerged, and the part PR's CI must be green before the
  next wave starts.
- **B19 No local syntax aid** (P5.1a W8 kept): Agility headers are not on the Mac and MinGW's
  headers and warnings diverge from MSVC's. Revisit only if one task spends more than three CI
  rounds on compile errors alone; record the choice here.
- **B20 D9 dependency (resolved).** D9 merged as 45a0190 before P5.1a, so `DeviceTests.cpp`
  already asserts `maxAddrOffset ≥ 2^29 − 1`, the value the backend reports. Task 10 frees it
  with the other 17 files, and Task 11's 15p2 note has no condition left.
- **B21 Memory.** `Gpu` → `DEFAULT`; `Upload` → `GPU_UPLOAD` when reported, else `UPLOAD`;
  `Readback` → `READBACK`. Committed: `CreateCommittedResource3` (buffers `LAYOUT_UNDEFINED`;
  textures `UNDEFINED`, or `COPY_DEST` with initial data); placed: `CreatePlacedResource2`. Heaps:
  `ALLOW_ALL_BUFFERS_AND_TEXTURES`, alignment 4 MiB (`D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT`)
  for `Gpu` so multisample textures place, 64 KiB otherwise. `Upload`/`Readback` buffers are
  mapped once at creation. `maxBufferSize` = `DedicatedVideoMemory`, or `SharedSystemMemory`
  when that is 0, capped at 2^`MaxGPUVirtualAddressBitsPerResource`.
- **B22 Internal labels.** Queues `rojorhi.queue.graphics|compute|copy`, upload queue
  `rojorhi.upload`, frame fence `rojorhi.frame`, progress fences
  `rojorhi.queue.<type>.progress`, heaps `rojorhi.views`, `rojorhi.samplers`,
  `rojorhi.transient`, root signatures `rojorhi.root`, `rojorhi.root.kernel`, command signatures
  `rojorhi.signature.{draw,indexed,mesh,dispatch}`, staging buffers `rojorhi.staging`,
  RTV/DSV heaps `<texture label>.rtv|dsv`.
- **B23 Release stub flags (issue #83).** #83 tracks both stub-era allowances. Task 10 restores
  `check_lto_inline.py` on the d3d12 job (`Device::capabilities` is real after Task 2). The
  `/wd4702 /wd4722` flags stay while any entry the Release executables reach still aborts (the
  P5.1a handoff expects that until P5.4); Task 10 tries the build without them once and removes
  `unreachable_after_stubs()` only if it is warning-free, else keeps it and the PR body lists the
  sites. The PR body says "Refs #83" (not "Closes") unless both halves are done.
- **B24 RTV/DSV caches** (§5.5): a texture with `RenderTarget` or `DepthStencil` usage gets, at
  creation, one CPU-only heap with one RTV or DSV per (mip, layer), per (mip, depth slice) for 3D,
  immutable for its life.
- **B25 Parallel streams; whichever merges second rebases.** Open at verification: P5.3a
  pipelines (PR #85, `feat/d3d12-pipelines`, base `main`) adds `D3D12ShaderLibrary.{h,cpp}`,
  `Tests/D3D12/D3D12ShaderLibraryTests.cpp` and the same `Tests/D3D12` entries in both ci.yml
  layout lists as Task 1, and later edits `D3D12Device.{h,cpp}`, `xmake/targets.lua`,
  `xmake/slang.lua`, `Tools/shader_library.py`; prework #79 (new `tracker-stress` job in ci.yml),
  #80 (ci.yml `build` job's `check_dxil.py` step), #81; P5.4 edges-early #76 (`Source/Core`
  sparse/tracker, Metal 4 sparse: no file of ours). Collision risk with this part: the ci.yml
  `d3d12` and `format` jobs (#85, Tasks 1, 10; #79/#80 only as neighbouring hunks) and
  `xmake/targets.lua` (#85; Tasks 1, 2, 6, 8, 9, 10). The orchestrator rebases the part branch on
  `origin/main` before each wave; if #85 lands first, Task 1 keeps its layout-list lines out (they
  exist) and its skeleton leaves `D3D12ShaderLibrary.*` as merged.

---

## Component skeleton (B2): files, owners, interfaces

All in `Backends/D3D12/Source/`, helpers in `rojoRHI::d3d12`. "Stub" = Task 1's body.

| File | Owner | Produces (exact names; stub behaviour) |
|---|---|---|
| `D3D12Common.{h,cpp}` | T1 | `template<class T> class Com` (owning COM pointer: `get`, `put`, `->`, `reset`, `detach`, copy AddRefs); `Error hresultError(HRESULT, std::string_view what)`; `void setName(ID3D12Object*, std::string_view label)`; `bool waitForFence(ID3D12Fence*, uint64_t value, uint64_t timeoutNs, std::string_view what)`; `notImplemented`, `unsupported` (kept) |
| `D3D12Loader.{h,cpp}` | T1 | `struct Loaded { Com<ID3D12DeviceFactory> factory; Com<IDXGIFactory6> dxgi; Com<IDXGIAdapter1> adapter; Com<ID3D12Device12> device; bool layer; bool gpuValidation; }`; `Result<Loaded> loadDevice()` |
| `D3D12Device.{h,cpp}` | T1 | `DeviceImpl`, `QueueImpl`, `CommandListImpl`, `SwapchainImpl` (`of`, `wrap`); `createDevice`, `destroyDevice`, `coreOf`, `capabilities`, `formatCapabilities`, `counters`, `queue`, B1's core-served entries |
| `D3D12Capabilities.{h,cpp}` | T1 floor, T2 report | `Result<void> checkFloor(ID3D12Device12*)` (T1, real); `struct DeviceFeatures { uint32_t shaderModel; bool gpuUploadHeap; bool relaxedFormatCasting; uint32_t tiledResourcesTier; uint32_t conservativeRasterTier; bool copyQueueTimestamps; }`; `Result<void> reportCapabilities(DeviceImpl&)` (stub: returns success, sets nothing) |
| `D3D12Formats.{h,cpp}`, `D3D12Memory.{h,cpp}` | T2 (new) | `DXGI_FORMAT dxgiFormat(Format)`; `DXGI_FORMAT resourceFormat(Format, TextureUsage, bool relaxedCasting)`; `DXGI_FORMAT viewFormat(Format texture, Format view, TextureAspect, TextureUsage viewUsage)`; `DXGI_FORMAT dsvFormat(Format)`; `std::span<const DXGI_FORMAT> castableFormats(Format)`; `core::FormatTable queryFormatTable(ID3D12Device12*)`; `D3D12_HEAP_TYPE heapType(Memory, bool gpuUploadHeap)` |
| `D3D12Debug.{h,cpp}` | T3 | `class MessageRouter { void attach(ID3D12Device12*, bool layer); void detach(); }` (stub: no-op); `[[nodiscard]] bool deviceRemoved(HRESULT)`; `void reportRemoval(DeviceImpl&, std::string_view entry)` (stub: logs the entry); `Error failure(DeviceImpl&, HRESULT, std::string_view what)` (stub: `hresultError`) |
| `D3D12RootSignature.{h,cpp}` | T4 | `class RootSignatures { Result<void> create(DeviceImpl&); ID3D12RootSignature* global() const; ID3D12RootSignature* kernel() const; ID3D12CommandSignature* draw() const; …drawIndexed(); …mesh(); …dispatch(); std::span<const std::byte> serializedGlobal() const; }` (stub: `create` succeeds, all null) |
| `D3D12Descriptors.{h,cpp}` | T5 | `class DescriptorHeaps { Result<void> create(DeviceImpl&); uint32_t viewCapacity() const; uint32_t samplerCapacity() const; ID3D12DescriptorHeap* views() const; ID3D12DescriptorHeap* samplers() const; D3D12_CPU_DESCRIPTOR_HANDLE viewCpu(uint32_t) const; D3D12_GPU_DESCRIPTOR_HANDLE viewGpu(uint32_t) const; D3D12_CPU_DESCRIPTOR_HANDLE samplerCpu(uint32_t) const; TransientSlot claimTransient(uint32_t frameSlot); void resetTransient(uint32_t frameSlot); }`; `struct TransientSlot { uint32_t index; D3D12_GPU_DESCRIPTOR_HANDLE gpu; D3D12_CPU_DESCRIPTOR_HANDLE cpu; D3D12_CPU_DESCRIPTOR_HANDLE cpuTwin; }`; `Result<uint32_t> createRawViews(DeviceImpl&, ID3D12Resource*, uint64_t width, bool writable)`; `uint64_t rawViewElements(uint64_t width)`; `void releaseRawViews(DeviceImpl&, uint32_t base, uint32_t count)` (stub: capacities 1,000,000 / 2,048, the rest `notImplemented`) |
| `D3D12Addr.{h,cpp}` | T5 | `inline constexpr uint64_t kMaxAddrOffset = (1ull << 29) - 1;`; `struct BufferPayload { ID3D12Resource* resource; D3D12_GPU_VIRTUAL_ADDRESS base; uint64_t size; uint64_t bufferBits; uint32_t viewIndex; uint32_t viewCount; std::byte* host; }`; `class HeapDirectory { void create(uint32_t capacity); void publish(uint32_t index, const BufferPayload*); void clear(uint32_t index); const BufferPayload* find(uint32_t index) const; }`; `core::decodeAddr`, `operator+` (stubs: `notImplemented`) |
| `D3D12Queue.cpp`, `D3D12Fences.{h,cpp}` | T6 | `Queue::submit`; `beginFrame`, `endFrame`, `createFence`, `completedValue`, `wait`, `destroy(Fence)`; `Result<void> createQueues(DeviceImpl&)`; `void idleQueues(DeviceImpl&)`; hooks `retireFence`, `waitFrame`, `reuseSlot`; `struct QueueProgress { Com<ID3D12Fence> fence; uint64_t value; }`; `class FrameState { Result<void> create(DeviceImpl&); ID3D12Fence* fence() const; void keep(DeviceImpl&, ID3D12Fence*); void releasePassed(DeviceImpl&); void releaseAll(); size_t keptCount() const; }` (the frame fence and the kept fences of B9) (stubs: `createQueues` succeeds making nothing, `idleQueues` no-op, entries `notImplemented`/`unsupported`) |
| `D3D12ListState.{h,cpp}`, `D3D12CommandList.cpp` | T6 (acquire, `end`), T8 (`uploadFrameData`) | `class ListPools` (per frame slot: mutex, lists, used count); `Device::acquireCommandList`, `core::coreOf(CommandList)`, `CommandList::end`, `CommandList::uploadFrameData` |
| `D3D12Stager.{h,cpp}` | T7 | `struct SubresourceData { std::span<const std::byte> bytes; uint64_t bytesPerRow; uint64_t bytesPerImage; }`; `class Stager { Result<void> create(DeviceImpl&); Result<void> uploadBuffer(DeviceImpl&, ID3D12Resource* destination, std::span<const std::byte>); Result<void> uploadTexture(DeviceImpl&, ID3D12Resource* destination, std::span<const SubresourceData> subresources); void idle(); }` (stub: `create` succeeds, uploads `notImplemented`) |
| `D3D12Buffers.{h,cpp}` | T8 | `createBuffer`, `createHeap`, `placementRequirements(BufferDesc)`, `address`, `cpuPointer`, `destroy(Buffer)`, `destroy(Heap)`; hooks `retireBuffer`, `retireHeap`; `Result<core::ArenaPage> createArenaPage(void* context, uint64_t size)` |
| `D3D12Textures.{h,cpp}`, `D3D12Views.{h,cpp}` | T9 | `struct TexturePayload { ID3D12Resource* resource; Com<ID3D12DescriptorHeap> targets; uint32_t targetCount; DXGI_FORMAT resourceFormat; }`; `D3D12_CPU_DESCRIPTOR_HANDLE targetView(const TexturePayload&, uint32_t mip, uint32_t layer)`; `createTexture`, `placementRequirements(TextureDesc)`, `destroy(Texture)`, `createView`, `writeView`, `createSampler`, `writeSampler`, `reserveViews`, `reserveSamplers`, `release(IndexRange)`, `destroy(ViewIndex)`, `destroy(SamplerIndex)`; hooks `retireTexture`, `retireView` |

`DeviceImpl` (Task 1, declaration order = creation order; destruction runs in reverse after
`core.beginDestroy()`): `core::DeviceCore core; d3d12::Loaded loaded; d3d12::MessageRouter
messages; d3d12::DeviceFeatures features; std::array<QueueImpl, 3> queues; d3d12::FrameState
frames; d3d12::DescriptorHeaps descriptors; d3d12::HeapDirectory directory;
d3d12::RootSignatures roots; d3d12::Stager stager; d3d12::ListPools lists; std::string
adapterName; std::string floorName;` plus `ID3D12Device12* device() const`. `QueueImpl`:
`DeviceImpl* device; QueueType type; Com<ID3D12CommandQueue> native; QueueProgress progress;`.
`CommandListImpl`: `core::CommandListCore core;` only.

`createDevice` order (Task 1, final): `validateDeviceDesc` → `loadDevice` (OS check, factory,
debug settings, adapter loop with `checkFloor`) → `DeviceImpl` → `setRetireHooks` →
`messages.attach` → `createQueues` → `frames.create` → `descriptors.create` →
`directory.create(descriptors.viewCapacity())` → `reportCapabilities` → `roots.create` →
`stager.create` → `setSurfaceCheck(IsWindow)` → `setArenaSource(createArenaPage)`.
`destroyDevice`: `idleQueues` → `stager.idle()` → `core.beginDestroy()` → release kept fences →
delete the `DeviceImpl`.

---

## Waves

| Wave | Tasks (parallel within a wave) | Needs |
|---|---|---|
| 1 | T1 | P5.1a merged |
| 2 | T2, T3, T4, T5, T6, T7 | T1 |
| 3 | T8, T9 | wave 2 (T8: T5, T6, T7; T9: T2, T5, T7, and T8's heap payload by interface only) |
| 4 | T10, T11 | wave 3 |

Model: opus for T1, T5, T6, T7, T8, T9 (structure, lifetime, descriptor heaps, barriers, the
`Addr` lowering); sonnet for T2, T3, T4, T10, T11.

## Tasks

Each brief says what the Windows CI run must show. One commit per task.

### Task 1 (wave 1, opus): device creation and the component skeleton
- [ ] Brief `task-1-brief.md`. Commit `d3d12: create a device on WARP through the Agility factory`.
- [ ] Frees `FundamentalsTests.cpp`, `MessageDeathTests.cpp`; adds the `death` entry; reformats
      `d3d12_pending` one name per line (B17); adds `Tests/D3D12` to the layout lists.

### Task 2 (wave 2, sonnet): capabilities, limits and the format table
- [ ] Commit `d3d12: report capabilities, limits and the format table`. Frees `FormatTests.cpp`.

### Task 3 (wave 2, sonnet): debug-layer messages and device removal
- [ ] Commit `d3d12: route debug-layer messages and report device removal`. Adds
      `ROJORHI_EXPECT_ABORT` to `Tests/Support/Death.{h,cpp}`.

### Task 4 (wave 2, sonnet): root signatures and command signatures
- [ ] Commit `d3d12: create the global root signature and command signatures`.

### Task 5 (wave 2, opus): descriptor heaps and the `Addr` directory
- [ ] Commit `d3d12: add the shader-visible heaps and the Addr directory`.

### Task 6 (wave 2, opus): queues, fences, frames and list acquisition
- [ ] Commit `d3d12: submit fence waits and signals and pace frames`. Frees `QueueTests.cpp`,
      `QueueDeathTests.cpp`.

### Task 7 (wave 2, opus): staged initial data and native readback support
- [ ] Commit `d3d12: stage initial data on the internal upload queue`.

### Task 8 (wave 3, opus): buffers, heaps, placement, addresses, frame data
- [ ] Commit `d3d12: create buffers and heaps with raw views and addresses`. Frees
      `BufferTests.cpp`, `BufferDeathTests.cpp`, `HeapTests.cpp`, `CommandListTests.cpp`,
      `CommandListDeathTests.cpp` (each only if it passes whole).

### Task 9 (wave 3, opus): textures, views and samplers
- [ ] Commit `d3d12: create textures, views and samplers`. Frees `SamplerTests.cpp`,
      `SamplerDeathTests.cpp`, `FormatDeathTests.cpp`, `TextureDeathTests.cpp` (each only if it
      passes whole without heaps).

### Task 10 (wave 4, sonnet): entries, pending lists, first T1 cases on WARP
- [ ] Commit `test: run the D3D12 contract, death and conformance entries`. Frees the rest of
      the 18 (`DeviceTests.cpp` included, B20), adds the `conformance` entry and
      `d3d12_conformance_pending` (B16) with `Buffer`, `Frame`, `Queue` running, CI floors,
      Release `conformance`, `check_lto_inline.py`, B23 (#83).

### Task 11 (wave 4, sonnet): documents
- [ ] Commit `docs: describe the D3D12 device and resources`. `d3d12-backend.md`; spec 01:222
      (D11), 02 Open and Floor-check cells (D4, D12); 15p2:74-77 note (D9 merged, B20).

---

## Final commit list (reshape)

| # | Subject | Tasks |
|---|---|---|
| 1 | `d3d12: create a device on WARP through the Agility factory` | 1, 2, 3 |
| 2 | `d3d12: add descriptor heaps, root signatures and the Addr directory` | 4, 5 |
| 3 | `d3d12: submit fence waits and signals and pace frames` | 6 |
| 4 | `d3d12: create buffers, heaps, textures, views and samplers` | 7, 8, 9, 10 |
| 5 | `docs: describe the D3D12 device and resources` | 11 |

Each reshaped commit passes the Mac checks of `common.md`; the Windows checks are CI's run at
HEAD. If P5.1b is split into two PRs (design §8 allows it), the cut is after commit 3.

## Exit checks (design §8 row P5.1b, as narrowed)

- [ ] `d3d12` job green at HEAD: Debug (`warp`, `gbv`) `abi`, `contract`, `death`,
      `conformance`; Release `contract` and `conformance`, warning-free; `check_lto_inline.py`.
- [ ] `d3d12_pending` = the 37 files of later chapters; `d3d12_conformance_pending` = every
      conformance file except `Buffer`, `Frame`, `Queue`.
- [ ] No debug-layer or GBV message above `Info` in the green run's log.
- [ ] `docs/architecture/d3d12-backend.md` describes P5.1b, labelled WARP-only; spec cells of
      D4, D11, D12 amended.
- [ ] Every other CI job green; the `interface` job unchanged.
- [ ] Evidence: the green run's `d3d12` job log saved to
      `../rojo-rhi-evidence/d3d12-foundation/<date>/` with `SHA256SUMS`, cited in the PR body as
      WARP-only.

## Stop conditions

A floor item WARP lacks after all (C1); a `Source/Core` change that would alter another
backend's behaviour or contract (a backend-neutral seam addition is the orchestrator's call); CI
that cannot be made green honestly (no relaxed assertion, no filtered message
without a reason that holds on hardware); a task blocked after two attempts; a debug-layer error
on a valid path that the design cannot explain.

## Decisions recorded during implementation

_(the orchestrator appends here)_
