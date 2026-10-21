# ADR 0026: D3D12 is implemented before hardware and verified on WARP

**Status**: Accepted (2026-10-21)

**Supersedes**: the passages of [ADR 0008](0008-backends-and-vulkan-oracle.md) that kept D3D12
planned, absent and not yet the Windows default, quoted below; the rest of ADR 0008 stands.

## Context

ADR 0008 kept D3D12 planned in four passages, which this ADR supersedes:

1. In its production backend set, the words "and D3D12, planned" of:

   > The production backend set is Metal 4, implemented through metal-cpp, and D3D12, planned.

2. The clause:

   > D3D12 stays planned and absent: this repository holds no D3D12 code. Its implementation
   > follows the Null and Metal 4 backends passing the v2 suite, and starts only when a Windows
   > build, run and GPU-validation environment exists.

   The sentence after it, "It is accepted when it passes the conformance suite unchanged", stands.

3. In its build rule, the words "D3D12 by default on Windows once it exists".

4. In its Consequences:

   > the founding rule stands that D3D12 stays a planned backend in every document and is never
   > described as present.

ADR 0008's retained rule that "no document, release text or interface comment describes RojoRHI as
having D3D12 or Vulkan support before it is real" stands, and is consistent with this ADR: the
description below claims no support.

The project has no Windows machine with a GPU. Hosted `windows-latest` CI runners have WARP, the
software rasterizer, and run it with the Agility SDK's debug layer and GPU-based validation. The
floor probe (`Tools/D3D12Probe/`) created a device on the app-local WARP there and met every floor
item of [ADR 0010](0010-hardware-floor.md) it checks (feature level 12_2, Shader Model 6.6 heap
indexing with resource binding tier 3, `TypedUAVLoadAdditionalFormats`, enhanced barriers,
`DynamicDepthBiasSupported`), reporting shader model 6.9 and resource heap tier 2, with the debug
layer and GPU-based validation active
([run 39398881860](https://github.com/AmanThuL/rojo-rhi/actions/runs/39398881860); the results
table of [the D3D12 build page](../architecture/d3d12-build.md#the-floor-probe)).

The second passage's first condition held: the Null and Metal 4 backends pass the v2 suite. Its
second is met by hosted CI for correctness only. The debug layer and GPU-based validation judge
every command WARP executes, but WARP has no vendor driver, so it shows neither hardware behaviour
nor timing, and [ADR 0014](0014-overhead-measurement.md) measures D3D12 costs on the Windows GPU
machine.

## Decision

- The D3D12 backend is implemented in this repository against WARP on hosted Windows CI, under the
  debug layer and GPU-based validation. It remains ADR 0008's second native backend, no longer
  planned.
- Documents describe it as "implemented, verified on WARP only, not on hardware", never as
  supported or accepted. This rule replaces ADR 0008's founding rule.
- Acceptance is unchanged: the backend is accepted when it passes T1 unchanged on hardware
  ([ADR 0009](0009-conformance-and-valid-usage.md)). A WARP run is neither T1 acceptance nor a
  measurement.
- The default backend on Windows stays `null`. It becomes `d3d12` only after acceptance on
  hardware and a decision to change it.
- No timing and no overhead budget is judged on WARP (ADR 0014).
- The hardware run follows [the D3D12 hardware run](../guides/d3d12-hardware-run.md).

## Consequences

- What WARP cannot show stays open: vendor and driver behaviour, the `OPTIONS19` values shipping
  drivers report, timing, and the rules the API reference still marks `unverified` for hardware.
  Issues #20, #21 and #22 track them.
- The `d3d12` job of
  [CI run 39641225757](https://github.com/AmanThuL/rojo-rhi/actions/runs/39641225757) ran every T1
  conformance file the WARP build does not leave out, with none failing ([the T1 run on
  WARP](../architecture/d3d12-backend-edges.md#the-t1-run-on-warp)); it is not acceptance.
- `README.md` names D3D12 only in the qualified form, and `Tools/check_project_policy.py` enforces
  it.
- A later ADR records acceptance on hardware and supersedes this one's description of the backend.
