# D3D12 drawing: implementation plan

**Status**: In progress (P5.3b, 2026-10-20)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax in the briefs. Per-task detail lives in the workspace
> `.superpowers/sdd/d3d12-drawing/` (`common.md`, `common-review.md`, `task-N-brief.md`).

**Goal:** P5.3 of the D3D12 backend: shader libraries in a stamped `.d3d12lib` container built by
the shader rule and loaded by `Device::loadShaderLibrary` (closes #23), graphics, mesh and compute
pipelines from state streams naming the global root signature, a pipeline-library cache, render
passes with load, store and resolve, dynamic state, direct draws, dispatches, `ExecuteIndirect`
draws and dispatches, and mesh tasks, verified on WARP under the debug layer and GBV.

**Architecture:** two pull requests. **P5.3a** (`feat/d3d12-pipelines`, Tasks 1-6) is device-side
only: the container, its packaging and `[abi]` cases, the loader, pipelines and the cache; it
needs P5.1b, not P5.2. **P5.3b** (`feat/d3d12-drawing`, Tasks 7-13) is recording-side: compute
binding and dispatches, passes, graphics state and draws, indirect draws and mesh, the
per-backend multisample expectation, the sweep of pending lists and the documents; it needs P5.2
and P5.3a merged.

**Tech stack:** C++23 (MSVC `/W4 /WX` on `windows-latest`), Agility SDK 1.619.6 headers, WARP
1.0.21, DXC v1.9.2607 through Slang v2026.14.1, xmake, Python 3, GitHub Actions.

**Spec:** `.superpowers/p5/d3d12-backend-design.md` §5.7, §5.9 (lazy bindings, dynamic state),
§5.10, §5.13, §7.4, §7.5, §8 row P5.3, §10 (D1, D2, D8-D11, D14-D17, D25, D29, D30); digests
`spec-d3d12.md` §3, §4 (05, 06, 07, 08), §8 items 3, 4, 7, 9, 11, 16 and `backend-shape.md` §1, §4;
the API reference chapters 05 (all parts), 06 (both), 07 (all), 08 with their D3D12 rows; issue
#23. AGENTS.md hard rules override everything.

## Global Constraints

- One global root signature (ADR 0018): root SRV `t0` space 0 (`DATA_VOLATILE`, D14), one root
  constant `b1` space 0, flags `CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | SAMPLER_HEAP_DIRECTLY_INDEXED`,
  no input-layout flag; every pipeline names it in its stream; shaders carry none.
- Command signatures (08:87-177): draw = [`CONSTANT` (param 1, offset 0, 1 value), `DRAW`] stride
  20; indexed = [`CONSTANT`, `DRAW_INDEXED`] stride 24; mesh = [`CONSTANT`, `DISPATCH_MESH`]
  stride 16; dispatch = [`DISPATCH`] stride 12, no root signature.
- `ROJORHI_SHADER_ABI_VERSION` is read from `Shaders/RojoRHI/RojoRHI.slang`; C++ uses
  `core::kShaderAbiVersion`; no test hard-codes the number. No reserved binding changes here.
- Every entry calls the core's check first; no `ROJORHI_VU` under `Backends/`; no new valid-usage
  identifier. Release recording takes no lock (ADR 0020); payloads come through
  `pool<...>().native(handle)` and the list's own state, never a record or a map.
- Each RojoRHI recording call costs at most its native minimum + 1 (ADR 0014:65, D16); no hidden
  PSO variant; no per-draw heap allocation; RojoRHI never calls `ResourceBarrier`.
- No Windows, D3D12 or vendor type in `Include/`; nothing under `Legacy/`; no "v2" in names;
  RojoRHI reads no environment variable.
- Documents say "implemented, verified on WARP only, not on hardware" (D29); a WARP run is never
  T1 acceptance (D2) and never a cost.
- P4.6's code merged (PR #70, 2026-10-20) and no `m4o/*` branch remains, so design §3's avoid
  list no longer binds (E16). Still off limits here: `Benchmarks/`, `Tools/bench.py` (P5.5) and
  `docs/conventions/testing.md`, `docs/architecture/overview.md`, `AGENTS.md`, `README.md` (P5.6).
  This part needs no `Source/Core` change; a task that finds it needs one stops and reports.
- Spec chapters stay at 300 lines or fewer (05 is at 293, 06 at 297, 07 at 300, 08 at 297), prose
  at 100 columns; `docs/architecture/d3d12-drawing.md` at 300 or fewer.

## Review Focus

1. A truncated or inconsistent `.d3d12lib` (entry byte counts that do not sum to the `dxil` line,
   a count past the end, an entry whose bytes do not start `DXBC`) is `DriverFailure` naming the
   line, never an out-of-bounds read or a crash in the runtime. Owner: Task 2 ([d3d12] parser
   cases on hand-built blobs).
2. A pipeline-cache blob from another backend, another RojoRHI version, another adapter or
   driver, or truncated after a valid header is one `Warning` and a miss; creation still succeeds
   and a later `pipelineCacheData` is accepted by a new device; a device whose runtime refuses
   pipeline libraries works with every creation a miss. Owner: Task 4.
3. Backend-internal work between the caller's binding and its use: a `fillBuffer` or
   `clearTexture` recorded after `setPipeline`/`setRoot` and before the next dispatch, or after
   `setRoot` and before the next pass, leaves that dispatch or the pass's first draw with the
   caller's pipeline, root and `b1` (one pipeline slot serves graphics and compute on D3D12).
   Owner: Task 7 (dispatch), Task 9 (draw).
4. A resolve target resolved by two consecutive passes, then sampled: every hidden
   `RESOLVE_DEST` transition is undone at `EndRenderPass`, so GBV sees the caller's
   `ColorAttachment` layout each time. Owner: Task 8.
5. A direct draw right after an `ExecuteIndirect` reads `drawId` 0, and a second direct draw
   sends no further `b1` write; `count` 0 draws nothing, `count` > `maxCount` draws `maxCount`.
   Owner: Task 10 ([d3d12] counts) with the existing conformance cases.

---

## Decisions for this part

- **E1. Container bytes** (refines design §5.10's format with a closing line, so the reader never
  reads DXIL as text; mirrors PR #42's S1/S2). ASCII lines, each ended by one LF:
  `rojorhi.d3d12.shaderlibrary`, `abi <N>`, one or more
  `entry <name> <stage> <x> <y> <z> <bytes>`, then `dxil <total>`, then exactly `<total>` bytes to
  end of file: each entry's DXIL container in entry order, `<total>` the sum of the entries'
  `<bytes>`. Stage words, names, sizes and canonical numbers as S1 (`core::stageName` spelling;
  sizes from the SPIR-V `LocalSize`, `0 0 0` for vertex and fragment). File `<Name>.d3d12lib`
  beside the `.dxil` files.
- **E2. Writer checks DXIL.** `Tools/shader_library.py --format d3d12` refuses an entry whose file
  does not start `DXBC`, whose container size field (little-endian `uint32` at byte 24) differs
  from the file's length, or whose 16-byte digest (bytes 4-19) is all zero (unsigned DXIL, which
  the runtime refuses). The `.dxil` per entry is the one the rule already writes
  (`<name>.dxil` for one entry, `<name>.<entry>.dxil` for several), passed as `--dxil NAME=PATH`.
- **E3. Reader rules** (`d3d12::parseShaderLibrary`, S2 in its order): first line not the magic,
  a bare DXBC included: `DriverFailure` "the shader library was not built for the D3D12 backend";
  second line not `abi <number>`: `DriverFailure`; another value: `ShaderAbiMismatch` naming both,
  before any entry line; then seven-token entry lines, `dxil <total>`, the sum, the remaining
  length and each entry's `DXBC` prefix, any breach `DriverFailure` naming the line. Pure C++,
  no Windows header. The payload keeps one copy of the DXIL region and each entry's offset and
  size, in `record.entries` order; the `shaderLibrary` retire hook frees it.
- **E4. `ShaderList.h`** gains `kD3D12LibrariesBuilt` = the rule's `dxil_available`; packaging runs
  wherever DXIL is built (the `d3d12` job, and the `interface` Windows job's SDK DXC), so the D3D12
  `[abi]` cases run there and skip elsewhere.
- **E5. Pipelines through one stream path.** Graphics, mesh and compute pipelines are built as a
  `D3D12_PIPELINE_STATE_STREAM_DESC` and created by `ID3D12Device2::CreatePipelineState` (or the
  library's `LoadPipeline`), so creation and the cache share one path. Subobjects: root signature
  (global); VS+PS, AS+MS+PS or CS (`D3D12_SHADER_BYTECODE` into the library payload, which a
  pipeline keeps referenced only during creation); for graphics `BLEND` (`IndependentBlendEnable`
  true), `RASTERIZER2` (bias fields 0), `DEPTH_STENCIL2`, `RENDER_TARGET_FORMATS`,
  `DEPTH_STENCIL_FORMAT`, `SAMPLE_DESC {samples, 0}`, `SAMPLE_MASK` all ones,
  `PRIMITIVE_TOPOLOGY` (type of the class), `INPUT_LAYOUT` empty (vertex pipelines), `FLAGS`
  `DYNAMIC_DEPTH_BIAS` when `raster.depthBias`. Line rasterization aliased, no strip cut, no
  depth bounds. The payload `d3d12::PipelineState` holds the PSO (+1), the kind (graphics, mesh,
  compute), the class's list topology and `depthBias`, read while recording through
  `pool<Pipeline>().native`. The native compile adds to `pipelineCompileNanoseconds`.
- **E6. Cache blob.** `pipelineCacheData` returns ASCII lines `rojorhi.d3d12.pipelinecache`,
  `version 1`, `rojorhi <major>.<minor>.<patch>`, `library <bytes>`, then the
  `ID3D12PipelineLibrary1::Serialize` bytes. `createDevice` checks the header; any mismatch, or
  `CreatePipelineLibrary` failing on the bytes (`D3D12_ERROR_DRIVER_VERSION_MISMATCH`,
  `D3D12_ERROR_ADAPTER_NOT_FOUND`, `E_INVALIDARG`), is one `Warning` and an empty library
  (06p2:63-64). The device keeps its copy of the library bytes for its life (the library
  references them). If even an empty library is refused (`DXGI_ERROR_UNSUPPORTED`), the device
  has no library: one `Warning`, every creation a miss, and the blob carries `library 0`.
- **E7. Cache key.** 128-bit FNV-1a (offset basis `0x6c62272e07bb014262b821756295c58d`, prime
  2^88 + 0x13b, two `uint64_t` limbs) over canonical key bytes: the kind, every field of the
  description that reaches the stream (formats, states, samples, class, flags) in a fixed order,
  and each stage's entry name and DXBC digest (bytes 4-19). Name = 32 lowercase hex digits as a
  wide string. `LoadPipeline` hit counts `pipelineCacheHits`; `E_INVALIDARG` (absent, or a
  colliding name whose stored desc differs) is a miss: `CreatePipelineState`, then
  `StorePipeline`, whose `E_INVALIDARG` for an existing name is ignored. Both calls run under one
  leaf mutex (creation-time only, never while recording); misses count `pipelineCacheMisses`.
- **E8. Lazy binding as P5.2 left it** (D16). `setRoot`/`pushRoot`, the root-dirty flags,
  `drawIdDirty` and the restore marks after the fill kernel are P5.2's. This part consumes them:
  the next dispatch issues `SetComputeRootShaderResourceView(0, va)` when `computeRootDirty`; the
  next draw `SetGraphicsRootShaderResourceView(0, va)` when `graphicsRootDirty` and
  `SetGraphicsRoot32BitConstant(1, 0, 0)` when `drawIdDirty`. A pipeline the fill kernel displaced
  is set again at the next dispatch. If P5.2 left any of these undone, Task 7 adds it in P5.2's
  files and says so.
- **E9. Starting dynamic state** (refines design §5.9 by 06p2:110-125): `beginRenderPass` writes
  `OMSetStencilRef(0)` and `OMSetBlendFactor(0,0,0,0)` only where the list's held value differs
  (the held values start unknown at acquisition, so the list's first pass writes both), resets the
  recorded depth-bias triple to zero without a call, and writes no viewport or scissor: the
  reference leaves them unset and a draw requires them. Setters filter against the held value.
- **E10. Depth bias.** `setPipeline` with a `depthBias` pipeline issues `RSSetDepthBias` with the
  recorded triple every time, unfiltered (whether D3D12 keeps it across `SetPipelineState` is #22
  item 13); `setDepthBias` records and, with a `depthBias` pipeline bound, issues it.
  `IASetPrimitiveTopology` is filtered against the held value; mesh pipelines set none.
- **E11. Index buffers.** One `D3D12_INDEX_BUFFER_VIEW` of the whole buffer from offset 0
  (`SizeInBytes` = `min(width, UINT32_MAX)` rounded down to the index size), `firstIndex` as
  `StartIndexLocation`, `firstInstance` as `StartInstanceLocation`; `IASetIndexBuffer` is skipped
  when the held (VA, format) is equal. The VA comes from the buffer's payload via `pool.native`.
- **E12. Passes.** `beginRenderPass`: the core's check; the pass's scope through P5.2's scope
  helper (event and timestamp); one enhanced `Barrier` moving every resolve target
  `RENDER_TARGET` → `RESOLVE_DEST` (sync `RENDER_TARGET` → `RESOLVE`, VU-rojo-cmd-0007 puts it in
  `ColorAttachment`); `BeginRenderPass` with `D3D12_RENDER_PASS_RENDER_TARGET_DESC`s from P5.1b's
  per-texture RTV cache and a `D3D12_RENDER_PASS_DEPTH_STENCIL_DESC` from its DSV cache, load ops
  `PRESERVE/CLEAR/DISCARD`, store `PRESERVE/DISCARD`, or `RESOLVE` with `AVERAGE` and
  `PreserveResolveSource` = (store is `Store`) when a resolve target is named (07:139); stencil
  `NO_ACCESS` for a format without stencil; flags none. Resolve parameters live in list-owned
  storage until `EndRenderPass`. `endRenderPass`: `EndRenderPass`, one `Barrier` back to
  `RENDER_TARGET`, the scope's end. `encodersOpened` counts passes begun; each barrier call counts
  one `nativeBarriers`.
- **E13. Indirect.** `ExecuteIndirect(signature, maxCount, args resource, args.hi, count resource
  or null, count.hi)`, both resources from the heap directory (one acquire load each);
  `drawIndexedIndirect` sets the index buffer first (E11); after any draw signature, `drawIdDirty`.
  Mesh direct: `DispatchMesh`; mesh pipelines read `b1` in every stage (ADR 0018:59).
- **E14. Pending lists** (design §7.4). Each recording task removes from
  `d3d12_conformance_pending` the case names its entries make pass, and the CI run must show them
  run; contract files leave `d3d12_pending` only in the sweep tasks (6 and 12), whole files only,
  each remaining one named with its reason in the PR body. A file is never split.
- **E15. Spec cells** change in the commit that implements them, only where the code differs
  from the cell, each named in the report: 05 (stamp, D17; "Shader build"), 06 and 06p2
  (`loadShaderLibrary`, pipelines, cache), 05p3 (`drawId` direct form), 07 and 07p2 (passes,
  `setPipeline`, draws), 08 (indirect), each subject to E16's open-PR check.
- **E16. Open-PR check** (replaces the P4.6 gate; lead's update 2026-10-20). P4.6's code is on
  `main`. Before editing a spec chapter, `AbiComputeConformanceTests.cpp` or a shared test file, a
  task runs `gh pr list --state open` and, for each open P4 or P5 PR touching that file, `gh pr
  diff <n> -- <file>`; it proceeds when no hunk lies within ten lines of its edit (D25), else it
  writes the edit into `.superpowers/p5/spec-amendments.md` and reports. On 2026-10-20 the open
  PRs are #71 (P5.1a) and #75 (build warnings); neither touches these files.
- **E17. Multisample expectation** (design §7.5; closes spec-amendments ledger row 1). `Tests/Support/Backend.h` gains
  `bool multisampleReadsSize();` (Metal 4 true: the pinned Slang's Metal lowering reads the size;
  Null and D3D12 false). The two MS cases clear sample data first through a render pass (clear
  colour (1, 0, 1, 1), barriers as the graphics-barrier cases use) and expect
  `multisampleReadsSize() ? <Metal's value, unchanged> : Float4{1, 0, 1, 1}`; Metal's assertions
  stay exact.
- **E18. Local aids.** Windows-header-free files (the parser, the cache key) syntax-check on the
  Mac with `xcrun clang++ -fsyntax-only`; anything else follows P5.1b's recorded choice on a MinGW
  syntax aid (none unless P5.1b set one up). CI is the compiler.
- **E19. Issues.** #23 closes with P5.3b (load, T3 shaders run on WARP, mismatch rejected). #22
  markers are untouched (P5.6). New deferred work is filed under "P5: D3D12 backend".

## Interfaces this part builds on (volatile)

Written from the design before P5.1b and P5.2 exist. Before a task starts, the orchestrator checks
each row against merged code and amends the brief; a difference the brief does not cover is
NEEDS_CONTEXT.

| Interface (assumed) | Source | Used by |
|---|---|---|
| `d3d12` configure, DXIL mandatory, `d3d12_pending`, test entries `abi`/`contract` | P5.1a (#71) | 1, all |
| `Tools/check_dxil_nonuniform.py`, `Shaders/Tests/AbiNonUniform.slang`, #73's lowering | prework | 1 |
| `DeviceImpl` (`ID3D12Device12* device`, `core::DeviceCore core`, retire-hook block in `createDevice`, message routing to the callback), `d3d12::Com<T>`, `errorOf(HRESULT)`, `setName(object, label)` | P5.1b | 2-4, 7-10 |
| `d3d12::RootSignatures` in `D3D12RootSignature.{h,cpp}` (`global`; `kernel`; the four command signatures, or none: then Tasks 7 and 10 add them) | P5.1b / P5.2 | 3, 7, 10 |
| `d3d12::dxgiFormat(Format)`, the RTV/DSV/view-format helpers, per-texture RTV/DSV cache (`rtv(mip, layer)`, `dsv(mip, layer)`) in the texture payload | P5.1b | 3, 8 |
| heap directory lookup (`Addr` → resource and GPU VA, one acquire load) in `D3D12Addr.h`; buffer payload VA | P5.1b | 7, 9, 10 |
| `death` and `conformance` entries on `d3d12`, `d3d12_conformance_pending`, `Tests/Support/D3D12` surfaces | P5.1b | 2, 7-12 |
| `CommandListImpl` (`ID3D12GraphicsCommandList9* list`, `d3d12::ListState state`), lazy root, `drawIdDirty`, restore marks, scopes with timestamps, the enhanced-barrier helper, `noteNativeCall` sites | P5.2 | 7-10 |

## Tasks

One commit per task with the brief's subject. Model: opus for structural work (pipelines, cache,
lazy binding, passes, indirect), sonnet otherwise.

| # | PR | Task | Model | Depends | Commit subject |
|---|---|---|---|---|---|
| 1 | a | `.d3d12lib` packaging and `[abi]` cases | sonnet | P5.1a | `shader: package DXIL entries in a stamped D3D12 shader library` |
| 2 | a | Load shader libraries; test-support libraries | sonnet | 1 (format), P5.1b | `d3d12: load shader libraries and check their ABI stamp` |
| 3 | a | Pipelines from state streams | opus | 2 | `d3d12: create graphics, mesh and compute pipelines from state streams` |
| 4 | a | Pipeline cache | opus | 3 | `d3d12: cache pipelines in a D3D12 pipeline library` |
| 5 | a | Documents for P5.3a | sonnet | decisions | `docs: describe D3D12 shader libraries and pipelines` |
| 6 | a | Contract sweep for P5.3a | sonnet | 4 | `test: run the pipeline contract files on D3D12` |
| 7 | b | Compute binding, dispatches, `dispatchIndirect` | opus | P5.2, P5.3a | `d3d12: bind compute pipelines and dispatch` |
| 8 | b | Render passes | opus | P5.2, P5.3a | `d3d12: record render passes with their load, store and resolve operations` |
| 9 | b | Graphics state and direct draws | opus | 7, 8 | `d3d12: bind graphics pipelines, dynamic state and direct draws` |
| 10 | b | Indirect draws and mesh tasks | opus | 9 | `d3d12: draw indirectly and draw meshes through ExecuteIndirect` |
| 11 | b | Multisample expectation per backend | sonnet | 8 | `test: expect each backend's multisample read in the ABI cases` |
| 12 | b | Sweep: contract files, graphics-barrier and remaining cases | sonnet | 10, 11 | `test: run the drawing contract and conformance cases on D3D12` |
| 13 | b | Documents for P5.3b | sonnet | decisions | `docs: describe D3D12 passes, draws and indirect drawing` |

## Parallel schedule

Worktrees `../rojo-rhi-worktrees/p5d-<n>` branched from the PR head
when the wave starts; results are cherry-picked onto the PR branch in task order.

| Wave | Tasks | Starts after | Why the files do not overlap |
|---|---|---|---|
| a1 | 1 ∥ 2 (parser half) | P5.1a merged (2's loader half waits for P5.1b) | 1: `Tools/`, `xmake/slang.lua`, `Tests/ShaderLibraryTests.cpp`, `ci.yml`, spec 05; 2: `Backends/D3D12/D3D12ShaderLibrary.*`, `D3D12Pipelines.cpp`, `Tests/Support/D3D12`, `Tests/D3D12` |
| a2 | 3 ∥ 5 | 2 | 3: pipeline files, `DeviceImpl` hooks; 5: `docs/architecture/` only |
| a3 | 4 | 3 | cache files, `createDevice`'s cache lines, `D3D12Pipelines.cpp` |
| a4 | 6 | 4 | `xmake/targets.lua` |
| b1 | 7 ∥ 8 ∥ 13 | P5.2 and P5.3a merged | 7: `D3D12Dispatches.cpp`, compute branch of `D3D12GraphicsState.cpp`, dispatch signature; 8: `D3D12RenderPasses.cpp`, pass fields of `D3D12ListState.h`; 13: docs |
| b2 | 9 ∥ 11 | 7, 8 | 9: graphics branch and setters, `D3D12Draws.cpp`; 11: `Tests/Support`, `AbiComputeConformanceTests.cpp` |
| b3 | 10 | 9 | `D3D12Indirect.cpp`, draw signatures |
| b4 | 12 | 10, 11 | `xmake/targets.lua`, `ci.yml` floors |

Shared files with fixed hunk ownership: `D3D12Device.{h,cpp}` (2 the library hook, 3 the
pipeline hook and payload, 4 the cache member and its creation and release); `D3D12ListState.h`
(8 owns new fields in b1, except the two pipeline fields 7 adds when P5.2 has none, named in its
brief; 9 owns the held graphics state); `D3D12GraphicsState.cpp` (7 creates it with the compute branch of `setPipeline`;
9 owns the graphics branch and every setter); `xmake/targets.lua` (each task removes only its own
conformance names; contract files in 6 and 12 only).

**Early start.** Task 1 needs only P5.1a (#71) merged and runs before P5.1b and P5.2. Task 2's
parser and its [d3d12] parser cases need only P5.1a (the file includes no Windows header). Tasks
2 (loader), 3, 4, 5 and 6 need P5.1b and nothing of P5.2, so P5.3a may run alongside P5.2: the
two touch `D3D12Device.{h,cpp}` at different hunks (P5.2: lists and the kernel PSO; P5.3a: library
and pipeline hooks, the cache) and `xmake/targets.lua` at different list entries; whichever merges
second rebases. P5.3b starts when P5.2 and P5.3a are both on `main`.

## Final shape (reshape)

The plan commit `docs: plan D3D12 drawing` is dropped from each PR.

| PR | # | Subject | Tasks |
|---|---|---|---|
| a | 1 | `shader: package DXIL entries in a stamped D3D12 shader library` | 1 |
| a | 2 | `d3d12: load shader libraries and create pipelines with a pipeline-library cache` | 2, 3, 4, 6 |
| a | 3 | `docs: describe D3D12 shader libraries and pipelines` | 5 |
| b | 1 | `d3d12: bind compute pipelines and dispatch` | 7 |
| b | 2 | `d3d12: record render passes, graphics state and draws` | 8, 9 |
| b | 3 | `d3d12: draw indirectly and draw meshes through ExecuteIndirect` | 10, 12 |
| b | 4 | `test: expect each backend's multisample read in the ABI cases` | 11 |
| b | 5 | `docs: describe D3D12 passes, draws and indirect drawing` | 13 |

Each reshaped commit passes the Mac checks of `common.md`; the Windows checks are CI's run at
HEAD.

## Exit (design §8 row P5.3)

- [ ] P5.3a: the `d3d12` job's `abi` entry runs the D3D12 container cases (not skipped);
      `[d3d12]` library, pipeline and cache cases pass; `PipelineTests` and `PipelineDeathTests`
      pass on WARP under GBV; Release `contract` passes.
- [ ] P5.3b: the ABI, write-only, dispatch, render-pass, draw, indirect, counted, mesh and
      graphics-barrier `[conformance]` cases pass on WARP under the debug layer and GBV, except
      those whose pending reason names P5.4 (presentation, upscale, sparse, capture);
      `[d3d12]` count cases pin native minimum + 1; Release `contract` and `conformance` pass.
- [ ] The `d3d12` job's `[conformance]` and `[d3d12]` case-count floors raised to the run's counts.
- [ ] `docs/architecture/d3d12-drawing.md` Accepted, WARP-only wording; spec cells per E15.
- [ ] #23 closed by P5.3b with the run link; deferred work filed.

## Stop conditions

WARP refuses a floor feature this part needs (mesh shaders, `ExecuteIndirect` with root
constants, pipeline libraries beyond E6's fallback) in a way no reference-allowed `Unsupported`
covers; a conformance assertion would have to change for D3D12 (anti-goal); CI that cannot be made
green honestly; a task blocked after two fix rounds.
