# D3D12 edges (P5.4): presentation, sparse tiles, capture, FSR and the full T1 run on WARP: implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or
> superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Status**: Not started (written 2026-10-20)

**Goal:** the edge entries of chapters 11, 12, 13 and 14 on the D3D12 backend (DXGI flip-model
swapchains, reserved resources and `UpdateTileMappings`, PIX capture, FSR through the FidelityFX
runtime), issue #68 on every backend (`VU-rojo-sparse-0014`, its tracker check, death and contract
cases, the Metal 4 lowering and a T1 case under GPU-based validation), the chapter 11/12/13
amendments (D18), the remaining spec-amendment ledger rows, and the full T1 run on WARP with both
pending lists emptied except cases recorded PC-only with their reasons.

**Architecture:** the part lands as three pull requests, so the work that needs no command
recording runs beside P5.2 and P5.3 as P4's edges stream ran beside P4.2: **PR A** (no D3D12
device needed: #68's core and Metal halves, the FidelityFX pin and an FSR probe on WARP), **PR B**
(needs only P5.1b's device and resources: swapchains, reserved resources and tile mapping, capture,
FSR context creation), **PR C** (the tail, after P5.3: upscale recording, the #68 T1 case, every
contract file and T1 case on WARP, the ledger, the documents). Every entry calls the core's check
first, then translates; no `ROJORHI_VU` under `Backends/`.

**Tech Stack:** C++23 (MSVC `/std:c++23preview`, `/W4 /WX`), xmake, Agility SDK 1.619.6, WARP
1.0.21, DXGI 1.6, FidelityFX SDK v2.3.0 (ffx API, runtime-loaded), PIX GPU capturer (runtime-loaded),
Catch2, hosted `windows-latest` CI as the only D3D12 compiler.

**Spec:** `/Users/rudyz/Documents/projects/rojo-rhi/.superpowers/p5/d3d12-backend-design.md`
§5.16, §5.17, §5.19, §5.20, §5.22, §5.23, §7, §8 row P5.4, §10 (D2, D3, D18, D19, D20, D25, D29,
D30); the ledger `.superpowers/p5/spec-amendments.md`; issue #68; the API reference chapters 02,
09 part 2, 11, 12, 13, 14, verbatim, with their D3D12 rows. The digest `spec-d3d12.md` §4
(chapters 11–14) and §8 items 15, 17, 20, 21 name the open points this plan settles.

## Global Constraints

- AGENTS.md hard rules win over everything here.
- Every check lives in `Source/Core`; no `ROJORHI_VU` under `Backends/`; library invariants use
  `ROJORHI_INTERNAL_ASSERT`. Exactly one new valid-usage identifier in this part:
  `VU-rojo-sparse-0014` (Task 1). No identifier renumbered or reused.
- No Windows, D3D12, DXGI, PIX or FidelityFX type in `Include/`; no public API added (design §2
  non-goals). DLSS and XeSS are not compiled in.
- Release recording takes no lock except a frame-arena page claim (ADR 0020); `upscale` takes none.
  RojoRHI creates no thread (ADR 0013) and reads or sets no environment variable.
- Every native object gets `SetName` from its RojoRHI label (`rojorhi.` default); DXGI swapchains
  and FidelityFX contexts take no name (they are not `ID3D12Object`s) and the page says so.
- Pins live in `xmake/setup.lua`'s Windows branch, each download hashed (SHA-256 of the file
  downloaded) before use; nothing from a vendor SDK is committed (D18, D19).
- Documents say "implemented, verified on WARP only, not on hardware" (D29); a WARP run is never
  called T1 acceptance (D2); #21 and #22 stay open.
- `Legacy/`, `Benchmarks/`, `Tools/bench.py` are not touched. `README.md`, `AGENTS.md` and
  `docs/conventions/testing.md` change only through ledger row 2's facts (Task 12).
- Commit scopes from `Tools/check_project_policy.py` (`d3d12`, `rhi`, `metal`, `test`, `docs`,
  `build`); no AI or tool attribution; no "agent", "prompt", "review", "plan", "task" or model
  names in messages; never `--no-verify`; never set `ROJORHI_DEATH_CHILD`.
- No local D3D12 compile and no MinGW syntax aid (as P5.1a's W8): Windows CI on the PR is the
  compiler. Before any local build or test, `pgrep -fl "bench.py timing|RojoRHIBench"` must print
  nothing (P4.6 round 2 is timing on this Mac).

## State this plan assumes (verify at each PR's start)

- **P4.6**: its code PR #70 merged on 2026-10-20 (main 08ac348; the tree equals
  `feat/metal4-overhead`'s tip) and no `m4o/*` branch exists; round 2's timing runs from
  `rojo-rhi-worktrees/t2-round2` on main 0a0b53f and edits only `docs/research/`. The avoid list of
  design §3 has therefore lapsed for this part. Each PR's first step re-checks
  `git ls-remote origin 'refs/heads/m4o/*' 'refs/heads/feat/metal4-overhead*'` and
  `gh pr list --state open --search metal4`; if round 2 missed and the owner reopened fixes, any
  file such a branch touches is edited only at hunks ten lines away, and the PR body names the
  check. Local Metal 4 GPU runs (Tasks 1, 2, 10) still wait on the `pgrep` check.
- **P5.1a** (PR #71, `feat/d3d12-wiring`, draft, every task done) has merged before Task 3 starts:
  `ThirdParty/{agility,warp,dxc}` pins, `rojorhi.d3d12_runtime` (already copies
  `ThirdParty/fidelityfx/bin/amd_fidelityfx_{loader,upscaler}_dx12.dll` when present), the probes
  `RojoRHID3D12Probe{Factory,Exports}`, the `d3d12` CI job, `d3d12_pending`, commit scope `d3d12`.
  Its gate findings bind here: D4 = device factory; a hidden window gets a flip-model swapchain
  that acquires and presents on the hosted runner (so D20's PC-only fallback is not expected);
  `OPTIONS19` view heap 2,097,152; WARP reports SM 6.9.
- **P5.1b** (device and resources) has merged before PR B starts: `DeviceImpl`, `QueueImpl`,
  `SwapchainImpl` in `D3D12Device.h`; the DXGI factory kept by `D3D12Loader`; `Com<T>`,
  `d3d12::check`/HRESULT → `Error`, `d3d12::setName` in `D3D12Common`; buffer, heap and texture
  creation in `D3D12Buffers`/`D3D12Textures` with retire hooks; per-texture RTV/DSV CPU heaps;
  `D3D12Capabilities` (`reportCapabilities` or its equivalent) and `D3D12Formats`;
  `Queue::submit`, fences, frames and the `frameSlot` hook; `Tests/Support/D3D12` with
  `runsOnGpu() == true`; the `conformance` entry and `d3d12_conformance_pending` in
  `xmake/targets.lua`; `noteNativeCall`. Names follow design §5.1; where P5.1b's differ, the
  implementer uses P5.1b's and reports it. Nothing in PR B is written against an unmerged branch.
- **P5.2 and P5.3** have merged before PR C starts: list state with the dirty flags of D16, the
  barrier lowering (discard from `Undefined` included), timestamps, render passes, pipelines and
  the graphics state restore of §5.9.
- **The prework stream** (D27 `clearTexture` from `CopyDst`, #25, #73) may touch
  `Source/Core/Tracker/` and the death-test files Task 1 touches; its hunks are the clear rule's,
  Task 1's are the tile rule's. Whichever merges second rebases; no file region is shared.

## Decisions

- **X1. Three pull requests, one plan.** PR A `feat/d3d12-edges-early` (Tasks 1–3), PR B
  `feat/d3d12-edges` (Tasks 4–8), PR C `feat/d3d12-finish` (Tasks 9–12). Each runs the part
  procedure on its own (plan commit first and dropped, draft PR, ledger section, reshape, merge).
  PR A does not wait for P5.1b; PR B does not wait for P5.2 or P5.3; PR C waits for P5.3 and PR B.
  Reason: maximum parallelism with no task written against unmerged code. #68 closes with PR C.
- **X2. `VU-rojo-sparse-0014` states chapter 13's existing prose and nothing more.** Text, in the
  `Queue::mapTiles` entry: "`VU-rojo-sparse-0014` The first use of a tile that `Queue::mapTiles`
  newly mapped onto heap memory is, for a texture, a texture barrier from `Layout::Undefined` over
  the subresource that holds it, and, for a buffer, ordered after the mapping by a global barrier
  whose destination covers that use. Checked by the validation tracker." No API version change (a
  rule the prose already stated gains its identifier). Tracker semantics:
  - `TileChange` gains `bool unmap`. A change that maps (not `unmap`) marks its region on the
    resource: per subresource with the change's texel box (the whole subresource for a mip tail),
    or the buffer byte range. A remap marks again; an unmap clears the region's mark (an unmapped
    tile has no first use to judge); destroying the resource drops its marks.
  - Marks belong to the resource, not to a queue: the next event that reaches a marked region on
    any queue is judged (cross-queue ordering stays `VU-rojo-sparse-0013`'s and
    `VU-rojo-queue-0005`'s).
  - Texture: a texture barrier from `Undefined` over the subresource clears every mark of it. Any
    other event on the subresource reports when it overlaps a mark: an event with a texel box
    (copies) overlaps when the boxes intersect, an event without one (barriers, passes, views,
    clears) always overlaps. An event with a box that misses every mark neither reports nor clears.
  - Buffer: a use of marked bytes clears the mark when a global barrier recorded on the use's
    queue after the mapping's position there (in a later submission, or in the same list before
    the use) has a destination covering the use's stage and access; otherwise it reports.
  - Precedence: checked in the same slot as `VU-rojo-sync-0017` (textures) and `VU-rojo-sync-0009`
    (buffers), after the cross-queue rules (`VU-rojo-sparse-0013`, `VU-rojo-queue-0005`), so every
    existing death test keeps its identifier and an event that violates only this rule reports
    only this one.
  The limitation (a partially resident texture cannot take a new tile without discarding its whole
  subresource) is chapter 13's already; Task 1 files it as an issue for the owner and changes
  nothing.
- **X3. Existing cases that now misuse.** Task 1 audits every case that maps tiles
  (`Tests/Contract/Sparse*.cpp`, `Tests/Conformance/SparseConformanceTests.cpp`,
  `Tests/Metal4/Metal4SparseTests.cpp`, `Tests/Core/{Sparse,Tile}CoreTests.cpp`,
  `Tests/Null/NullCounterTests.cpp`, `Tests/Support/SparseKit.*`) and inserts the barrier X2
  requires before a first use that lacks it, in the same commit. No assertion changes; a case that
  cannot be fixed by adding a barrier is reported, not weakened. `Benchmarks/` maps no tiles
  (`RojoTrace` only forwards `mapTiles`), so it stays untouched.
- **X4. Metal 4 half of #68.** A texture barrier from `Undefined` on a `Sparse` texture adds
  `MTL4::VisibilityOptionResourceAlias`, as for a placed one (`Metal4Barriers.cpp` near line 251),
  and the 09 part 2 Visibility row (line 187) names Sparse textures. A `[metal4]` case pins it
  through the seam `Metal4BarrierTests.cpp` already uses for placed textures.
- **X5. FSR is probed on WARP before any backend code (design §8 "first task").** Task 3 extends
  the floor probe with an `fsr` section: load `amd_fidelityfx_loader_dx12.dll` beside the
  executable, query the upscaler provider's version, create one upscale context on the WARP device
  (64×64 → 128×128, `RGBA16Float` color, `D32Float` depth, `RG16Float` motion), record one
  dispatch on a direct list after one enhanced barrier, execute, wait, and read one texel of the
  output; fields `fsr.loaded`, `fsr.version`, `fsr.contextCreated`, `fsr.dispatched`,
  `fsr.outputNonZero`, `fsr.layerMessages` (debug-layer and GBV errors raised during the dispatch).
  FSR is not a floor item: the probe's exit code ignores it. Outcomes: (a) all true, no message →
  the upscale cases run on WARP (Tasks 7, 9, 11); (b) loaded but context or dispatch fails, or a
  GBV error from FidelityFX's own legacy barriers → `upscalers` still lists FSR on hardware, and the
  `Upscale*` contract files and `UpscaleConformanceTests.cpp` are PC-only on WARP in the pending
  lists with the probe JSON as the reason (design §7.5, risk 6); (c) the loader does not load →
  stop and report (the pin is wrong). The outcome is recorded in this plan's decisions log.
- **X6. FidelityFX pin.** `setup.lua` fetches, at tag v2.3.0, the raw files design §4.1 lists
  (ffx API headers, `ffx_upscale.h`, the two signed DLLs and the licence files), each with its own
  SHA-256 checked before it is kept, into `ThirdParty/fidelityfx/{include,bin}` with
  `.archive-sha256` markers like the other pins; the Windows branch only; setup prints the pin.
  The exact repository paths are read from the tag's tree when Task 3 starts and written into the
  brief's report; the hashes are computed from the downloads in a scratch directory on the Mac and
  checked again by CI's `xmake setup`.
- **X7. Swapchains (Task 4).** `CreateSwapChainForHwnd` on the graphics queue with
  `FLIP_DISCARD`, `BufferCount = imageCount`, the color space's format, flags
  `FRAME_LATENCY_WAITABLE_OBJECT` (+ `ALLOW_TEARING` for `Immediate`), then
  `SetMaximumFrameLatency(maxLatency)`, `CheckColorSpaceSupport` + `SetColorSpace1`; `Immediate`
  without `DXGI_FEATURE_PRESENT_ALLOW_TEARING`, or a color space the output refuses, fails creation
  with `Unsupported` naming it (11:66-67). `MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER)`.
  Back buffers: fetched with `GetBuffer` once per create and resize; each keeps one RTV in a
  swapchain-owned CPU heap and one `SetName` (`<label>.image<i>`). `acquire` waits on the waitable
  object (one second, else a null `Texture`, no state change), reads
  `GetCurrentBackBufferIndex`, and hands the core (`core::acquireImage(swapchain, native)`) a
  payload that borrows the back buffer and its RTV: the record's `swapchainImage` flag makes the
  texture retire hook skip every release. `present` (on `Queue::present`): `Present(1, 0)` Fifo,
  `Present(0, 0)` Mailbox, `Present(0, DXGI_PRESENT_ALLOW_TEARING)` Immediate; `S_OK` and
  `DXGI_STATUS_OCCLUDED` succeed, a device-removed result goes through §5.21's path, anything else
  aborts with the HRESULT. `resize`: the core check, then wait for the frame fence of the last
  ended frame (every list that used the images), drop the back-buffer references, `ResizeBuffers`
  with the same count, format and flags, fetch again. `destroy`: the swapchain is filed in the
  slot's retired list and released by the `frameSlot` hook (and `destroyDevice`), as Null and
  Metal 4 do. The surface check is `IsWindow` (registered by P5.1b or here). Counting:
  `nativeSubmitCalls` per `Present`; `nativeHostCalls` per waitable wait and per resize fence
  wait; `nativeCreateCalls` stays the core's one per swapchain. Lock: one swapchain mutex, a leaf,
  held only around back-buffer reads and writes, never across a wait or `Present`.
- **X8. Reserved resources (Task 5).** `BufferUsage::Sparse` and `TextureUsage::Sparse` create
  through `CreateReservedResource2` (initial layout `UNDEFINED`), `Memory::Gpu` only; a buffer is
  rounded up to a whole 64 KiB tile and keeps its raw view pair (it is a buffer like any other).
  Tile shape per texture from `GetResourceTiling`, cached in `TextureRecord::tiles` at creation
  (`width/height/depth` from `D3D12_TILE_SHAPE`, `bytes` 65,536, `mipTailFirst` =
  `NumStandardMips`, or the mip count when `NumPackedMips` is 0, `mipTailBytes` =
  `NumTilesForPackedMips` × 65,536). A [d3d12] case pins equality with `core::standardTileExtent`
  for every format reported sparse. `placementSparse` = `TiledResourcesTier` ≥ 2 (D13); a format
  reports `sparse` when `D3D12_FORMAT_SUPPORT2_TILED` is set, 3D textures only at tier 3; a sparse
  texture of a type or format the device refuses fails creation with `Unsupported`.
  `Queue::mapTiles`: `core::mapTiles` first, then one `UpdateTileMappings` per (resource, heap or
  null) group in element order: a region gives, per layer, one coordinate at subresource
  `mip + layer × mips` with `UseBox` and the box in tiles, one heap range from `heapOffset / 64 KiB`
  advancing by the layer's tile count; a mip tail gives, per layer, one coordinate at the first
  packed mip of that layer with `NumTiles = NumTilesForPackedMips`; a buffer range gives one
  coordinate at `bufferOffset / 64 KiB`; unmap passes a null heap and
  `D3D12_TILE_RANGE_FLAG_NULL`. Flags `D3D12_TILE_MAPPING_FLAG_NONE`. Grouping is order-free
  because `VU-rojo-sparse-0012` forbids two elements naming one tile. Destroying a reserved
  resource issues no native unmap. Counting: `nativeSubmitCalls` per `UpdateTileMappings`. If the
  debug layer refuses `UpdateTileMappings` on the copy queue, stop and report (chapter 13 says any
  queue type may map).
- **X9. Capture (Task 6).** With `enableCapture`, before the device factory: find the newest
  `%ProgramFiles%\Microsoft PIX\<version>\WinPixGpuCapturer.dll` (`SHGetKnownFolderPath`
  `FOLDERID_ProgramFiles`, highest version directory by numeric compare) and `LoadLibraryW` it.
  Task 6 first reads `pix3.h`/`pix3_win.h` from the pinned WinPixEventRuntime NuGet in a scratch
  directory to learn where `PIXBeginCapture2`/`PIXEndCapture` live. If the capturer exports them,
  the backend resolves them with `GetProcAddress`; otherwise `setup.lua` pins WinPixEventRuntime
  (hashed), `rojorhi.d3d12_runtime` copies `WinPixEventRuntime.dll` optionally, and the backend
  loads it at run time and resolves them there. Without PIX (hosted CI) `beginCapture` returns
  `Unsupported` naming PIX; a refused start returns `DriverFailure` with the HRESULT. A real
  capture is [HW] and stays on the PC checklist.
- **X10. FSR (Tasks 7 and 9).** At device creation, behind a once-flag:
  `LoadLibraryW(<exe dir>\amd_fidelityfx_loader_dx12.dll)` and `GetProcAddress` for
  `ffxCreateContext`, `ffxDestroyContext`, `ffxConfigure`, `ffxQuery`, `ffxDispatch`; FSR is listed
  in `upscalers` when they all resolve and a version query with the device returns an upscaler
  provider (D18). The module is freed at `destroyDevice` after every context retired. Test seam:
  `d3d12::loadFidelityFx(std::wstring_view directory)` returning an optional table, so a [d3d12]
  case loads from an empty directory and sees nothing. `createUpscaler`: the core's
  `prepareUpscaler`, then `ffxCreateContext` with `ffxCreateContextDescUpscale` chained to the DX12
  backend desc, under a leaf mutex (creation is free-threaded; the loader's thread safety is not
  documented); `Auto` → FSR; DLSS/XeSS → `Unsupported`. The retire hook calls `ffxDestroyContext`
  under the same mutex. `upscale` (Task 9): `ffxDispatch` with `ffxDispatchDescUpscale` on the
  graphics list, inputs passed in the legacy states the enhanced-barrier spec maps from the
  reference's layouts (`ShaderRead` ↔ `NON_PIXEL_SHADER_RESOURCE | PIXEL_SHADER_RESOURCE`,
  `ShaderWrite` ↔ `UNORDERED_ACCESS`), then every lazily bound state is marked dirty (heaps, both
  root signatures, root SRVs, `b1`, pipeline, topology; §5.9). No lock. Counting: one
  `noteNativeCall` for the dispatch, plus the restore calls when the next draw or dispatch issues
  them. Chapter 12's and 02's `upscalers` sentence becomes "FSR is compiled in always and is listed
  when the FidelityFX runtime it loads is present beside the executable" (D18), in Task 7.
- **X11. Pending lists.** PR B removes a contract file from `d3d12_pending` and a case from
  `d3d12_conformance_pending` only when it passes whole on WARP under GBV, death tests included
  (no split file). Task 11 removes the rest. A case stays only as **PC-only**, named with its
  reason and the evidence (probe JSON or CI log line), when WARP cannot run it; each such entry
  becomes an issue line on the hardware checklist. A failing case caused by a backend defect is
  fixed, never listed.
- **X12. Conflicts with P5.2 and P5.3 (PR B runs beside them).** Footprint PR B allows itself:
  `D3D12Device.h` (one member each in `DeviceImpl` and `SwapchainImpl`, one include per new
  header); `D3D12Device.cpp` (one-line calls in `createDevice`, `destroyDevice`, the `frameSlot`
  hook and the capabilities report; the stub entries of capture, tile shapes and upscalers moved
  out); `D3D12Queue.cpp` (the `present` and `mapTiles` stubs moved out to `D3D12Swapchain.cpp` and
  `D3D12Sparse.cpp`); `D3D12Buffers.cpp`/`D3D12Textures.cpp` (one branch each for `Sparse`);
  `D3D12Formats.cpp` (the tiled bit); `xmake/targets.lua` (names in the two pending lists);
  `Tests/Support/D3D12/D3D12Backend.cpp` (`surface`); chapters 11, 12, 13, 14 and 02 rows. Whoever
  rebases second keeps both sides; a task that must edit beyond this footprint stops and reports.
- **X13. Evidence.** PR C downloads the green run's `d3d12` job log and the probe artifact into
  `../rojo-rhi-evidence/d3d12-t1-warp/<date>/` with `SHA256SUMS`; the PR body quotes the
  `conformance`, `contract`, `death` and `abi` summary lines of Debug (GBV) and Release, and the
  PC-only list. PR A records the FSR probe JSON the same way under `d3d12-fsr-probe/<date>/`.

## Review Focus

1. **Resize or destroy while DXGI still holds references**: `ResizeBuffers` fails with
   `DXGI_ERROR_INVALID_CALL` if any back-buffer reference or RTV-holding record survives. Task 4
   pins: acquire/present over 3 × `framesInFlight` frames, resize three times between frames, then
   destroy with an image acquired, all with no debug-layer message and no leak report at
   `destroyDevice`.
2. **FidelityFX runtime absent beside the executable** (a consumer that does not ship the DLLs):
   `upscalers` is empty and `createUpscaler(Auto)` returns `Unsupported`, never a crash. Task 7
   pins it through `loadFidelityFx` on an empty directory.
3. **FidelityFX clobbers list state**: after an upscale the next dispatch and draw must rebind
   heaps, root signatures, root SRVs, `b1`, pipeline and topology. Task 9 pins: upscale, then a
   dispatch writing through its root block, then a draw, both outputs read back, no GBV message.
4. **Subresource and mip-tail arithmetic in `UpdateTileMappings`** on 2D arrays with tails: a
   wrong `mip + layer × mips` or a tail on the wrong layer maps memory silently. Task 5 pins a
   [d3d12] case that maps the tail of layer 1 only and region tiles of layer 0, checks
   `GetResourceTiling`'s packed-mip data against `tileShape`, and Task 11 runs
   `SparseConformanceTests.cpp`'s tail and region cases on WARP.
5. **Present-mode and occlusion edges**: `Immediate` where tearing is unsupported must fail
   creation with `Unsupported`, and `DXGI_STATUS_OCCLUDED` from a hidden window must count as a
   successful present. Task 4 pins both (the tearing branch through a seam taking the feature
   result, so WARP's own answer does not decide which branch is tested).

## Tasks

One commit per task with the subject given; files, interfaces, tests and the CI expectation are in
`task-N-brief.md`. "Tests first" means the cases are seen failing (locally for Null/Metal 4, in a
CI run for D3D12) before the code.

| # | Task | PR | Model | Depends on | Commit subject |
|---|---|---|---|---|---|
| 1 | `VU-rojo-sparse-0014`: spec, tracker, Null death and contract cases, test audit (X2, X3) | A | opus | main now | `rhi: check the first use of a newly mapped sparse tile` |
| 2 | Metal 4 discard of a Sparse texture with alias visibility (X4) | A | sonnet | main now | `metal: discard sparse textures with alias visibility` |
| 3 | FidelityFX pin and the FSR probe on WARP (X5, X6) | A | opus | P5.1a merged | `build: fetch the FidelityFX upscaler and probe it on WARP` |
| 4 | DXGI flip-model swapchains (X7) | B | opus | P5.1b merged | `d3d12: present through DXGI flip-model swapchains` |
| 5 | Reserved resources, tile shapes, `UpdateTileMappings` (X8) | B | opus | P5.1b merged | `d3d12: map tiles of reserved resources` |
| 6 | Capture through the PIX GPU capturer (X9) | B | sonnet | P5.1b merged | `d3d12: capture through the PIX GPU capturer` |
| 7 | FSR loading, `upscalers`, `createUpscaler`, D18 amendment (X10) | B | opus | P5.1b and PR A merged | `d3d12: create FSR upscalers through the FidelityFX runtime` |
| 8 | `docs/architecture/d3d12-backend-edges.md` (D19, D29) | B | sonnet | 4–7 | `docs: describe D3D12 presentation, sparse tiles, capture and FSR` |
| 9 | Upscale recording (X10) | C | opus | P5.2, P5.3, PR B merged | `d3d12: record FSR upscales on graphics lists` |
| 10 | #68's T1 case under GBV | C | sonnet | P5.2, P5.3, PRs A and B merged | `test: render into a newly mapped sparse tile after a discard` |
| 11 | Every contract file and T1 case on WARP; pending lists emptied (X11) | C | opus | 9, 10 | `test: run every contract file and T1 case on D3D12 under WARP` |
| 12 | Ledger rows, documents, evidence (X13) | C | sonnet | 11 | `docs: record the D3D12 T1 run on WARP` |

## Parallel schedule

Per-task worktrees `/Users/rudyz/Documents/projects/rojo-rhi-worktrees/p5-de-taskN` on branches
`de/taskN`, each from its PR branch's head when its wave starts; finished tasks and fixups are
cherry-picked onto the PR branch in task order and CI reruns there. A task finding it must edit a
region another task of its wave owns stops and reports.

| Wave | Tasks, concurrently | Starts after | Why they do not overlap |
|---|---|---|---|
| A1 | 1, 2 | now (beside P5.1a/P5.1b) | 1: chapter 13, `Source/Core/{Sparse.cpp,Tracker/}`, contract/conformance/core sparse tests, `validation-tracker-queues.md`; 2: `Metal4Barriers.cpp`, 09 part 2 line 187, `Metal4BarrierTests.cpp` |
| A2 | 3 | P5.1a merged (may overlap A1) | `xmake/setup.lua` (FidelityFX block), `Tools/D3D12Probe/`, the probe step of `ci.yml` |
| B1 | 4, 5, 6, 7 | P5.1b merged (7 also needs PR A on main) | 4: `D3D12Swapchain.{h,cpp}`, `present`, `surface`, chapter 11; 5: `D3D12Sparse.{h,cpp}`, `mapTiles`, tile shapes, the Sparse branches of buffers/textures, formats, chapter 13; 6: `D3D12Capture.{h,cpp}`, capture entries, chapter 14 row (and a setup.lua block only under X9's fallback); 7: `D3D12Upscaler.{h,cpp}`, `createUpscaler`, capabilities line, chapters 12 and 02 |
| B2 | 8 | 4–7 on the branch | the new architecture page and `CHANGELOG.md` |
| C1 | 9, 10 | P5.3 and PR B merged (10 also PR A) | 9: `D3D12Upscaler.cpp`'s `upscale`, list-state dirty marking; 10: `SparseConformanceTests.cpp` only |
| C2 | 11 | 9, 10 on the branch | `xmake/targets.lua` pending lists, `ci.yml` (job split if needed), backend fixes as fixups of the commit owning the file |
| C3 | 12 | 11 green | documents only |

**Right after P5.1b merges**: Tasks 4, 5, 6 and 7 (and 3 if PR A is still open). **Before P5.1b**:
Tasks 1 and 2 (now) and 3 (once P5.1a merges). PR C is the only stage waiting for P5.2/P5.3.

## Checks

`common.md` holds the list: on the Mac, policy tools, `xmake format --check`, Null Debug and
Release (`contract`, `death`, `abi`), comments and headers, layout with the four D3D12
`--exclude`s, and, for Tasks 1, 2 and 10 only, Metal 4 Debug `contract`, `death`, `conformance`
under `MTL_DEBUG_LAYER=1` (each after the `pgrep` check). On CI, every job green; each brief names
what the `d3d12` job must show at its commit.

## Final shape

- **PR A** (3 commits): `rhi: check the first use of a newly mapped sparse tile` (1);
  `metal: discard sparse textures with alias visibility` (2);
  `build: fetch the FidelityFX upscaler and probe it on WARP` (3).
- **PR B** (3 commits): `d3d12: present through DXGI flip-model swapchains` (4);
  `d3d12: map reserved tiles, capture through PIX and create FSR upscalers` (5, 6, 7);
  `docs: describe D3D12 presentation, sparse tiles, capture and FSR` (8).
- **PR C** (4 commits): `d3d12: record FSR upscales on graphics lists` (9, plus backend fixes Task
  11 makes in D3D12 files); `test: render into a newly mapped sparse tile after a discard` (10);
  `test: run every contract file and T1 case on D3D12 under WARP` (11's lists, CI and test-support
  changes); `docs: record the D3D12 T1 run on WARP` (12). A Task 11 fix in a file another part
  owns (P5.1b–P5.3 code) is its own `d3d12:` commit with the defect named, placed before the
  `test:` commit.

The plan commit is dropped from each PR; fixups go with the commit owning their files.

## Exit

- PR A: `VU-rojo-sparse-0014` in chapter 13 with a death test per arm (texture, buffer) and
  positive contract cases, `check_vu_coverage.py` green; Metal 4 `contract`, `death`,
  `conformance` green locally under `MTL_DEBUG_LAYER=1`; the probe artifact holds the `fsr`
  section and the outcome (X5) is recorded.
- PR B: `Backends/D3D12` has no `notImplemented`/`unsupported` stub left in swapchain, sparse,
  capture or upscaler creation entries; `[d3d12]` cases for each pass on WARP under GBV; every
  edges contract file that passes whole has left `d3d12_pending`.
- PR C: `d3d12_pending` is empty; `d3d12_conformance_pending` holds only PC-only cases with reasons
  (expected: none, or the `Upscale*` set under X5 (b)); Debug (GBV) `contract`, `death`, `abi`,
  `conformance` and Release `contract`, `conformance` green on WARP; #68 closed; the ledger empty
  or each remaining row named for P5.5/P5.6 with its reason; evidence stored and cited.

## Stop conditions

Stop and report (no merge) when: the FidelityFX loader does not load on the runner (X5 (c)); the
debug layer refuses a native form X7 or X8 fixes and no listed fallback exists; a T1 case fails on
WARP and neither a backend fix nor documented WARP evidence explains it after two attempts; a fix
would edit a live P4.6 branch's hunk; X2's rule makes a case unfixable without weakening it.

## Decisions log (orchestrators append)

- 2026-10-20 (planner): X1–X13 as above. P4.6 code merged before this plan (PR #70); avoid list
  lapsed, local GPU timing rule still binds.
