# ADR 0005: Conformance checkpoint A freezes eight semantic areas behind one test tag

**Status**: Accepted (2026-09-19)

**Restates**: Luminex ADR 0009, "Portability checkpoint A freezes eight semantic areas behind one
test tag"

## Context

A second backend needs an acceptance gate a reviewer or a CI job can run as one command, not a
prose promise that source can drift away from. Conformance-style cases for the interface's
execution substrate already exist from shipping it, so the freeze does not need new tests: it needs
to name the cases that already prove each area and make exactly that set runnable, unchanged,
against a future backend.

The checkpoint was frozen while the component still lived inside its consumer, so its case
inventory spans both. This ADR owns the RojoRHI half.

## Decision

Checkpoint A covers eight semantic areas: upload and layout, resource views, sRGB, reversed-Z,
storage hazards, load/store behaviour, indirect arguments and frame-slot retirement. Each area's
strongest existing Catch2 case carries the tag `[checkpoint-a]` alongside its own tags, and the
frozen set is exactly that tag as one filter. It runs with `MTL_DEBUG_LAYER=1` against a real
device, with the test binary's build directory as the working directory, because the backend
resolves shader paths relative to the current directory.

RojoRHI owns these cases, frozen by case name:

| Area | Case |
|---|---|
| Upload and layout | "a copy writes buffer bytes into a texture sub-rectangle" |
| Upload and layout | "a BC1 block decodes to its endpoint colour when sampled" |
| Upload and layout | "a cubemap samples the face its direction points at" |
| Resource views | "a storage texture view addresses a single mip level" |
| Resource views | "a copy captures one array layer and mip of a cubemap" |
| sRGB | "an sRGB texture is linearised by the sampler, a linear one is not" |
| Reversed-Z | "a Greater depth test keeps the nearer fragment in reversed-Z" |
| Storage hazards | "a storage texture written by one dispatch is read by the next" |
| Storage hazards | "a storage texture written by a dispatch is sampled by a later draw" |
| Storage hazards | "a copy captures an intermediate mip level of a GPU-written chain" |
| Load/store behaviour | "a depth-only pass stores depth a later pass can sample" |
| Indirect arguments | "an indirect dispatch reads its threadgroup counts from a buffer" |
| Indirect arguments | "an indirect draw reads its vertex range from a buffer" |
| Indirect arguments | "an indirect indexed draw reads its index range and base vertex from a buffer" |
| Frame-slot retirement | "uniform ring survives twelve frames overlapping in flight" |
| Frame-slot retirement | "the frame-data arena survives twelve frames overlapping in flight" |

The frame-data arena case is the one row not in the source ADR's frozen table: it was tagged
`[checkpoint-a]` additively after that freeze, which the source ADR permits, and it asserts against
`metal4::frameDataCounters`, a diagnostic counter scoped to the Metal 4 backend. The row is kept,
because the area it proves is frozen either way, but the case must be given a backend-neutral
counter before a second backend is judged against it; as written, it cannot run unchanged on a
backend that has no such counter.

Every case above runs against a real GPU device. The consumer keeps the rest of the frozen
inventory: the transient-resource cases, in its two transient-resource test files, which prove
transient replacement, heap-generation retirement and the graph-contract rejection of a loaded
transient attachment. Those belong to the layer that owns transients, not to RojoRHI, but they gate
a future backend on the same terms as the cases above: the acceptance gate is the whole frozen
inventory, both halves, and a backend that passes only this repository's half has not passed
checkpoint A. Until the planned test split moves the cases above into this repository, they still
live in the consumer's test tree; the split changes their location and nothing else, and the two
halves' filters together equal the frozen inventory by case name.

Adding `[checkpoint-a]` to a case not listed above needs no ADR: it adds evidence for an area
already frozen and changes nothing this table asserts. Removing a listed case's tag, or retagging
it to a different case even one judged to prove its area better, requires a superseding ADR,
because either edits the table, and the table is not correctable once this ADR is accepted except
by supersession. A future backend is not accepted until it passes every case above unchanged — no
edited assertion, no relaxed tolerance — because relaxing a frozen case to fit a backend defeats
the reason it was frozen.

**Known caveat: barrier sufficiency, not necessity.** The storage-hazard cases verify correct
results with the declared barrier present; they do not prove the barrier is necessary. Removing the
barrier recording from the compute and copy hazard paths left the compute-hazard cases passing
anyway, because this hardware happened to order the accesses without it, while render-target-to-
sampled cases failed under the same mutation. The checkpoint therefore freezes sufficiency and
cannot, without a fuzzing harness or a backend that reorders more aggressively than Metal 4, prove
necessity. A backend more tolerant of missing barriers could pass while still shipping a real
hazard; that risk is accepted rather than solved here.

## Consequences

A future backend's acceptance gate is concrete: build against the same public headers, wire up the
backend, run the tag filter from its build directory, and every case must pass with its assertions
unchanged. Reviewers and CI check the freeze mechanically instead of re-deriving which tests matter.
The eight areas gate a backend's execution substrate and do not claim to cover everything RojoRHI
exposes. A green run is evidence of correct results under the barriers as declared, not proof that
a new backend's synchronization is complete; that gap stays documented for whoever plans a second
backend's validation, to weigh alongside the platform validation layer's own results.
