# Spike 2: how Metal 4 indexes textures

**Status**: Accepted (2026-10-12)

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

Measured on 2026-10-12 on an Apple M3 Max (Apple9) under macOS 27.0.1 (26A434), Xcode 26.6,
`metal` 32023.883 and the pinned `metal-cpp`, on AC power. All 120 timing processes started and
ended at the nominal thermal state; none was discarded. The harness is kept as measured under the
tag `spike-2-texture-indexing-2026-10-12`; raw timings, logs and a SHA-256 manifest are kept
outside the published tree.

**Gates.** All three arms and the sampler-table variant pass the correctness gate at every count,
pattern and access under API and shader validation, and the pool's IDs equalled `base + i` at
every count. A control run with the pool base shifted by one fails every element of arm P, while
shader validation reports nothing for the out-of-pool ID it reads. P fails the documented-behaviour
gate: besides the undocumented arithmetic, MSL forms its handle only by punning the integer
through a pointer, since `as_type` to a texture is rejected and `MTLResourceID` has no integer
constructor. The validation layer bounds an argument table at 128 textures, so F ran at N of 1, 16
and 128.

**GPU kernel time**, median of process medians in microseconds, and T over P with its 95%
interval. Material rows are marked.

| N | Pattern | Access | F | T | P | T over P |
|---|---|---|---|---|---|---|
| 1 | uniform | read | 190.9 | 198.8 | 192.3 | +3.3% [+3.3, +3.4] |
| 1 | uniform | sample | 193.6 | 205.5 | 191.0 | +7.6% [+7.5, +7.7], material |
| 1 | divergent | read | 191.4 | 198.9 | 192.6 | +3.3% [+3.2, +3.3] |
| 1 | divergent | sample | 193.8 | 205.6 | 191.0 | +7.7% [+7.6, +7.7], material |
| 16 | uniform | read | 190.6 | 198.8 | 190.7 | +3.4% [+3.3, +3.8] |
| 16 | uniform | sample | 193.7 | 205.4 | 190.9 | +7.6% [+7.5, +7.7], material |
| 16 | divergent | read | 278.3 | 275.4 | 275.3 | +0.0% [+0.0, +0.1] |
| 16 | divergent | sample | 274.7 | 276.0 | 278.1 | −0.7% [−0.8, −0.7] |
| 128 | uniform | read | 190.6 | 198.6 | 192.6 | +2.8% [+2.7, +3.6] |
| 128 | uniform | sample | 193.6 | 205.4 | 191.1 | +7.5% [+7.4, +7.6], material |
| 128 | divergent | read | 1,529 | 1,546 | 1,534 | +0.8% [+0.7, +0.9] |
| 128 | divergent | sample | 1,546 | 1,546 | 1,540 | +0.4% [+0.3, +0.4] |
| 1,024 | uniform | read |  | 198.8 | 192.9 | +3.1% [+2.8, +3.3] |
| 1,024 | uniform | sample |  | 205.8 | 192.0 | +7.2% [+6.4, +7.3], material |
| 1,024 | divergent | read |  | 1,759 | 1,759 | −0.0% [−0.0, +0.0] |
| 1,024 | divergent | sample |  | 1,755 | 1,756 | −0.0% [−0.0, −0.0] |
| 16,384 | uniform | read |  | 200.1 | 193.7 | +3.3% [+3.3, +3.4] |
| 16,384 | uniform | sample |  | 206.7 | 192.0 | +7.6% [+7.6, +7.7], material |
| 16,384 | divergent | read |  | 1,940 | 1,837 | +5.5% [+5.4, +5.8], material |
| 16,384 | divergent | sample |  | 1,990 | 1,865 | +6.6% [+6.6, +6.7], material |
| 100,000 | uniform | read |  | 201.5 | 194.7 | +3.5% [+3.4, +3.6] |
| 100,000 | uniform | sample |  | 207.9 | 193.1 | +7.7% [+7.6, +7.8], material |
| 100,000 | divergent | read |  | 3,911 | 3,835 | +2.3% [+2.0, +2.4] |
| 100,000 | divergent | sample |  | 4,047 | 3,951 | +1.9% [+1.9, +2.1] |

P runs at the speed of fixed slots, and T's tax is present at N of 1, so it is the dependent
8-byte handle load in front of each fetch rather than a cache effect. Fetching the sampler from a
table instead of a `constexpr` sampler costs under 0.4%. Of the 288,000 timed dispatches, 0.40%
fell below 0.8 times their process median and 0.75% above 1.2 times; medians absorb them.

**CPU publish cost**, nanoseconds per texture, median of process medians:

| N | F | T | P | T over P |
|---|---|---|---|---|
| 1 | 1145.5 | 604.0 | 542.0 | +15% [−38, +77] |
| 16 | 99.0 | 52.1 | 67.7 | −42% [−67, +29] |
| 128 | 32.6 | 12.7 | 38.9 | −66% [−78, −53] |
| 1,024 | 36.8 | 11.3 | 30.3 | −65% [−70, −60] |
| 16,384 | 77.8 | 25.3 | 62.3 | −57% [−71, −37] |
| 100,000 | 113.9 | 33.1 | 81.0 | −60% [−61, −54] |

At N of 1 and 16 a publish is too short to time reliably and the intervals include zero; at 128
and above the table is materially cheaper. F binds at most 128 slots, so its larger counts time
the first 128.

**Decision.** Under the rule above P wins materially in five hot configurations, which hands the
choice to the owner because P rests on undocumented behaviour. The owner keeps the ID table. The
table is documented, its CPU publish is materially cheaper, and the measured tax comes from a
kernel that does nothing but fetch; the composed frame of ADR 0014 judges whether it matters
against the 1% GPU budget, and a miss attributable to it reopens this decision. Running both
mechanisms behind a runtime switch is excluded, because it would add the hidden pipeline variants
ADR 0014 budgets at zero.
