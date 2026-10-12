# Spike 3: how Metal 4 runs multi-draw indirect with a GPU count

**Status**: Proposed (until spike 3 of ADR 0014 reports)

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

Not yet measured.
