# ADR 0004: D3D12 is the design target for a second backend

**Status**: Proposed

**Restates**: Luminex ADR 0007, "D3D12 is the intended second backend"

## Context

Metal 4 is the only implemented backend and Apple Silicon macOS the only development and validation
environment. A second explicit API is still needed as design pressure, so that the interface does
not quietly acquire Metal-only assumptions before anything else exercises it.

Linux support is not a goal. A production Vulkan backend would add permanent implementation,
conformance, debugging and hardware-validation obligations without serving a target platform. D3D12
is the native Windows API and a materially different translation target. Vulkan specifications and
extensions can supply design evidence without a shipping backend.

## Decision

Metal 4 remains the first, current and only backend. D3D12 is the intended second production
backend and is a design target, not a capability: no D3D12 code exists here, and its implementation
is scheduled only once a Windows build, run and GPU-validation environment exists. Vulkan and Linux
support are not planned.

Vulkan specifications, extensions, samples and disposable prototypes may challenge interface
decisions, but they create no production or CI commitment and must not make the public interface
Vulkan-shaped. (The source ADR says this much. It does not say that interface decisions are weighed
against their D3D12 mapping, so that stronger rule is not claimed here.)

Backend-neutral conformance tests keep growing on Metal first. A second backend is accepted only
when it passes the frozen conformance set unchanged.

## Consequences

The absence of a second backend does not block work on the Metal 4 one, and no document, release
text or interface comment may describe a second backend as something RojoRHI has. When D3D12 work
is scheduled it inherits a concrete acceptance gate rather than a negotiation. Vulkan stays
available as a third design oracle where Metal 4 and D3D12 do not expose enough contrast.
