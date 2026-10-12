# ADR 0007: The interface is address-first with thin handles

**Status**: Accepted (2026-10-12)

**Supersedes**: [ADR 0003](0003-thin-rhi.md), [ADR 0006](0006-execution-model.md)

## Context

ADR 0006 adopted an address-first path for per-frame data only and kept everything else
object-shaped: argument-table binding slots, per-resource barriers valid only between passes and a
compute or copy pass for every group of dispatches. It rested on a pre-registered experiment in the
consumer, Luminex ADR 0010, "The RHI adopts an address-first per-frame data path; the object model
stays", which compared the object-shaped interface with a whole-API address-first prototype.

Three things have changed since. First, the second production backend is D3D12, planned, with Vulkan
as an oracle (see [ADR 0008](0008-backends-and-vulkan-oracle.md)), so the interface is judged
against two explicit APIs besides Metal 4, and the research shows all three converging on one
shader-facing model: Metal 4 and 2026 Vulkan (`VK_EXT_descriptor_heap`, push data, unified image
layouts) are both close to the "No Graphics API" design, and D3D12 Shader Model 6.6 bindless reaches
it through heap indices and a root constant buffer view
([surface and mapping](../research/2026-10-11-rhi-redesign/01-nographicsapi.md),
[platform state](../research/2026-10-11-rhi-redesign/05-d3d12-vulkan-state.md),
[Metal 4 and Slang](../research/2026-10-11-rhi-redesign/06-metal4-slang-state.md)). Second, the
experiment's losses can be attributed: its largest CPU win came from defects in the incumbent, and
three of its regressions were properties of the prototype rather than of address-first design.
Static roots were re-pushed every frame, stage-only barriers could not name a resource, and an
address lookup capped the allocator at 64 blocks. Third, the engines and standalone RHIs surveyed
show where a thin layer pays and where it hides cost
([standalone RHIs](../research/2026-10-11-rhi-redesign/02-standalone-rhis.md),
[engine RHIs](../research/2026-10-11-rhi-redesign/03-engine-rhis.md)).

## Decision

RojoRHI v2 is an address-first hybrid: the shader-facing model follows "No Graphics API", and the
CPU-facing model keeps thin, generation-checked handles only where Metal 4 or D3D12 force them. It
is a clean break, built beside the frozen v1 interface.

**No binding slots.** The v1 slot model (`bindBuffer`, `bindTexture`, `bindSampler` and the storage
variants) is removed. A shader reaches all data through three paths: one root block, addresses
inside it, and heap indices inside it. There are no descriptor sets, no pipeline layouts and no
per-pipeline binding declarations; every backend uses one fixed global layout. The shader ABI
itself is owned by [ADR 0011](0011-shader-abi.md).

**Root by address.** Every draw and dispatch reads one flat root block bound by address.
`setRoot(Addr)` binds an address the caller already has, so a static root is built once and never
re-uploaded; this, not a fused call, is what answers the prototype's per-frame re-push of static
data. `pushRoot(value)` keeps the single-call path for per-draw data: one allocation, one copy and
one bind. `uploadFrameData` returns an `Addr` and is a separate public call from `setRoot`, so a
caller may upload and bind apart. The rest of ADR 0006's per-frame data rule carries forward: one
suballocation from the open frame's arena and one copy per upload, an address valid only within the
frame that produced it, arena growth owned by the backend, and failure to grow fatal.

**`Addr`.** An 8-byte value naming buffer memory. It supports offset arithmetic, so a consumer may
suballocate inside one large buffer. Shared structs carry `Addr`, never a raw 64-bit pointer,
because the D3D12 form is a raw-buffer heap index and byte offset rather than a virtual address.
Only the shader ABI module dereferences it.

**Handles.** `Device`, `Queue`, `CommandList` and `Swapchain` are trivially copyable value wrappers
around one backend pointer with non-virtual member functions. Resources (`Buffer`, `Texture`,
`Pipeline`, `ShaderLibrary`, `Heap`, `Fence`, `Upscaler`) are 64-bit identifiers: a 32-bit index, a
24-bit generation and an 8-bit type tag, so a stale identifier asserts in non-Release (Debug)
builds. A sampler has no handle; it is a `SamplerIndex` into the sampler heap. `Device::destroy` is
deferred until the last frame that could have used the object retires. Copies take buffer handles
and offsets rather than addresses, because recovering a native buffer from an address needs a
reverse lookup, the structure that capped the prototype.

**Hybrid heap indices.** `ViewIndex` and `SamplerIndex` are 32-bit indices into two global heaps.
The default is RHI-assigned: `createView` and `createSampler` allocate a slot, and release goes
through the deferred destruction path, so a slot is never reused while unretired work may read it.
Callers needing layout control reserve caller-owned contiguous ranges and write them explicitly. A
bindless shader's reads are invisible to the CPU, so replacing a live slot is a race the tracker
cannot see; the reference gives the safe recipe of writing a fresh slot, publishing it, then
releasing the old one. The hybrid costs nothing when ranges are unused.

**Small pipelines.** With no vertex-input state and no layouts, a pipeline holds only state at least
one backend bakes at creation: shaders, primitive class, raster state, depth-stencil state, colour
targets, formats and sample count. Vertices are always pulled by the shader. Depth-stencil and blend
stay in the pipeline because D3D12 bakes them there, and making them dynamic would mean hidden
pipeline variants. Viewport, scissor, stencil reference, blend constants, exact topology and depth
bias values are dynamic. There is no run-time specialization; permutations compile offline.
Shaders are precompiled libraries and RojoRHI never compiles a shader at run time. A pipeline cache
blob and creation counters exist from the first version to control stutter.

**Lazy encoders.** Only render passes have scope. Compute and copy commands are valid anywhere
outside a render pass; there is no compute or copy pass. A backend whose API has encoders opens one
lazily on the first compute or copy command and closes it at the next render pass or at the end of
the list. A render pass performs no layout transitions, and barriers inside one are invalid.

**Batched barriers.** Barriers are explicit and batched into one call taking global, buffer and
texture barriers. Global barriers order memory by stage and access with no resource; buffer
barriers name a range; texture barriers carry old and new layouts, because D3D12 render-target and
depth compression apply only in their own layouts. The batch collapses to one stage barrier on
Metal 4, so naming resources costs nothing there, which answers the prototype's barrier regression.
Nothing is tracked in release builds.

**Validation tiers.** Release builds check nothing. Debug builds check arguments, handle
generations and scope rules. A third tier exists only in non-Release builds and is selected at run
time through `DeviceDesc::validation`: a backend-neutral tracker that checks every barrier, every
observable access, heap-slot typing, cross-queue handoff and lifetime, and reports a missing
barrier identically on every backend. The observable accesses are attachments, copy and clear
endpoints, index and indirect reads, upscaler and present images, and shader accesses declared
with `CommandList::declareAccess`; undeclared bindless accesses through heap indices or `Addr` are
untracked. For the accesses it observes, the tracker checks necessity as well as sufficiency (see
[ADR 0009](0009-conformance-and-valid-usage.md)).

**Retained from ADR 0003 and ADR 0006:**

- Creation returns `Result<T>` with a stable error category and a message; misuse of the interface
  is a fatal `ROJORHI_ASSERT`, not an error code.
- Every GPU object is created with a label, defaulted with the `rojorhi.` prefix when none is given.
- Diagnostics leave through one message callback taking a severity and a message, defaulting to
  stderr; RojoRHI takes no logging dependency.
- RojoRHI has no shader-toolchain duty at run time: it loads precompiled libraries and reports a
  load failure.
- Public headers name only the standard library and RojoRHI's own types: no backend, windowing,
  ImGui, consumer or third-party type. The optional ImGui adapter stays a separate target.
- The interface grows by demonstrated need. Ray tracing is reserved in the reference, as an
  address-based acceleration-structure model and heap-index space, but 0.1.0 contains no such
  interface.

**Changed from ADR 0003 and ADR 0006:** binding is no longer argument-table shaped; the backend is
no longer chosen behind a factory but compiled in, which ADR 0008 owns; the object model no longer
stays; allocation, copy and bind are no longer inseparable, because static roots must be built once
and bound many times; and the rule against a parallel interface lapses for the migration, since the
v1 interface stays frozen beside v2 until the consumer has ported and it is then deleted. Threading,
language and dispatch, and versioning are decided in [ADR 0013](0013-threading.md),
[ADR 0012](0012-language-and-dispatch.md) and [ADR 0015](0015-versioning.md).

## Consequences

The shader-facing model matches the three APIs closely, so most operations map as native or cheap
and the reference records the exceptions with a cost class. The exceptions are named here so they
are not rediscovered. Dereferencing `Addr` on D3D12 costs one descriptor fetch per base, so whether
it is recommended on hot paths waits on a pre-registered spike
([ADR 0014](0014-overhead-measurement.md)); until it reports, the reference recommends root fields
and heap indices for hot per-draw data. Metal 4 textures cost one extra 8-byte load through a
hidden ID table, and a counted multi-draw indirect costs an extra dispatch on Metal 4; spikes 2
and 3 of [ADR 0014](0014-overhead-measurement.md) measured both and kept them
([spike 2](../research/2026-10-12-spike-2-texture-indexing.md),
[spike 3](../research/2026-10-12-spike-3-multi-draw-indirect.md)).

Callers keep two rules the type system cannot enforce. An address is a number, so caching one past
its frame compiles and reads recycled memory; the rule is stated where the operation is declared.
Replacing a heap slot that unretired work may read is a race the tracker cannot observe. Each D3D12
buffer also costs descriptor slots, so the reference recommends few large buffers with
suballocation.

The interface needs more machinery than the prototype's slot model hid: a heap manager, a
backend-neutral validation tracker and a global shader ABI module. The tracker is the price of
leaving release builds untracked, and it is what lets a missing barrier on an observable access fail
on a GPU that happens to order the accesses anyway. Because v2 is a clean break, the consumer ports
in one step and v1 is deleted afterwards; until then both interfaces coexist and v1 stays frozen.
