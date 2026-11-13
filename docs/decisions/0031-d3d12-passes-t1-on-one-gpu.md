# ADR 0031: D3D12 passes T1 on one GPU

**Status**: Accepted (2026-11-13)

**Supersedes**: [ADR 0026](0026-d3d12-implemented-before-hardware.md)'s description of the backend,
its second decision, "Documents describe it as "implemented, verified on WARP only, not on
hardware", never as supported or accepted", and its consequence that `README.md` names D3D12 only
in that form; and the first sentence of [ADR 0027](0027-hosted-ci-compiles-d3d12.md)'s third
decision, "D3D12 stays "implemented, verified on WARP only, not on hardware"". The rest of both
stands.

## Context

ADR 0026 implemented D3D12 before any hardware, described it as verified on WARP only, and left
the record of a pass on hardware to a later decision. Acceptance is ADR 0009's: a backend passes
every T1 case unchanged, with the platform validation layer on
([ADR 0009](0009-conformance-and-valid-usage.md)).

[The first hardware run](../research/2026-11-12-d3d12-hardware-run.md) followed [the hardware run
guide](../guides/d3d12-hardware-run.md) on an NVIDIA GeForce RTX 3080 Ti on driver 621.14
(32.0.16.2114), Windows 11 build 26300, at 9d60f85, in Debug under the debug layer and GPU-based
validation and in Release. Every entry passed at or above the guide's floors: `[d3d12]` 260 of 262
cases, `abi` 11 of 13, `contract` 600 of 603 in Debug, `conformance` 182 of 191 in both builds,
`imgui` 15 of 15 and `death` 553 of 557, its two failures the harness's own expected ones. Each
skip prints its reason, and the note accounts for every one. The debug layer reported no message
of any severity in any conformance run.

Two changes to the reference came first, both binding every backend and both merged.
[ADR 0030](0030-view-floor-is-tier-3-heap-less-reservation.md) set the `maxViews` floor at tier 3's
heap less the D3D12 reservation, because the GPU reports a view heap of exactly 1,000,000. Chapter
15 and its format case widened the bound on a filtered sRGB mean from half an 8-bit step to one,
because the GPU returns its means on a grid of 1/512. ADR 0009 forbids a tolerance relaxed to fit a
backend. This decision holds that the second change is not one: the case asserted a filtering
precision the reference never stated, the bound now stated binds Metal 4 and Null too, and one
step still separates a mean taken in linear space from one taken in encoded space by over 0.09.

[The markers note](../research/2026-11-13-d3d12-hardware-markers.md) ran new cases on the same GPU.
Its case for item 17 failed in Release: the discard lowering did not wait for its source stages on
this driver, a backend defect that pull request #181 fixed; the case now passes in Debug and in
Release. That case was added after the T1 run and is not part of its counts.

[The first D3D12 overhead round](../research/2026-11-12-d3d12-overhead-results.md), on the same
machine, misses [ADR 0014](0014-overhead-measurement.md)'s CPU budget in 19 of 32 judged
configurations. Overhead is a separate gate from conformance: ADR 0009 accepts a backend on T1, and
ADR 0014 judges its cost. Issue #21 stays open for the budgets.

## Decision

- D3D12 passes T1 unchanged on hardware on one GPU, an NVIDIA GeForce RTX 3080 Ti on driver
  621.14, in Debug under GPU-based validation and in Release. That meets ADR 0009's condition on
  that GPU of one vendor, and on no other.
- Documents describe the backend as "implemented and conformant (T1) on one GPU, an NVIDIA GeForce
  RTX 3080 Ti on driver 621.14, with its overhead budgets not yet met and other vendors
  unverified". This rule replaces ADR 0026's. A sentence about a narrower piece of evidence, such
  as what a WARP run or one case showed, may state that evidence instead.
- No document calls D3D12 supported, or accepted without naming the GPU: one GPU of one vendor is
  not every Windows GPU. No document infers the overhead result from the conformance result, or
  the reverse.
- The Windows default backend stays `null`. ADR 0026 made `d3d12` the default only after
  acceptance on hardware and a decision to change it; this record makes no such decision, which
  stays the owner's.
- `README.md` names D3D12 only in a sentence that holds "conformant (T1) on one GPU", "overhead
  budgets not yet met" and "other vendors unverified", and neither "support" nor "accept";
  `Tools/check_project_policy.py` enforces it.
- The description changes again only by a decision record, when a round meets ADR 0014's budgets
  or T1 passes on another vendor's GPU.

## Consequences

- `AGENTS.md`, `README.md`, the conventions, the architecture pages, the API reference and the
  guides carry the new description. Research notes keep the wording of their dates.
- What one GPU cannot show stays open: AMD and Intel GPUs and other NVIDIA drivers, the reference's
  markers that rest on a vendor's behaviour (issue #22), the pipeline cache's cross-adapter paths
  (#94), DLSS and XeSS, and a PIX capture.
- Hosted CI still executes nothing on D3D12 ([ADR 0027](0027-hosted-ci-compiles-d3d12.md)), so
  every D3D12 conformance run is a local run on the GPU machine, recorded as
  [recording GPU evidence](../guides/gpu-evidence.md) says. `--rojorhi_d3d12_adapter=warp` stays
  for a Windows build without a GPU, and a WARP run is still no T1 result.
- A conformance case that fails on this GPU is a backend defect, fixed under the guide's rule, as
  item 17's is; no case is edited to pass.
- ADR 0026's other decisions stand: the backend is implemented in this repository, nothing is
  timed on WARP, and the hardware run follows the guide.
