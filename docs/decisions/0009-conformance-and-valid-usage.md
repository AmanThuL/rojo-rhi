# ADR 0009: Conformance is four test tiers and a valid-usage identifier per rule

**Status**: Accepted (2026-10-12)

**Supersedes**: [ADR 0005](0005-conformance-checkpoint.md)

## Context

ADR 0005 froze eight semantic areas behind one test tag, `[checkpoint-a]`, as the acceptance gate
for a second backend. It froze sixteen existing cases by name, and it recorded three limits. The
frozen set proves sufficiency of barriers and not their necessity, since a GPU that happened to
order accesses without a barrier let compute-hazard cases pass with the barrier recording removed.
One case asserted against a diagnostic counter scoped to the Metal 4 backend, so it could not run
unchanged elsewhere. And half of the frozen inventory lived in the consumer's test tree.

The v2 interface changes the position. A backend-neutral validation tracker and a backend that
needs no GPU (see [ADR 0008](0008-backends-and-vulkan-oracle.md)) make a missing barrier and a
misused call checkable on hosted CI. A reference in which every rule is individually identified
makes coverage a mechanical question where a frozen list of case names was a manual one. And the
suite must stand without any consumer.

## Decision

The test suite lives in this repository, covers the interface without any consumer, and uses Catch2
as before. It has four tiers.

| Tier | Runs on | Covers |
|---|---|---|
| T0 contract | Null backend, hosted CI | every operation; every valid-usage rule as a death test that asserts the expected diagnostic; counters asserted exactly |
| T1 conformance | Metal 4 on the Mac, D3D12 on the Windows machine | readback oracles per reference chapter: format classes, every copy path, each barrier kind and layout, load and store operations, indirect and counted draws, mesh tasks, multi-queue handoff, sparse mapping, frame-slot retirement, upscaler and presentation smoke cases |
| T2 benchmarks | both machines; counters on CI | the measurement suite owned by [ADR 0014](0014-overhead-measurement.md) |
| T3 shader ABI | hosted CI and both machines | Slang compiles for MSL, DXIL and SPIR-V; layout check; compile-and-run of the exact ABI ([ADR 0011](0011-shader-abi.md)) |

T1 runs on Metal 4 only until a D3D12 backend has been built; its Windows half follows that backend.

**Valid-usage identifiers.** Every valid-usage rule in the API reference carries a stable
identifier of the form `VU-rojo-<area>-<number>`, such as `VU-rojo-cmd-0002`. A rule is a
requirement on the caller whose violation is misuse, and misuse is a fatal `ROJORHI_ASSERT` (see
[ADR 0007](0007-address-first-interface.md)). Each check the validation tiers perform corresponds
to one identifier. A policy tool fails CI when an identifier has no test naming it. That coverage
tool arrives with the Null backend, because T0 is where the death tests live; the structure
checker for reference entries, `Tools/check_spec.py`, is separate and checks the entry grammar
only.

**Acceptance.** T1 cases carry the `[conformance]` tag, and a backend is accepted when it passes
every T1 case unchanged: no edited assertion and no relaxed tolerance, because relaxing a case to
fit a backend defeats its purpose. GPU cases run with the platform validation layer enabled, which
is `MTL_DEBUG_LAYER=1` on Metal 4. The `[conformance]` tag and this acceptance rule replace the
`[checkpoint-a]` tag and ADR 0005's frozen table as the gate for a backend. The table named cases of
the v1 interface and is not carried forward by name.

**Barrier necessity.** ADR 0005's caveat stands as the motivation for the validation tracker, and
it is resolved there rather than in the hardware tests. T1 still proves only that results are
correct with the declared barriers, on whatever ordering the GPU happens to provide. The tracker
exists only in non-Release builds. It records every barrier and every observable access:
attachments, copy and clear endpoints, index and indirect reads, upscaler and present images, and
shader accesses declared with `CommandList::declareAccess`. Undeclared bindless accesses through
heap indices or `Addr` are untracked. For the accesses it observes, it reports a missing barrier, a
stage or access that does not cover the actual use, a wrong old layout and a texture used in a
layout that does not permit the use. It is shared by every backend, so a missing barrier fails
identically on Null, Metal 4 and the planned D3D12 backend, including on hardware that would have
hidden it. T0 asserts that diagnostic, so necessity is tested without a more aggressively
reordering GPU.

**Counters.** Diagnostic counters are backend-neutral and always compiled in, so a test that
asserts against them runs unchanged on every backend. They replace the Metal-specific frame-data
counter that made one ADR 0005 case non-portable.

## Consequences

Coverage of the reference is a mechanical check. A rule without a test fails CI once the coverage
tool exists, and a test cannot silently drift from the rule it names because both carry the same
identifier. Identifiers stay stable once published, so a test, a diagnostic and a reference entry
refer to one rule across revisions.

The acceptance gate for a second backend is concrete and lives wholly in this repository: build
against the same public headers, wire the backend, run the `[conformance]` filter, and every case
passes unchanged. A green T1 run is still evidence of correct results under the barriers as
declared and not of complete synchronization; the tracker, not T1, provides that guarantee, and
only as far as its model of accesses is accurate.

Costs: T0 needs the Null backend and the tracker before the death tests can exist, so the coverage
tool and the T0 suite are delivered with them and not before. T1 and T2 need the two machines and
cannot run on hosted CI, which asserts counters only. Until the coverage tool lands, the
identifier rule is enforced for entry structure only, and this ADR does not claim more.
