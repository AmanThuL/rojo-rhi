# Implementation language, API ABI, dispatch, and overhead measurement — Part B: overhead measurement methodology, open questions and sources

**Status**: Proposed

Part 3 of 3; the summary and the list of parts are in [07-language-and-measurement.md](07-language-and-measurement.md).

## Part B: overhead measurement methodology

### B.1 What the consumer's harness already establishes

Luminex's M5.1 spec ("RHI Execution-Model Decision"), its evidence record ("M5.1 execution-model
evidence"), M5.2's frame-data spec and record, and the `Benchmarks/FrameData` plus `Tools/Bench`
code define a protocol worth adopting nearly unchanged:

- **Frozen before measuring.** The protocol freezes workloads, case identifiers, seeds, schedules,
  hypotheses (H1–H6), rubric, gates and thresholds before any measurement. Any change to interface,
  workload, shaders or instrumentation invalidates collected data; round 1 and round 2 were in fact
  withdrawn under that rule.
- **Paired runs.** Each repetition runs both sides in fresh processes, back to back, alternating
  AB/BA. There are 12 pairs, each with 16 warm-up and 256 measured frames, and the statistic is the
  per-repetition median frame time.
- **Decision statistic.** The median of the paired percentage deltas, with a two-sided 95%
  bootstrap CI (10,000 resamples, fixed seed). A win is material at ≥25% with the CI excluding zero.
  Asymmetric guards apply where needed, for example an upper CI bound of no more than +5% on cases
  that must not regress.
- **Exact counters.** Calls, bytes, address binds, page and buffer creations, barriers and API
  surface concept counts are exact, and material at ≥25%.
- **Correctness first.** Byte-identical readback digests are compared, and the driver refuses to
  report timing when they disagree. Gates (correctness, diagnosable misuse, capture quality, honest
  mappings) apply before any benefit is scored.
- **Recorded environment.** Each round records the environment: hardware, OS build, Xcode, Slang,
  power and thermal state. Validation is off for performance runs and on for correctness runs, and
  never mixed within a run.
- **Diagnosis by neutralization.** Hypotheses are tested by building variants, not by profiler
  intuition. The cross-TU-call hypothesis was disproved this way, and out-of-sample checks confirmed
  the fitted per-call costs.

The gaps to close for RojoRHI:

- **No native baseline.** Overhead is only ever relative to the incumbent.
- **No per-operation breakdown.** The protocol measures whole frames. Per-category slopes come only
  from ad-hoc fits, such as M5.2's `a·n + b·n²` fit.
- **Wall-clock only.** CPU time is wall time of the timed region, with no thread CPU time,
  instruction count or hardware counters, and no tail metric (p95/p99).
- **Hidden pipelining.** A per-frame `waitIdle` isolates encoding but hides CPU/GPU overlap and
  frame-pacing behaviour.
- **Duplicated statistics.** The bootstrap is reimplemented in two drivers (`frame_data_paired.py`,
  `visibility_paired.py`, the latter reused by `lighting_paired.py`).
- **Thermal ordering effects.** One M3 Max produced time-window drift that depended on case order
  (F-STATIC-4096).
- **No CI execution.** Hosted macOS runners have no Metal 4, as RojoRHI's own `ci.yml` notes.

### B.2 How other RHIs and engines measure overhead

| Project | What it measures | Design worth copying |
|---|---|---|
| ANGLE `angle_perftests` ([README](https://chromium.googlesource.com/angle/angle/+/refs/heads/main/src/tests/perf_tests/README.md), [`DrawCallPerf.cpp`](https://chromium.googlesource.com/angle/angle/+/refs/heads/main/src/tests/perf_tests/DrawCallPerf.cpp)) | Draw loop × state-change category (vertex buffer, texture, program, uniform, scissor, ...) × backend × {passthrough, offscreen, null device}; plus `validation_only` | **Null drivers** "skip the driver entirely" and are "useful for diagnosing performance overhead in ANGLE code". Results go to the Chromium perf dashboard, with a repeat-until-low-variance runner. |
| Dawn `dawn_perf_tests` ([testing](https://dawn.googlesource.com/dawn/+/HEAD/docs/dawn/testing.md)) | Time per iteration (draw, upload, frame); DrawCallPerf varies vertex buffer, bind group and pipeline switching and render bundles | Per-operation unit; validation on and off as a parameter |
| wgpu-benchmark ([benches](https://github.com/gfx-rs/wgpu/tree/trunk/benches)) | Render pass encoding at 10k draws and 90k resources; compute pass; bind groups; resource creation | Saved baselines (`--save-baseline v28`, `--baseline`); Tracy and Superluminal hooks |
| Godot ([godot-benchmarks](https://github.com/godotengine/godot-benchmarks)) | Scene-level rendering and scripting benchmarks with JSON output | Tracked over time on fixed hardware ([dashboard](https://benchmarks.godotengine.org)) |
| bgfx ([17-drawstress](https://github.com/bkaradzic/bgfx/tree/master/examples/17-drawstress)) | Increases draw count until the frame budget is exceeded | Capacity metric ("draws per budget") |
| 3DMark API Overhead ([AnandTech](https://www.anandtech.com/show/11223)) | Draws per second before the frame rate falls below 30 FPS, per API | Native-API ceiling. Not for cross-vendor comparison, and artificial by its authors' own description. |
| NRI ([README](https://github.com/NVIDIA-RTX/NRI)) | No published overhead numbers. Its samples double as tests. | `NONE` backend and a validation layer as a wrapper backend |
| Diligent, The Forge, NVRHI | No published RHI-versus-native overhead measurements found | — |

No surveyed RHI publishes a systematic "RHI versus hand-written native" overhead figure. The closest
practice is ANGLE's null-driver split of front-end and driver cost, and per-operation timing with
saved baselines (Dawn, wgpu).

### B.3 Tools

| Need | Apple Silicon (Metal 4) | Windows (D3D12) |
|---|---|---|
| Steady-state CPU timing | `std::chrono::steady_clock` around the timed region (current practice); Catch2 `BENCHMARK` for microbenchmarks, which bootstraps a CI and is already a dependency ([docs](https://github.com/catchorg/Catch2/blob/devel/docs/benchmarks.md)) | Same code; also QueryPerformanceCounter |
| CPU attribution | Instruments Time Profiler and CPU Counters (`xctrace` from the command line); [Tracy](https://github.com/wolfpld/tracy) CPU zones | Superluminal, WPA/ETW, VTune or AMD uProf, Tracy, PIX timing captures |
| GPU timing | `MTL4CounterHeap` timestamps ([docs](https://developer.apple.com/documentation/metal/mtl4counterheap)), already used by `Device::passTimings()`; Metal System Trace; Xcode GPU capture counters | D3D12 timestamp queries plus `GetTimestampFrequency` ([timing](https://learn.microsoft.com/en-us/windows/win32/direct3d12/timing)); PIX GPU captures; [RGP](https://gpuopen.com/rgp/); [Nsight Graphics](https://developer.nvidia.com/nsight-graphics) GPU Trace |
| Clock stability | None to set. Control power source, Low Power Mode, thermal state, QoS class of the encoding thread, and case order. | `SetStablePowerState` (developer mode only, clocks well below shipping speeds, [docs](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-setstablepowerstate)). NVIDIA advises locking core and memory clocks via Nsight instead ([NVIDIA](https://developer.nvidia.com/blog/advanced-api-performance-setstablepowerstate)). |
| Deterministic counts | Backend counters such as `FrameDataCounters`; an `operator new` hook for allocations | Same; also `cachegrind`-style instruction counts if a Linux null build ever exists |
| Build cost | `-ftime-trace`; binary and section sizes (`size -m`, bloaty) | MSVC Build Insights (`vcperf`); `/d1reportTime`; dumpbin |

Tracy's Metal GPU zones (`TracyMetal.hmm`) use `MTLCounterSampleBuffer`, the pre-Metal 4 path, so
GPU zones for `MTL4` command buffers would need RojoRHI's own counter-heap readout. Its CPU zones
work as is.

### B.4 Metrics

Each metric is declared as either *time* (paired statistics) or *count* (exact) before collection,
as in Luminex's rubric.

- **CPU per operation (time).** ns per draw, dispatch, bind (per slot kind), root or frame-data
  push (per size), pipeline switch, barrier or fence, pass begin/end, resource create and destroy,
  transient allocation, and submit. Estimate each as the slope of frame time over operation count
  (fit `t(n) = a + b·n` over n ∈ {256, 1,024, 4,096}), so fixed frame costs cancel. Add `b₂·n²` when
  growth is expected; M5.2's registry scan is the reason.
- **CPU per frame (time).** Timed-region median and p95 for the representative frame, in two modes:
  isolated (per-frame `waitIdle`, as now) and pipelined (three frames in flight, measuring pacing
  wait separately).
- **Overhead versus native (time).** `(T_rhi − T_native) / T_native` per category and per frame,
  where the native baseline encodes the identical command sequence directly in metal-cpp (B.5).
- **Native call amplification (count).** Native API calls issued per RHI call and per frame, and the
  ratio to the native baseline's count. It is deterministic, cheap, and the best early proxy on the
  D3D12 paper mapping.
- **Bytes written (count).** Binding and root bytes, command-stream bytes, and uploaded bytes per
  frame.
- **Allocations (count).** Heap allocations and GPU object creations in steady-state frames. The
  target is zero after warm-up, which M5.2 already asserts for arena pages.
- **Lookups (count).** Hash and map lookups and linear scans per operation, instrumented in debug
  counters. The O(n) registry scan would have shown here first.
- **Synchronization (count).** Barriers, fences and encoder or pass splits per frame for the frozen
  hazard set.
- **GPU time (time, descriptive unless frozen otherwise).** Per-pass timestamps. Indirection added
  by the API shape (extra argument-table hops, extra pass splits, extra barriers) shows up here, not
  on the CPU.
- **PSO and compile behaviour (descriptive).** Cold and warm pipeline creation, and stalls on first
  use. M5.1 rightly kept this out of the thresholds because driver caches are not controllable.
- **Build footprint (count).** Library and test binary size, preprocessed size and parse time of
  `<rojoRHI/RHI.h>`, and full-build time.

### B.5 Comparing candidate designs before committing

The aim is to rank two or three candidate API shapes and the dispatch options cheaply, on Metal
first, without building a production backend for each.

1. **Freeze first.** Write a short spec in the M5.1 style before writing prototype code: hypotheses,
   workloads, metrics, thresholds and gates, with ties going to the simpler design. For dispatch, a
   reasonable frozen rule is "adopt link-time selection unless the representative frame regresses
   (upper CI > +2%)". For API shape, keep M5.1's ≥25% materiality on the scored core dimensions.
2. **One manifest, several adapters.** Keep an API-neutral workload manifest (resources, passes,
   draw policy, seeds), as in M5.1, and one adapter per candidate plus a **native adapter** written
   directly in metal-cpp. The native adapter is the floor: same commands, same shaders, same digest.
   Overhead for each candidate is then reported both against the incumbent and against native.
3. **Microbenchmarks per category** (B.4 list), each running n ∈ {256, 1,024, 4,096} operations per
   frame and reporting the fitted slope. Use Catch2 `BENCHMARK` for quick in-process iteration
   during design. Scored numbers come from the paired external driver, because Catch2's statistics
   are not paired across two builds.
4. **One representative frame.** Reuse M5.1's frozen 13-pass graph, or its S-BIND and FrameData
   workloads, rather than inventing new ones, so results stay comparable with recorded history.
5. **Dispatch ablation as a cheap side-experiment.** Put the same Metal 4 backend behind three
   front doors (virtual, proc table, direct `final` plus ThinLTO) in one build tree with a build
   option. It costs about a day and answers A.7 item 4 with real command-list code instead of the
   A.6 probe.
6. **Paper D3D12 column.** For every candidate operation, record the D3D12 calls it would emit (for
   example, one `bindFrameData` → one `SetGraphicsRootConstantBufferView`). The amplification metric
   can then be compared across candidates before any Windows code exists. Classify each mapping
   native, emulated, unavailable or unknown, as M5.1 did.
7. **Shared statistics module.** Move the paired bootstrap, digest gate, environment capture and
   drift check (first 32 against last 32 frames) into one RojoRHI `Tools/Bench` module with a
   self-test. Each benchmark executable then only emits the JSON schema `FrameDataBench` already
   uses.

### B.6 Guarding overhead after the redesign

| Tier | Where | What runs | Gate |
|---|---|---|---|
| 0, every PR | Hosted CI (no Metal 4) | Build-footprint metrics (binary size, header preprocessed lines and parse time); API-surface counts parsed from `Include/rojoRHI/`; self-tests of the statistics module | Fail on unexplained growth above a recorded budget (for example, +5% header parse time, +2% library size) |
| 1, pre-merge for performance-relevant changes | Developer Mac, or a self-hosted Apple Silicon runner | `xmake bench --quick`: microbenchmark slopes and the representative frame, 5 paired repetitions of the candidate against its merge-base built in a worktree; exact counters (native calls, bytes, steady-state allocations = 0) | Counters: exact match or an explained change. Time: fail if the upper 95% CI of the slowdown exceeds +5% on any guarded case; flag 2–5%. |
| 2, milestone or release | Same Mac under the full frozen protocol; a Windows GPU machine once D3D12 exists | 12-pair collection, environment record, native-baseline overhead table, GPU pass times | The frozen thresholds of the governing spec; results published in the milestone record |

The `xmake bench` task and the shared module do not exist yet; they would arrive with the work that
adds them. Raw results stay outside the published tree (documentation conventions), for example as
CI artifacts or on a results branch. Counter-based checks carry most of the continuous protection
because they are deterministic and run without noise control. Timing gates guard what counters
cannot see, such as the write-combined regression, which changed no count.

Noise control on the Mac:

- AC power, Low Power Mode off, no other heavy processes;
- the encoding thread at `QOS_CLASS_USER_INTERACTIVE` so it stays on performance cores;
- fresh process per side, AB/BA alternation, and case-order rotation across collections, because of
  F-STATIC-4096's time-window drift;
- thermal state recorded at start and end, and the first-32/last-32 ratio reported per run.

### B.7 What one Apple Silicon Mac can and cannot establish

| Question | One M-series Mac | Needs a Windows GPU machine |
|---|---|---|
| Candidate API shapes on Metal 4: CPU, counts, GPU pass time, correctness | Yes | — |
| Overhead against native Metal 4 | Yes | — |
| Dispatch mechanism on arm64 with Apple clang and ThinLTO | Yes | — |
| Dispatch cost on x86-64 with MSVC, `/GL` and CFG | No; the branch predictor, CFG and compiler differ | Yes |
| D3D12 call amplification (paper) | Yes, analytically | Verify on hardware |
| D3D12 CPU cost of descriptor-heap writes, root signature changes and barrier batching | No | Yes, ideally on NVIDIA, AMD and Intel, since driver cost varies widely |
| D3D12 GPU timing with stable clocks, PIX/RGP/Nsight analysis | No | Yes |
| Cross-machine generalization (other M-series, core counts) | Partly; repeat on a second Mac if available | — |
| Continuous GPU timing in CI | Only with a self-hosted Mac runner | Only with a self-hosted Windows runner |

Until a Windows machine exists, D3D12 claims stay at the "paper mapping plus amplification count"
level, and no document states D3D12 overhead figures (ADR 0004, documentation conventions).

## Open questions

- Should the null/test backend be accepted by ADR as a test double so hosted CI can run counter
  benchmarks without Metal 4, or should counters stay on the self-hosted Mac?
- Which exact thresholds should the redesign spec freeze for dispatch (proposed above: no
  regression beyond +2% upper CI) and for API shape (M5.1's 25%)?
- Does Luminex ship Windows builds with CFG? That decides whether B.7's CFG measurement matters.
- `docs/conventions/documentation.md` defines no research document type for RojoRHI. Durable
  conclusions from this note need to move into an ADR or plan if adopted.

## Sources

Language and ABI:
[SakuraEngine CGPU design note](https://github.com/SakuraEngine/SakuraEngine/blob/engine/agent_docs/graphics/cgpu_design.md),
[CGPU README](https://github.com/SakuraEngine/SakuraEngine/blob/engine/engine/modules/engine/graphics/include/SkrGraphics/README.md),
[CGPU `cgpu.c`](https://github.com/SakuraEngine/SakuraEngine/blob/engine/engine/modules/engine/graphics/src/common/cgpu.c),
[NRI](https://github.com/NVIDIA-RTX/NRI),
[SDL3 GPU](https://wiki.libsdl.org/SDL3/CategoryGPU),
[sokol_gfx](https://github.com/floooh/sokol/blob/master/sokol_gfx.h),
[webgpu-headers](https://github.com/webgpu-native/webgpu-headers),
[The Forge releases](https://github.com/ConfettiFX/The-Forge/releases),
[Diligent C API tutorial](https://github.com/DiligentGraphics/DiligentSamples/tree/master/Tutorials/Tutorial03_Texturing-C),
[metal-cpp](https://developer.apple.com/metal/cpp/),
[Apple C++ support](https://developer.apple.com/xcode/cpp/),
[cppreference C++23 support](https://en.cppreference.com/w/cpp/compiler_support/23),
[MSVC Build Tools 14.51 C++23](https://devblogs.microsoft.com/cppblog/c23-support-in-msvc-build-tools-14-51),
[arewemodulesyet](https://arewemodulesyet.org/tools/),
[xmake C++ modules](https://xmake.io/examples/cpp/cxx-modules.html),
[xmake Rust](https://xmake.io/examples/other-languages/rust.html),
[rustc linker-plugin LTO](https://doc.rust-lang.org/rustc/linker-plugin-lto.html),
[objc2-metal](https://github.com/madsmtm/objc2/tree/main/framework-crates/objc2-metal),
[windows-rs](https://github.com/microsoft/windows-rs).

Dispatch:
[wgpu CHANGELOG](https://github.com/gfx-rs/wgpu/blob/trunk/CHANGELOG.md),
[wgpu #6002](https://github.com/gfx-rs/wgpu/pull/6002),
[wgpu #5124](https://github.com/gfx-rs/wgpu/issues/5124),
[Godot `rendering_device_driver.h`](https://github.com/godotengine/godot/blob/master/servers/rendering/rendering_device_driver.h),
[GPUOpen: reducing Vulkan API call overhead](https://gpuopen.com/reducing-vulkan-api-call-overhead/),
[volk](https://github.com/zeux/volk),
[Bendersky: virtual vs CRTP](https://eli.thegreenplace.net/2013/12/05/the-cost-of-dynamic-virtual-calls-vs-static-crtp-dispatch-in-c),
[Aras Pranckevičius: the virtual and no-virtual](https://aras-p.info/blog/2011/02/01/the-virtual-and-no-virtual/),
[Control Flow Guard](https://learn.microsoft.com/en-us/windows/win32/secbp/control-flow-guard).

Measurement:
[ANGLE perf tests](https://chromium.googlesource.com/angle/angle/+/refs/heads/main/src/tests/perf_tests/README.md),
[Dawn testing](https://dawn.googlesource.com/dawn/+/HEAD/docs/dawn/testing.md),
[wgpu benches](https://github.com/gfx-rs/wgpu/tree/trunk/benches),
[godot-benchmarks](https://github.com/godotengine/godot-benchmarks),
[bgfx drawstress](https://github.com/bkaradzic/bgfx/tree/master/examples/17-drawstress),
[AnandTech 3DMark API overhead](https://www.anandtech.com/show/11223),
[Tracy](https://github.com/wolfpld/tracy),
[MTL4CounterHeap](https://developer.apple.com/documentation/metal/mtl4counterheap),
[D3D12 timing](https://learn.microsoft.com/en-us/windows/win32/direct3d12/timing),
[SetStablePowerState](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device-setstablepowerstate),
[NVIDIA on SetStablePowerState](https://developer.nvidia.com/blog/advanced-api-performance-setstablepowerstate),
[Catch2 benchmarks](https://github.com/catchorg/Catch2/blob/devel/docs/benchmarks.md),
[Sebastian Aaltonen: No Graphics API](https://www.sebastianaaltonen.com/blog/no-graphics-api).
