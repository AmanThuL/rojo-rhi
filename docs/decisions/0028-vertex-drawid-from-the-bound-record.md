# ADR 0028: On Metal the vertex stage reads drawId from the bound record, and a lone record draws natively

**Status**: Accepted (2026-11-04)

**Supersedes**: the Metal 4 cell of the draw-record row of [ADR
0018](0018-root-srv-and-reserved-bindings.md)'s reserved bindings, "`buffer(3)`, mesh and task
stages only; the vertex stage reads `[[base_instance]]`", and the decision recorded with [spike
3](../research/2026-10-12-spike-3-multi-draw-indirect.md) that adopts the `drawId` lowering through
`[[base_instance]]` for the non-indexed and indexed commands. The rest of ADR 0018 stands.

## Context

Spike 3 moved a vertex shader's `drawId` on Metal 4 into the draw's base instance. The native base
instance of an indirect draw is the record's `firstInstance`, so every indirect draw, a lone record
with a null count included, ran a backend kernel on a compute encoder ahead of the render encoder
to rewrite its record. Spike 3 named the cost and left the alternative open as a separate design
question.

Its consumer answered it. Luminex draws its scene and shadow passes with one
`drawIndexedIndirect` of `maxCount` 1 and a null count per run, with a root upload between runs,
so each draw cost a one-thread dispatch on that encoder. Measured on an Apple M4 Pro under macOS
27.0.1, AC power, Release, unscored (the evidence is kept outside this repository):

- **The pass timings were inflated.** The pass's begin timestamp was written on the kernels'
  encoder, whose opening barrier waits for nothing, so it started alongside the earlier passes and
  dated the pass from there. Against the frozen v1 App, which drew each record natively, `gpuSumMs`
  rose 20.6% on Sponza, 12.1% on Light Lab and 49.6% on Visibility Lab (twelve AB/BA pairs per
  scene), while the scene pass with direct draws rose only about 15% on Sponza.
- **The rest is real.** A prototype that drew a lone null-count record natively, with no kernel,
  rendered bit-identical images and brought the indirect pass times to the direct ones (Sponza
  scene 0.807 to 0.632 ms against 0.631 direct; Light Lab shadow 0.094 to 0.034 against 0.034).
  In the prototype the frame scope's GPU time fell 4 to 10 µs on Sponza, Light Lab and batched
  Visibility Lab, and 0.54 ms (12.7%) on Visibility Lab with 4,711 indirect draws, whose CPU encode
  time fell 0.48 ms (26%); four AB/BA pairs per configuration, every pair agreeing in sign.
- **The implementation confirms it.** With this decision implemented and no consumer source change,
  the same consumer's images stayed bit-identical, and Visibility Lab with indirect submission went
  from 3.000 to 2.515 ms of frame GPU time and from 1.782 to 1.289 ms of CPU encode, against the
  unchanged v2 build: four AB/BA pairs of 32 warm-up and 128 measured frames per configuration, the
  same machine, OS and power, unscored; the other scenes moved by 0.03 ms or less.

A GPU count still needs a kernel: Metal has no multi-draw with a GPU count, and a command of an
indirect command buffer cannot itself be an indirect draw. Only the lone null-count record can be
drawn natively.

## Decision

- On Metal the vertex stage reads `drawId` from offset 0 of the record bound at `buffer(3)`, as the
  task and mesh stages already do. `rojoRHI::drawId(VertexIds)` keeps its signature;
  `rojoRHI::instanceIndex` still subtracts the base instance, so the base instance a draw passes is
  invisible to shaders.
- A `drawIndirect` or `drawIndexedIndirect` of `maxCount` 1 with a null count binds its record at
  `buffer(3)` of the list's argument table, when the table holds another, and is one native
  indirect draw from record offset 4 on the render encoder, opened if needed: no kernel, no compute
  encoder and no split. Its base instance is the record's `firstInstance`.
- A count, or a `maxCount` above 1, keeps the kernel. The writer kernel of `maxCount` 1 writes base
  instance 0 and the draw binds the caller's record; the commands the encode kernel writes into the
  indirect command buffer inherit no buffer and bind the root recorded with the call, the view and
  sampler tables and their own record at `buffer(0)` to `buffer(3)` on the vertex and fragment
  stages, as mesh commands already do.
- A direct draw binds the device's zero record at `buffer(3)`, which every list's table holds when
  the list is acquired, so a direct draw costs one compare when no indirect draw of the recording
  bound another record.
- A render pass's begin timestamp is written on its first render encoder. The kernels of counted
  draws ahead of it are outside the pass's timing, since their encoder may overlap earlier work.
- `ROJORHI_SHADER_ABI_VERSION` becomes 3: the binding a built-in reads changes, and a library
  compiled against 2 would read `drawId` from the base instance, which is now the record's
  `firstInstance` or 0. D3D12 and Vulkan keep their lowerings; their libraries change only in the
  stamp.

## Consequences

- A lone indirect draw costs what the native call costs, plus one argument-table write when its
  record differs from the bound one. The record layout of chapter 08 does not change.
- A vertex shader's `drawId` on Metal is one load through `buffer(3)` instead of a system value, the
  same cost the task and mesh stages and the Vulkan lowering already pay.
- Each command of a draw or indexed indirect command buffer binds eight buffers, so the buffer's
  memory per command grows, and so does the kernel's work per record. The `maxCount` above 1 path
  stays `costly`.
- A pass with counted draws no longer reports their kernels in its time.
- Every consumer shader library is rebuilt for the new stamp; one stamped 2 is refused with
  `ShaderAbiMismatch`.
