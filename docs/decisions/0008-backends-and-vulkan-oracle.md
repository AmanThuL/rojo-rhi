# ADR 0008: Metal 4 and D3D12 are the production backends; Vulkan is an oracle

**Status**: Accepted (2026-10-12)

**Supersedes**: [ADR 0002](0002-metal4-first.md), [ADR 0004](0004-second-backend-target.md)

## Context

ADR 0002 made Metal 4 the first backend and ADR 0004 named D3D12 a design target that exerts
pressure on the interface without being built. The redesign tightens both. The research found
D3D12 to be the binding constraint on the shader-facing model: HLSL and DXIL have no pointers,
Slang emits invalid HLSL for pointer types without an error, render-target and depth textures need
layout transitions for compression, and depth-stencil state lives in the pipeline state object
([platform state](../research/2026-10-11-rhi-redesign/05-d3d12-vulkan-state.md)). Metal 4 and 2026
Vulkan are both close to the model the interface adopts, so Vulkan adds design evidence at the
points where Metal 4 and D3D12 do not differ enough
([Metal 4 and Slang state](../research/2026-10-11-rhi-redesign/06-metal4-slang-state.md)).

Two needs follow. The interface must be tested without a GPU, so contract, valid-usage and counter
tests run on hosted CI rather than only on the two machines that have hardware. And Vulkan must
stay a mapping each entry is checked against, without the permanent implementation, conformance and
hardware-validation obligations a production Vulkan backend would add. ADR 0004 recorded that
Linux support is not a goal; that has not changed.

## Decision

**Production backends.** The production backend set is Metal 4, implemented through metal-cpp,
and D3D12, planned.

- Metal 4 is the current backend. There is no Metal 3 fallback and no translation layer, and a
  device below the hardware floor fails creation with an unsupported-device error. The floor itself
  is owned by [ADR 0010](0010-hardware-floor.md).
- D3D12 stays planned and absent: this repository holds no D3D12 code. Its implementation follows
  the Null and Metal 4 backends passing the v2 suite, and starts only when a Windows build, run and
  GPU-validation environment exists. It is accepted when it passes the conformance suite
  unchanged ([ADR 0009](0009-conformance-and-valid-usage.md)).
- The frames-in-flight count ADR 0002 fixed at 3 becomes a configured count of two or three, set on
  the device; resource lifetime and recycling rules are stated against that count.

**One backend per build.** Exactly one backend is compiled, chosen by an xmake option: Metal 4 by
default on macOS, D3D12 by default on Windows once it exists, and Null on any platform. There are
no virtual interfaces. Each backend's sources define the public member functions directly, so with
link-time optimization a call can inline into the consumer. The mechanism is owned by
[ADR 0012](0012-language-and-dispatch.md); the single-backend-per-build rule is the amendment to
the old single-backend rule that the Null backend required.

**Null is a test backend.** The Null backend implements the whole interface without a GPU. It
records commands, runs the backend-neutral validation tracker and maintains the counters, so every
contract, valid-usage and counter test runs on hosted CI. It is never a shipping backend and it
renders nothing.

**Vulkan is a required oracle with no code.** No Vulkan backend exists, none is scheduled, and no
CI job runs Vulkan. Vulkan remains a required design check in three ways:

- Every entry of the API reference documents its Vulkan mapping and a cost class. The classes are
  exactly `native`, `cheap`, `costly` and `impossible`, recorded for Metal 4, D3D12 and Vulkan.
  `native` is a direct equivalent, `cheap` a small bounded extra, `costly` extra work or an extra
  dispatch, and `impossible` an operation a backend cannot express. The reference owns the precise
  definitions.
- A design that maps badly onto any of the three is rejected, or changed until it does not. This
  strengthens ADR 0004, which weighed Vulkan only as evidence and did not claim that decisions were
  weighed against their D3D12 mapping. They now are, and the interface stays shaped by the common
  core, not by Vulkan.
- The shader build compiles Slang to SPIR-V in CI, so the oracle's shader side is checked
  mechanically rather than on paper. Compilation is a check, not execution: nothing runs the
  SPIR-V.

**Retained from ADR 0002 and ADR 0004:** metal-cpp as the Metal 4 binding with no Objective-C++
glue layer; no Linux support; and the rule that no document, release text or interface comment
describes RojoRHI as having D3D12 or Vulkan support before it is real.

## Consequences

Interface decisions are now weighed against two production backends, one planned, and one
oracle, so they are tested against more contrast than the Metal 4 origin gave them. The cost is
that Metal-first shortcuts are rejected, such as making depth-stencil and blend dynamic because
Metal 4 could do so cheaply while D3D12 would need hidden pipeline variants.

Hosted CI covers the interface contract through Null, but only compile behaviour for
Vulkan and nothing for D3D12 until its backend exists. A Vulkan mapping that compiles but
misbehaves at run time is not caught; the oracle is a design discipline, not a conformance
result. Maintaining the Null backend means every interface change lands in a further place, which
is the price of GPU-free tests.

The configured frames-in-flight count replaces a backend constant, so callers see one lifetime
model across backends. Until a D3D12 backend has been built, the reference's D3D12 columns are
design claims and are reviewed as such; the founding rule stands that D3D12 stays a planned
backend in every document and is never described as present.
