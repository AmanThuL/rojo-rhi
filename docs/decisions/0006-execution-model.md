# ADR 0006: Per-frame data is address-first; the object model stays

**Status**: Accepted (2026-09-19)

**Restates**: Luminex ADR 0010, "The RHI adopts an address-first per-frame data path; the object
model stays"

## Context

A pre-registered experiment compared the object-shaped interface against an address-first prototype
distilled from the "no graphics API" model, on one frozen representative frame graph and frozen
stress workloads, judged against gates and thresholds fixed before any measurement. Its rules:
gates first; a difference counts as material only when it is large and repeatable beyond a
confidence bound; wholesale replacement requires broad wins with no material regression in any
other core dimension; ties retain the incumbent; the smallest change that captures a proven win is
preferred.

The measurement found one dominant, localized win. Per-frame data delivery carried an
allocation-and-copy cliff: over a thousand per-draw uniform buffer creations in a single run once
a fixed-capacity ring overflowed, against single digits for the prototype, and a large repeatable
reduction in CPU encoding time with a confidence interval excluding zero. The rest of the prototype
did not qualify. Its address-first shape needed more interface, not less — more concepts,
operations and caller-side arguments over the same workloads — because its allocator, ring, handle
and capability machinery outweighed the objects it removed. Its stage-pair barriers could not scope
to a resource, cutting barrier count on a composed graph while multiplying it on isolated hazard
cases. Its root-data path re-pushed static data every frame, doubling binding calls against an
incumbent that prebuilds it.

## Decision

The change is bounded to the contract area the win is localized in.

RojoRHI delivers per-frame data through an address-first path. A caller uploads a per-draw or
per-pass block and receives the GPU address the shader reads it through. The block is suballocated
from memory the open frame owns; there are no per-draw buffer objects and no per-draw descriptor
churn.

One caller operation performs one allocation, one copy and at most one backend address bind. The
allocation, the copy and the bind are not separable calls, so the path cannot degrade into the
prototype's two-call root push.

The returned address is a plain value carrying no handle and owning nothing, which is what lets one
block's address be written into another block uploaded later in the same frame. It is valid only
inside the frame that produced it: the memory belongs to a frame-in-flight slot the backend
recycles once the GPU retires that frame, so keeping the address across a frame boundary reads what
a later frame wrote there. Data that must outlive a frame is a buffer, bound as an object.

Running out of frame-owned memory is not a caller error and is not reported as one. The backend
grows the frame's arena; failing to grow it is fatal, because command recording has no partial-frame
failure a caller could recover through.

Static data stays reusable. Nothing in this path requires re-uploading a block that has not
changed.

Everything else is retained. The resource, pass, pipeline, residency and barrier models stay
object-shaped, the binding model stays argument-table slots rather than a bindless resource path,
and the barrier model is untouched by this decision. There is no parallel interface: the
address-first path is the only per-frame data path, and the prototype is not a supported API.

Conformance semantics are preserved: this contract area changes how per-frame data reaches a
shader, not what any frozen conformance case asserts.

The header-contract wording above — the address as a plain value owning nothing, its validity
ending with the frame that produced it, and the rule being stated where the operation is declared —
is taken from the interface as it stands after the migration, not from the source ADR, which
decided the reshape without fixing that wording.

## Consequences

Per-frame data delivery no longer allocates per draw, and its cost does not depend on a fixed ring
capacity a busy frame can overflow. The interface keeps the object model that measured better on
API surface and on isolated-case barrier counts, so the reshape buys the encoding win without
importing the regressions that blocked wholesale replacement.

Callers gain a lifetime rule they must respect and that the type system cannot enforce: an address
is just a number, so caching one past its frame compiles and reads recycled memory. The rule is
stated where the operation is declared. Arena growth is the backend's business, which means a
caller cannot bound its per-frame memory through the interface and an exhausted device aborts
rather than degrading.
