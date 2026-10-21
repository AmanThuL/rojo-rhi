# The D3D12 rules WARP verifies

**Status**: Accepted (2026-10-21)

The API reference marks a D3D12 cell or rule `unverified` when it rests on a native behaviour no
run had confirmed ([conventions](../spec/00-conventions.md#mapping-tables-and-cost-classes)). This
note records the runs that settle some of those markers and the reasons the rest stay. Every run
here used WARP, the software adapter, on hosted `windows-latest`: the D3D12 backend is implemented,
verified on WARP only, not on hardware. None of it is T1 acceptance or a measurement
([ADR 0009](../decisions/0009-conformance-and-valid-usage.md)).

A marker leaves only when all four hold: (a) the rule is defined or enforced by the D3D12 runtime,
the debug layer, GPU-based validation or the compiler, not by a driver or vendor; (b) a CI run on
WARP under the debug layer exercises it with no message; (c) a named `[d3d12]` or `[conformance]`
case, or a tool, covers it; (d) this note records the run. A marker whose rule
depends on a driver or vendor stays until a hardware run; one that lacks (b) or (c) stays with what
is missing named below. Raw logs and their SHA-256 manifests are kept outside the published tree,
under `rojo-rhi-evidence/`.

## Runs

**The floor probe.** CI run
[39434942482](https://github.com/AmanThuL/rojo-rhi/actions/runs/39434942482), job `d3d12`,
2026-10-20, which repeats the probe values of run
[39398881860](https://github.com/AmanThuL/rojo-rhi/actions/runs/39398881860): both probe variants
created a device at feature level 12_2 on the app-local WARP 1.0.21 with Agility 1.619.6, the debug
layer, GPU-based validation and DRED active, and met the floor. The JSON is under
`rojo-rhi-evidence/d3d12-probe/2026-10-20/`, and its `SHA256SUMS` reads:

```
dd3b874bce8730a8ba65b5a461b04d3ea643499dd551526efe8aa12ed8065f53  probe-factory.json
776be6c319818c8f08e472d9907288a1a8c572f109b7820dbffbb8c90c43f643  probe-exports.json
```

It reports resource heap tier 2, dynamic depth bias, `OPTIONS19` heaps of 2,097,152 descriptors,
and no debug-layer error for a raw view of 2^27 or of 2^27 + 1 elements
([the build record](../architecture/d3d12-build.md#the-floor-probe)).

**The full T1 run on WARP.** CI run
[39641225757](https://github.com/AmanThuL/rojo-rhi/actions/runs/39641225757), job `d3d12`
(119364960545), 2026-10-21, ran every contract file and every T1 conformance file except the
draw, indirect and graphics ABI files, which waited for `vertexIndex` to exclude the base vertex.
Its log lines:

- Debug, debug layer and GPU-based validation: `abi` "test cases: 10 | 8 passed | 2 skipped";
  `contract` (`[contract]~[death]` and `[d3d12]~[death]`) "test cases: 578 | 567 passed | 11
  skipped"; `death` "test cases: 555 | 550 passed | 3 skipped | 2 failed as expected";
  `conformance` "test cases: 147 | 136 passed | 11 skipped".
- Release: `contract` "test cases: 588 | 485 passed | 103 skipped"; `conformance` "test cases: 152
  | 142 passed | 10 skipped".
- Debug with the debug layer alone: "All tests passed (402 assertions in 64 test cases)", the
  upscale cases WARP does not complete under GPU-based validation.

The log and the probe JSON of that job are under
`rojo-rhi-evidence/d3d12-t1-warp/2026-10-21/run-39641225757/`, whose `SHA256SUMS` holds the log's
line:

```
c1ef42919285465d30162bca058278a8111513b839fe92f837e1e9f5659490a0  ./d3d12-job.log
```

Run [39654761412](https://github.com/AmanThuL/rojo-rhi/actions/runs/39654761412) (job
119412463144) repeated it with identical counts; its log is under `run-39654761412/` beside it, in
that directory's `SHA256SUMS`:

```
b6d613077ad46fde0ed0d29fcdd8d9a4c1989dd003943f0d5a4ea29863e21130  ./d3d12-job.log
```

"No message" is read from the log. The backend aborts the process on any error or corruption
message of the layer, so a passing step had none. The Debug `conformance` step prints no
`[RHI]` line at all. The Debug `contract` step prints only live-object reports and the
pipeline-cache warning that contract cases provoke, and two pipeline-creation warnings
(`STATE_CREATION` 679 and 1317); no warning of the `STATE_SETTING`, `EXECUTION` or
`RESOURCE_MANIPULATION` categories appears in any Debug step. The `[d3d12]` recording cases cited
below (barriers, graphics state, indirect draws) also collect the messages themselves and check
that there are none.

**DXIL.** `python3 Tools/check_dxil.py` ran in the same job and printed "DXIL checks passed (6
files in 1 directory)": each heap fetch of `Shaders/Tests/AbiNonUniform.slang` carries the
non-uniform flag, and `Shaders/Tests/AbiRawAccess.slang` loads and stores one full-width raw access
each.

**The draw, indirect and graphics ABI files.** CI run
[39660621919](https://github.com/AmanThuL/rojo-rhi/actions/runs/39660621919), job `d3d12`
(119432336868), 2026-10-21, on the tree that added them to the D3D12 run: Debug `conformance`
"test cases: 173 | 162 passed | 11 skipped" (log line 2364), with no `[RHI]` line in that step;
`contract` "test cases: 578 | 567 passed | 11 skipped" (log line 1443). The log is under
`rojo-rhi-evidence/d3d12-t1-warp/2026-10-21/run-39660621919/`, in its `SHA256SUMS`:

```
381bf9c97f411f99a5eb7e34a5ecdf9bdbd0ec00d95596722a1a4e245ba8a4c1  d3d12-job.log
```

[The vertex-index note](2026-10-21-d3d12-vertex-index-warp.md) records the
earlier run that showed `SV_VertexID` excluding the base vertex.

**The FSR probe.** CI run
[39463099719](https://github.com/AmanThuL/rojo-rhi/actions/runs/39463099719), 2026-10-20, and
run 39466444833: without GPU-based validation FidelityFX creates its context and dispatches on
WARP with no debug-layer message; with it the submission did not complete within the probe's time
limit. The JSON is under `rojo-rhi-evidence/d3d12-fsr-probe/2026-10-20/`, and its `SHA256SUMS`
reads, for the factory probes:

```
d5a0ba9d47df3862142c7d850e33cf07996f513e7c14c71947e525ba14613597  run-39463099719/probe-factory.json
4f8d456b8cb3ddcefc213e4c4969ebd322176a32b081cece36f121ccab295e89  run-39466444833/probe-factory.json
```

`Tools/bench.py` has no D3D12 arm on 2026-10-21; timing waits for a Windows GPU machine (#21),
and no benchmark run is evidence here.

## Markers

Cites are chapter and line of `docs/spec/` as they stood on 2026-10-21 before this note's markers
were removed (`02:90` is `02-device.md` line 90; `p2` and `p3` are parts 2 and 3). In the run
column, "contract" and "conformance" name a run's Debug steps above, with their cases and passes.
Items 1 to 21 are the D3D12 markers issue #22 lists; A and B are the two FSR markers of chapter
12 outside that list.

| Item | Cite | Rule | Enforced by | Case or tool | Run | Decision |
|---|---|---|---|---|---|---|
| 1 | 02:90; 06p2:258, 272 | `DynamicDepthBiasSupported` on every vendor's driver | driver | the probe reports it on WARP; `createDevice` checks it | probe | stays, hardware: support per vendor is a driver's, and WARP is no vendor |
| 2 | 02:171; 15p2:49 | the `OPTIONS19` heap sizes devices ship | driver | the probe reports 2,097,152 on WARP | probe | stays, hardware: shipping values are drivers' |
| 3 | 03p2:29 | `ALLOW_ALL_BUFFERS_AND_TEXTURES` needs resource heap tier 2 on every floor device | driver | the probe reports tier 2 on WARP; `createDevice` checks it | probe | stays, hardware: the tier is per vendor |
| 4 | 03:169-170; 15p2:55, 85-91 | an `Addr` reaches 2^29 − 1 bytes, the 2^27 elements of one raw view | runtime (`D3D12_REQ_BUFFER_RESOURCE_TEXEL_COUNT_2_TO_EXP`) | `[d3d12]` "a fill across the raw view's 512 MiB reach writes every word on both sides"; the probe's raw views | 39641225757, contract | no marker: the text already states 2^29 − 1; the layer raised no error at 2^27 + 1, so the core's `VU-rojo-buffer-0011` is the check |
| 5 | 05:268 | `root<T>` reads a raw `ByteAddressBuffer` root SRV at `t0` | runtime, compiler | `[conformance]` "AbiCompute loads, stores and reads the ABI version through the root"; `[d3d12]` "the global root signature holds the root SRV at t0 and one root constant at b1" | 39641225757, conformance 147 / 136, contract 578 / 567 | removed |
| 6 | 05:273-296 | the ABI stamp lives in the `abi` line of the `.d3d12lib` container | RojoRHI's loader | `[abi]` "the D3D12 shader libraries carry each entry's stage, threadgroup size and DXIL"; `[d3d12]` "a library stamped with another ABI is ShaderAbiMismatch before a malformed entry line" | 39641225757, abi 10 / 8 | no marker: the text already states it |
| 7 | 05p2:44 | DXC keeps `NonUniformResourceIndex` through the helper and drops it through the store's `+ 1` | compiler | `Tools/check_dxil.py` | 39641225757, "DXIL checks passed (6 files in 1 directory)" | no marker on the compiler's part; "on a device unverified" stays: whether a divergent index reads the right descriptor is the driver's, and no case reads through an index that differs between threads |
| 8 | 05p2:93, 120, 173, 205, 233 | `load`, `store`, `texture`, `wTexture` and `sampler` lower to `ResourceDescriptorHeap` and `SamplerDescriptorHeap` fetches with `LoadAligned` and `StoreAligned` | runtime, compiler | `[conformance]` "AbiCompute loads, stores and reads the ABI version through the root", the `AbiTextures` cases ("AbiTextures texture2DMain samples a 2D texture" and the other shapes), "each write-only form stores one texel of an RGBA16Float view and leaves the rest"; `Tools/check_dxil.py` | 39641225757, conformance 147 / 136 and the DXIL line | removed from the D3D12 cells; 05p2:173's Metal marker is not D3D12's |
| 9 | 05p3:52-53 | `VertexIds` is `SV_VertexID` and `SV_InstanceID`, with no base, at native cost | runtime | `[conformance]` "draw writes each instance of each vertex range with drawId 0, firstVertex and firstInstance invisible", "AbiVertex stores vertexIndex, a zero-based instanceIndex and drawId 0 for draw and drawIndexed" | 39660621919, conformance 173 / 162 (log line 2364) | removed from the lowering; the Cost cell keeps its marker, because WARP measures no cost |
| 10 | 05p3:78-79; 07p2:10-11; 08:75-77, 112-115 | `SV_VertexID` excludes the base vertex, direct and indirect | runtime | `[conformance]` "drawIndexed applies firstIndex with UInt16 and UInt32 indices, baseVertex invisible", "drawIndexedIndirect draws min(count, maxCount) records with firstIndex, baseVertex invisible" | 39660621919, conformance 173 / 162 (log line 2364) | no marker: [ADR 0024](../decisions/0024-vertex-index-excludes-the-base.md) rewrote the text, which says "verified on WARP only" |
| 11 | 05p3:101 | `SV_InstanceID` excludes the base instance | runtime | `[conformance]` "draw writes each instance of each vertex range with drawId 0, firstVertex and firstInstance invisible", "AbiVertex reads the record's drawId through drawIndirect, firstInstance invisible" | 39660621919, conformance 173 / 162 (log line 2364) | removed; "taken to exclude" now reads "which excludes" |
| 12 | 05p3:127-128 | `drawId` is the `b1` root constant the command signature writes from record offset 0, 0 for a direct draw | runtime | `[conformance]` "AbiVertex reads the record's drawId through drawIndirect, firstInstance invisible", "AbiFragment adds the record's drawId", "a direct mesh draw after a counted one in the same pass keeps drawId 0"; `[d3d12]` "a direct draw after an indirect draw writes b1 once; a second direct draw writes nothing more" | 39660621919, conformance 173 / 162 (log line 2364); 39641225757, contract 578 / 567 | removed from the lowering; the Cost cell keeps its marker, because WARP measures no cost |
| 13 | 06p2:252 | D3D12 keeps depth-bias values across `SetPipelineState` | runtime | none: `[d3d12]` "a depthBias pipeline re-issues the recorded bias at every setPipeline; a pipeline without depthBias sends none" shows the backend never relies on it | none | stays: WARP evidence missing: a case that changes the pipeline without re-issuing the bias and reads the depth |
| 14 | 09:104-106 (`VU-rojo-sync-0001`) | a Compute list's barriers name only `IndirectArgs`, `ComputeShader`, `Copy` and `All`; a Copy list's only `Copy` and `All` | debug layer, GPU-based validation | `[d3d12]` "a Compute list records every stage and layout VU-rojo-sync-0001 and 0007 allow", "a Copy list records Copy and All on General textures and on buffers", "Copy drops render-target and depth-stencil sync on a Compute list and is SYNC_COPY alone on a Copy list"; `[conformance]` "barriers order copies on the Compute and Copy queues as on Graphics" | 39641225757, contract 578 / 567, conformance 147 / 136 | removed; the rule stays as conservative as it was |
| 15 | 09:228 (`VU-rojo-sync-0006`) | a texture a Copy list uses is in `General` | debug layer, GPU-based validation | `[d3d12]` "a Copy list records Copy and All on General textures and on buffers"; `[conformance]` "a new texture written on the Copy queue with no barrier is read back on the Graphics queue", "a new texture written on the Copy queue is moved from General on the Graphics queue and read back" | 39641225757, contract 578 / 567, conformance 147 / 136 | removed |
| 16 | 09:230-231 (`VU-rojo-sync-0007`) | a Compute list's barriers use only `Undefined`, `General`, `ShaderRead`, `ShaderWrite`, `CopySrc` and `CopyDst` | debug layer, GPU-based validation | `[d3d12]` "a Compute list records every stage and layout VU-rojo-sync-0001 and 0007 allow" | 39641225757, contract 578 / 567 | removed |
| 17 | 09p2:143-147 | a barrier from `Undefined` with `AccessBefore` `NO_ACCESS` waits for the writes of its `SyncBefore` stages | driver | `[d3d12]` "a barrier from Undefined discards and names no earlier access" shows the layer accepts the lowering | 39641225757, contract 578 / 567 | stays, hardware: the wait is the driver's and neither layer checks ordering; WARP evidence is also missing: a case that fails when the barrier does not wait |
| 18 | 09p2:208-209 | a discard from `Undefined` names no earlier layout, on another queue too | GPU-based validation | `[d3d12]` "a barrier from Undefined discards and names no earlier access" covers one queue only | 39641225757, contract 578 / 567 | stays: WARP evidence missing: a `[d3d12]` or `[conformance]` cross-queue discard case (the `[contract]` cases at `HandoffTests.cpp:219` and `:243` ran under GPU-based validation without a message) |
| 19 | 09p2:215-217 | `LAYOUT_COMMON` is a layout every queue type accepts, for the copy-queue handoff | debug layer, GPU-based validation | `[conformance]` "a new texture written on the Copy queue with no barrier is read back on the Graphics queue"; `[contract]` "a new texture uploaded on Copy is read on Graphics in General and then moved from General"; `[d3d12]` "a Copy list records Copy and All on General textures and on buffers" | 39641225757, contract 578 / 567, conformance 147 / 136 | removed; "taken to be the one layout" now reads "which every D3D12 queue type accepts" |
| 20 | 12:21-25, 49, 51, 73-75, 115-116, 133, 176, 212 | upscaler SDK identifiers | SDK headers | FSR: checked against the FidelityFX SDK v2.3.0 headers; the 64 cases the layer-only configure runs from six files, `UpscalerTests.cpp`, `UpscaleConformanceTests.cpp` and `D3D12UpscalerTests.cpp` among them | 39641225757, "All tests passed (402 assertions in 64 test cases)"; the FSR probe | FSR: no marker in the D3D12 column. DLSS and XeSS stay, hardware: their SDKs are not compiled in. The Vulkan cells' markers are not D3D12's |
| 21 | 15:9-11, 113-119; 15p2:7-9 | the guaranteed format table and the D3D12 limits hold on hardware | driver | `[conformance]` `FormatConformanceTests.cpp` checks what WARP reports | 39641225757, conformance 147 / 136 | stays, hardware: WARP's format support is not a vendor's |
| A | 12:173 | a `D32FloatS8` depth reaches FSR as `R32_FLOAT` through `R32_FLOAT_X8X24_TYPELESS` | runtime, SDK | none | none | stays: WARP evidence missing: an upscale case with a `D32FloatS8` depth |
| B | 12:174 | `jitterOffset` takes `jitterX` and `jitterY` unchanged, y down | SDK | none | none | stays: WARP evidence missing: a case with a nonzero jitter on an input that can show its sign |

## What stays

Items 1, 2, 3, 7 (on a device), 17, 20 (DLSS and XeSS) and 21 rest on a driver or a vendor and
need a hardware run; items 13, 18, A and B need a case first and can be settled on WARP once one
exists. The Cost cells of items 9 and 12 keep their markers: WARP measures no cost, and every
D3D12 Cost cell stays a design estimate until the T2 round on hardware. The other D3D12 lowerings
and rules of the reference carry no marker and are verified on WARP only, as
[conventions](../spec/00-conventions.md#mapping-tables-and-cost-classes) states.

Markers that are not D3D12's are not judged here, cited as above: the Metal 4 cells and sentences
at 01p2:63, 02:171, 02:174, 05p2:173, 08:255, 15:105, 15:137 and 15p2:50, 62, 65, 69, 70, 97; the
Vulkan cells at 02:171, 03:61, 04p2:77, 08:212, 213, 244, 245, 276, 277, 299, 300 and in chapter
12; and appendix B's ray-tracing names at B:54. The sentences at 15:9 and 15p2:8 introduce the
marker for item 21's values.
