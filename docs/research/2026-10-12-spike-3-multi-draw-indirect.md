# Spike 3: how Metal 4 runs multi-draw indirect with a GPU count

**Status**: Accepted (2026-10-13)

Spike 3 of [ADR 0014](../decisions/0014-overhead-measurement.md) decides the Metal 4 lowering of
[`CommandList::drawIndirect`](../spec/08-gpu-driven.md#commandlistdrawindirect) and
[`CommandList::drawIndexedIndirect`](../spec/08-gpu-driven.md#commandlistdrawindexedindirect)
with a non-null count, and with it `Limits::maxDrawIndirectCount`
([limits](../spec/15-formats-and-limits-part2.md#limits)). This note is the spike's
pre-registration: the workloads, metrics, statistics and decision rule below are fixed before any
measurement runs, and changing them invalidates the round. The results section is filled in when
the spike reports; nothing above it changes afterwards.

## Question

Metal 4 has no multi-draw with a GPU-written count. Does a backend kernel that encodes an indirect
command buffer (ICB) beat a `maxCount` loop of plain indirect draws whose surplus records are
emptied, on GPU time, CPU encode time and memory?

## Candidates

Every arm executes `min(count, maxCount)` draws of the caller's records, as chapter 08 defines.

| Arm | GPU work | CPU work per call |
|---|---|---|
| E, ICB encoding | a kernel, one thread per record, encodes records below `min(count, maxCount)` into an ICB and writes its execution range; a barrier; `executeCommandsInBuffer` with that GPU range | one dispatch, one barrier, one execute |
| L, loop | a kernel copies the records to native argument records in scratch memory, zeroing the instance count of records at or past `count`; a barrier; `maxCount` indirect draws | one dispatch, one barrier, `maxCount` draws |
| N, floor | native records already in scratch memory, and the CPU issues exactly `count` indirect draws | `count` draws |

N is not an available lowering, since the CPU does not know a GPU-written count; it is the floor
both candidates are scored against.

**`drawId` lowering.** Both kernels rewrite each record anyway, so both write its `drawId` into the
native base instance, and the shader reads it from `[[base_instance]]`. That is sound in RojoRHI
because `firstInstance` is invisible to shaders and pipelines have no vertex buffers, and it keeps
`rojoRHI::instanceIndex`, which subtracts `[[base_instance]]`, zero-based. It replaces chapter
05's per-draw binding of the record address for these two commands if it passes the gates. Mesh
draws have no base instance: E binds the record's address per command with `set_object_buffer`,
and L binds it with one CPU call per draw.

## Workloads

- **Draws**: each draw is one point primitive written to pixel `drawId` of a 256×256 `R32Uint`
  target, with value `drawId + 1`. Record `i` carries `drawId = (7919 i + 13) mod maxCount`, a
  permutation, and a non-zero `firstInstance` that must have no effect.
- **Variants**: non-indexed (`DrawIndirectArgs`) and indexed (`DrawIndexedIndirectArgs`, a
  one-index buffer).
- **`maxCount` sweep**: 1, 16, 256, 4,096 and 65,536.
- **Fill**: `count` is 0, 25% and 100% of `maxCount`, rounded down. The correctness pass also runs
  a count of twice `maxCount` and a null count.
- **Pipeline flag**: E needs pipelines created with `supportIndirectCommandBuffers`. With zero
  hidden variants allowed, every graphics pipeline would carry it, so every arm uses a pipeline with
  the flag, and one extra arm, N0, repeats N with the flag off.
- **Mesh gate**: E and L each run the non-indexed workload as mesh draws at every `maxCount`,
  checked for correctness only.

## Metrics

1. **Correctness gate.** The target equals the CPU-computed image, pixel for pixel, for every arm,
   variant, `maxCount` and fill, and a run with `MTL_DEBUG_LAYER=1` and `MTL_SHADER_VALIDATION=1`
   reports no error. An arm failing the gate is out; the largest `maxCount` an arm passes at is
   recorded.
2. **Native-call budget.** ADR 0014 allows at most the native minimum plus one native call per
   RojoRHI call on hot paths. The harness counts the native encoder calls each arm makes per call,
   exactly.
3. **GPU time.** Per frame, `GPUEndTime - GPUStartTime` from the commit feedback of a command
   buffer that holds only one arm's work for one call.
4. **CPU encode time.** Host clock around recording one call's commands, excluding commit and
   wait.
5. **Memory.** `allocatedSize()` of the ICB for E, of the scratch records for L.
6. **Flag cost.** N0 against N, on GPU time and CPU encode time.

## Statistics

- Release build. Validation off for timed runs, on for the correctness runs.
- Each repetition is one fresh process for one `maxCount` that runs every variant, fill and arm,
  alternating arm order between processes. 20 processes per `maxCount`.
- Per process, per arm and configuration: 50 warm-up frames, then 200 measured frames, each one
  commit waited to completion; the process median is kept. The reported value is the median of
  process medians.
- Paired deltas between arms within a process; two-sided 95% percentile bootstrap confidence
  intervals with 10,000 resamples over processes.
- Recorded per run: device and GPU family, macOS build, Xcode and `metal` versions, `metal-cpp`
  pin, power source, and the thermal state at start and end. A run that starts or ends above the
  nominal thermal state is discarded and rerun.

## Decision rule

- An arm that fails the correctness gate, or the native-call budget at a `maxCount` above 1, is
  out for that `maxCount`. If both candidates are out, the reference keeps the entry `costly` and
  names the failure.
- Between passing candidates, the one with lower GPU time wins where the difference is material:
  the 95% interval excludes zero and the effect exceeds 5% of the faster arm. CPU encode time
  decides on the same test where GPU time does not. Without a material difference, L wins as the
  simpler path.
- If N0 is materially faster than N, the ICB flag is a cost every pipeline pays, and the reference
  names it on E's mapping.
- `maxDrawIndirectCount` becomes the largest swept `maxCount` at which the chosen path passes the
  correctness gate, and at least 65,535 if that is 65,536.
- The `drawId` lowering through `[[base_instance]]` is adopted for the two commands if it passes
  the correctness gate in every case; the mesh gate decides whether `drawMeshTasksIndirect` keeps
  per-draw binding.
- The result changes the API reference in the same change that records it: chapter 08 and 15 part
  2 drop their `spike 3` notes and move to `Accepted`, and chapter 05 part 2 takes the `drawId`
  mapping while it still waits on spike 1.

## Procedure

- The harness is standalone C++ with metal-cpp and hand-written MSL, built by a script outside the
  xmake targets, so no production target changes. It lives under `Tools/Spikes/MultiDrawIndirect/`
  on this branch only and is throwaway: production work follows on a clean branch.
- Raw timings and logs are kept outside the published tree; this note keeps the environment, the
  summary tables and the decision.
- Metal 4 GPU capture refuses the compute encoder that writes an ICB, as the consumer's own ICB
  spike found; that limits debugging of E, not this measurement.

## Results

Measured on 2026-10-12 and 13 on an Apple M3 Max (Apple9) under macOS 27.0.1 (26A434), Xcode
26.6, `metal` 32023.883 and the pinned `metal-cpp`, on AC power. All 100 timing processes started
and ended at the nominal thermal state; none was discarded, and every timed frame still produced
the expected image. The harness is kept as measured under the tag
`spike-3-multi-draw-indirect-2026-10-12`; raw timings, logs and a SHA-256 manifest are kept
outside the published tree.

**Correctness gate.** Every arm produces the expected image, pixel for pixel, in all 250 cases:
both variants and the mesh gate, every `maxCount`, all five fills, under API validation. The run
with shader validation added reports no error, but it silently drops commands that the kernel
wrote into the indirect command buffer: mesh commands at every `maxCount`, and non-indexed and
indexed commands once the buffer holds 2,048 commands and runs with a GPU range. The kernel's
range readback is correct; the same commands draw when run with a CPU range; mesh commands encoded
on the CPU draw; neither an argument-encoder handle for the command buffer nor more object-buffer
slots changes the result. The gate's images are therefore judged under API validation and its
no-error condition under shader validation, as the owner ruled, and the drop is recorded as a
limit of the tool: conformance cannot check these images with shader validation on.

**`drawId` lowering.** Writing `drawId` into the native base instance passes in every case for
both kernels and the floor, with `firstInstance` non-zero and `instanceIndex` zero-based. Mesh
draws pass with the record's address bound per command by `set_object_buffer` in E and per draw
through the argument table in L.

**Native calls** per RojoRHI call: E makes 7 at every `maxCount` (open a compute encoder, set its
pipeline and table, dispatch, close it, barrier, execute); L makes `maxCount` plus 6; N makes
`count`.

**GPU time**, non-indexed, median of process medians in microseconds per command buffer, which
includes clearing the 256×256 target (3.8 µs with no draws), and E − L with its 95% interval.
Material rows are marked. A quarter of `maxCount` 1 is a count of 0.

| `maxCount` | Fill | E | L | N | E − L |
|---|---|---|---|---|---|
| 1 | full | 49.4 | 32.5 | 13.5 | +16.7 [+16.3, +17.0], material |
| 1 | quarter | 40.1 | 32.6 | 3.8 | +7.3 [+7.0, +7.7], material |
| 1 | zero | 40.2 | 32.6 | 3.8 | +7.5 [+7.2, +8.0], material |
| 16 | full | 51.4 | 33.8 | 11.5 | +17.6 [+17.5, +17.8], material |
| 16 | quarter | 50.0 | 32.9 | 13.2 | +17.2 [+17.0, +17.3], material |
| 16 | zero | 40.7 | 32.4 | 3.8 | +8.4 [+8.3, +8.5], material |
| 256 | full | 77.5 | 56.4 | 22.7 | +21.1 [+18.7, +23.0], material |
| 256 | quarter | 56.0 | 46.0 | 13.9 | +10.1 [+9.0, +11.5], material |
| 256 | zero | 40.5 | 41.6 | 3.8 | −1.1 [−1.5, −0.8] |
| 4,096 | full | 385.2 | 362.8 | 107.5 | +23.1 [+20.9, +29.0], material |
| 4,096 | quarter | 140.4 | 308.2 | 47.4 | −164.6 [−169.5, −135.6], material |
| 4,096 | zero | 40.8 | 210.5 | 3.8 | −169.8 [−170.7, −152.6], material |
| 65,536 | full | 3,172 | 3,531 | 624.1 | −357.3 [−361.3, −353.1], material |
| 65,536 | quarter | 828.4 | 3,347 | 164.0 | −2,518 [−2,520, −2,514], material |
| 65,536 | zero | 43.0 | 1,995 | 3.8 | −1,953 [−1,958, −1,952], material |

Indexed draws follow the same pattern, a little slower: E and L tie at 4,096 full (+3.1
[−1.2, +12.9]), and at 65,536 full E takes 3,680 µs against L's 4,227 and N's 647. E pays a fixed
37 µs or so for its compute pass and barrier and then about 48 ns per drawn command, against
9.5 ns for a native indirect draw; L pays for every record whether it draws or not. Of the 480,000
timed frames, 3.6% fell below 0.8 times their process median and 6.8% above 1.2 times; medians
absorb them.

**CPU encode time**, non-indexed, microseconds:

| `maxCount` | Fill | E | L | N |
|---|---|---|---|---|
| 1 | full | 3.8 | 2.6 | 0.7 |
| 16 | full | 3.8 | 3.6 | 1.7 |
| 256 | full | 3.8 | 17.0 | 15.6 |
| 4,096 | full | 9.4 | 209.0 | 213.3 |
| 65,536 | full | 10.7 | 3,057 | 3,135 |
| 65,536 | quarter | 9.7 | 3,022 | 765.3 |
| 65,536 | zero | 3.6 | 3,050 | 0.0 |

E's encode is nearly constant; L costs what N costs at a full fill at every count. Indexed encode
is within 5% of these, and every E − L difference above `maxCount` 16 is material.

**Memory**, `allocatedSize()` at `maxCount` 65,536: E's indirect command buffer takes 44.1 MB
for non-indexed, 45.4 MB for indexed and 84.2 MB for mesh commands, about 670, 690 and 1,280
bytes per command; L's scratch records take 1.0, 1.3 and 0.8 MB. At 256 the buffers are 176 KiB,
176 KiB and 336 KiB.

**Flag cost.** N0 against N is never material: GPU time differs by −1.9% to +0.1% and CPU time by
−0.9% to +0.3%, so every graphics pipeline can carry `supportIndirectCommandBuffers`.

**Decision.** Above a `maxCount` of 1, L fails the native-call budget, because its calls grow with
`maxCount`. The owner reads the native minimum as a native program whose culling kernel writes
indirect commands itself, a barrier and an execute, so E's 7 calls exceed it by 4 as well. Both
candidates are therefore out above 1, and the rule keeps the entries `costly` and names the
failure; the reference maps them to E, whose excess is a constant compute pass where L's grows
with `maxCount` and costs up to 46 times E's GPU time. At a `maxCount` of 1 the budget does not
apply and L is materially faster on GPU and CPU, so a single record runs the copy kernel and one
plain indirect draw. `maxDrawIndirectCount` keeps its floor of 65,535, and Metal 4 reports 65,536.
The `drawId` lowering through `[[base_instance]]` is adopted for the non-indexed and indexed
commands; mesh commands keep a per-draw record binding.

The lowering removes one native case: a lone record with a null count was a plain indirect draw,
but a native indirect draw passes the caller's `firstInstance` as the base instance, so the record
must be rewritten first. The owner accepts that cost now. Replacing the record's `firstInstance`,
which shaders never see, with `drawId` in the native base-instance position would restore the
native case and drop L's copy, but it changes the record layout on every backend and is left as a
separate design question. Direct draws pass a base instance of 0, so `drawId()` returns 0 for
them.
