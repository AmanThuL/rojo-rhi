# Spike 2: how Metal 4 indexes textures

**Status**: Proposed (until spike 2 of ADR 0014 reports)

Spike 2 of [ADR 0014](../decisions/0014-overhead-measurement.md) decides the Metal 4 lowering of a
`ViewIndex` ([ADR 0011](../decisions/0011-shader-abi.md),
[texture fetch by index](../spec/05-shader-abi-part2.md#heap-indices)). This note is the spike's
pre-registration: the workloads, metrics, statistics and decision rule below are fixed before any
measurement runs, and changing them invalidates the round. The results section is filled in when
the spike reports; nothing above it changes afterwards.

## Question

Does the contiguous `base + i` resource ID of a `MTL::TextureViewPool` cost materially less than
the documented table of 8-byte `MTL::ResourceID`s, enough to justify relying on undocumented ID
arithmetic?

## Candidates

| Arm | Shader access | CPU publish of one texture | Documented |
|---|---|---|---|
| F, fixed slots | `array<texture2d<float>, N>` bound at consecutive argument-table texture slots, indexed dynamically | `MTL4::ArgumentTable::setTexture(resourceID, slot)` | yes |
| T, ID table | `constant texture2d<float>* table [[buffer(k)]]`; `table[i]` | `MTL::Texture::gpuResourceID()` written as 8 bytes into a shared buffer | yes |
| P, view pool | a texture handle formed from `baseResourceID + i`, `baseResourceID` passed as a 64-bit constant | `MTL::TextureViewPool::setTextureView(texture, i)` | no |

F is the native floor and exists only up to the argument table's texture limit, which the harness
reads from the device and records; above it, T and P are compared with each other, since both are
how hand-written Metal 4 indexes many textures. RojoRHI adds nothing over T or P on the GPU, so
the spike measures the mechanisms, not a RojoRHI tax. If MSL offers no defined way to form P's
handle from an integer, P fails the correctness gate and that is the result.

Samplers have no pool on Metal 4, so `SamplerIndex` lowers through a table either way and is not
decided here. One secondary variant fetches the sampler from a table instead of a `constexpr`
sampler; it is reported for the cost summary and decides nothing.

## Workloads

All arms share one host, one set of textures and one kernel body; only the texture access differs.

- **Textures**: N distinct 16×16 `RGBA8Unorm` textures, private storage, each filled with a value
  that encodes its own index so the output identifies the texture read.
- **Kernel**: a 1920×1080 grid, one thread per element. Each thread reads 8 texels, each from the
  texture its index function selects, and writes their sum.
- **Access**: `read` at an integer coordinate; and `sample` with a bilinear `constexpr` sampler at
  a fractional coordinate.
- **Index pattern**: uniform, where every thread of a SIMD group uses the same index; and
  divergent, where each thread hashes its position to an index.
- **N sweep**: 1, 16, 128, 1,024, 16,384 and 100,000, capped at the pool's maximum
  `resourceViewCount` if that is lower; the cap is recorded. F runs at every N up to its limit.
- **CPU publish**: publish all N textures once, timed in nanoseconds per texture, at the same N
  sweep; T includes the `gpuResourceID()` read and the 8-byte write, P the `setTextureView` call.

## Metrics

1. **Correctness gate.** Every output element equals the expected value computed on the CPU, at
   every N, pattern and access, for every arm; and a run with `MTL_DEBUG_LAYER=1` and
   `MTL_SHADER_VALIDATION=1` reports no error. An arm failing the gate is out.
2. **Documented-behaviour gate.** An arm whose correctness rests on behaviour Apple's Metal
   documentation does not state fails this gate. P fails it unless that documentation is found.
3. **GPU kernel tax.** Kernel time from precise-granularity timestamps written into a
   `MTL4::CounterHeap` around each dispatch. Reported as T over P and as T and P over F.
4. **CPU publish cost.** Nanoseconds per texture published, from a monotonic host clock.

## Statistics

- Release build. Validation off for timed runs, on for the correctness runs.
- Each repetition is one fresh process that runs every arm, alternating arm order between
  processes. 20 processes per configuration.
- In each process, per arm: 50 warm-up dispatches, then 200 measured dispatches; the process
  median is kept. The reported value is the median of process medians.
- Paired deltas between arms within a process; two-sided 95% percentile bootstrap confidence
  intervals with 10,000 resamples over processes.
- Recorded per run: device name and GPU family, macOS build, Xcode and `metal` versions,
  `metal-cpp` pin, power source, and the thermal state at start and end. A run that starts or ends
  above the nominal thermal state is discarded and rerun.

## Decision rule

- The ID table stays the lowering unless P passes the correctness gate at every configuration and
  T's tax over P is material in at least one configuration a renderer hits on hot paths: divergent
  or uniform `sample` or `read` at N of 1,024 or more.
- Material means the 95% interval excludes zero and the effect exceeds 5% of P's kernel time, the
  per-operation budget of ADR 0014; CPU publish cost is material on the same test.
- Because P fails the documented-behaviour gate, a material win does not adopt it by itself: the
  reference would name the undocumented dependency and the owner decides whether to accept that
  risk. Without a material win, a tie keeps the table.
- The result changes the API reference in the same change that records it: chapters 04 and 04
  part 2 drop their `pending spike 2` cells and move to `Accepted`, and 05 part 2 keeps `Proposed`
  only for spike 1.

## Procedure

- The harness is standalone C++ with metal-cpp and hand-written MSL, built by a script outside the
  xmake targets, so no production target changes. It lives under `Tools/Spikes/TextureIndexing/`
  on this branch only and is throwaway: production work follows on a clean branch.
- Raw timings, captures and logs are kept outside the published tree; this note keeps the
  environment, the summary tables and the decision.

## Results

Not yet measured.
