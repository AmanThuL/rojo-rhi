# Testing Conventions

**Status**: Proposed

Tests are Catch2 cases in one test tree, built as one test target. Names and tags are owned by
[naming.md](naming.md).

## Tiers

**CPU contract tests** create no device. They pin the shape of the public vocabulary: the error
domain and `Result<T>`, descriptor defaults, size and alignment arithmetic, per-frame data
bookkeeping and any pure helper the interface exposes. They run anywhere, including a hosted runner
with no GPU, and a failure here is a contract change rather than a driver difference.

**Validation tests** pin what the interface rejects. The predicates behind each `ROJORHI_ASSERT`
precondition are testable functions, so a case asserts that an invalid range, an out-of-bounds
subresource, a mismatched attachment or an impossible pipeline descriptor is reported as invalid,
and that the neighbouring valid case is accepted. They need no device: an interface is a pure
virtual class, so a fake implementation reporting a chosen shape is enough. These cases carry the
same CPU tag as the contract tier and run in the same places.

**GPU conformance tests** run against a real device and are the only tier that observes what the
backend actually does. They run with `MTL_DEBUG_LAYER=1` so the platform validation layer speaks up,
and with the test binary's build directory as the working directory, because the backend resolves
shader library paths relative to the current directory.

## The conformance filter

[ADR 0005](../decisions/0005-conformance-checkpoint.md) freezes eight semantic areas behind the
single tag `[checkpoint-a]`. The frozen set is exactly that tag as one filter, run from the test
build directory with the validation layer enabled. A future backend is accepted only when every
listed case passes unchanged — no edited assertion, no relaxed tolerance.

Adding the tag to a further case needs no ADR; it adds evidence for an area already frozen.
Removing a listed case's tag, or retagging it to a different case, requires a superseding ADR.

## What belongs in this repository

A test translation unit belongs here if and only if it includes no project header outside RojoRHI
and its own test support. The rule is checkable, so placement is decided by reading includes rather
than by judgement. A test that reaches a consumer's renderer, scene or editor headers stays with
that consumer, even when the behaviour it observes is ultimately RojoRHI's.

Test support follows the same rule: the bootstrap that creates a device and a shader library for a
GPU case is RojoRHI's, while a fixture that builds a consumer's scene is not.

## Shaders for tests

- Test oracles are the only shaders this repository owns. They live under `Shaders/Tests/` and are
  compiled into the test target's own directory.
- A smoke shader that both RojoRHI and a consumer need is copied, not shared. The duplication is
  deliberate: it buys independence, and a later divergence between the two copies is then a
  decision someone makes rather than a break someone suffers.
- Shader basenames must be unique, including case, within a test target. The build rule rejects a
  collision. Uniqueness is scoped per target, so a copied smoke shader keeping its original
  basename in the consumer's target is not a collision.

## Discipline

- Never disable, skip or delete a failing test to make a run green. Fix the behaviour, or fix the
  test when the contract genuinely changed and say so in the commit.
- A behaviour change and a test move are separate commits. A move preserves every case name, so the
  before and after inventories match by name.
- A new capability lands with the case that proves it. A bug fix lands with the case that fails
  without it.
- Choose evidence by risk:

| Change | Minimum evidence |
|---|---|
| Documentation or policy | the policy checks the repository has; link and manual review |
| CPU vocabulary or validation | formatting, the focused case, the full CPU suite |
| Interface or shader ABI | build, CPU suite, GPU smoke run with platform validation |
| Synchronization or lifetime | a stress or recycle case and a labelled GPU capture |
| Performance claim | a repeatable command, the device and configuration, several samples, the raw measurement |

Hosted runners have no Metal 4 device, so they compile the GPU cases and inventory them without
running them. The GPU tiers run on Metal 4 Apple Silicon before merge.

## Differences from Luminex

- **One test tree.** The planned test split leaves Luminex with two test targets and a rule for
  which cases live where. Inside this repository there is one target, so those rules collapse to
  the mover rule above, stated once.
- **Copied smoke shaders.** Luminex shares one shader tree across its targets. RojoRHI's copies of
  the shaders both sides need are duplication accepted for independence, since the two repositories
  must build without each other.
- **Split conformance inventory.** Luminex's frozen checkpoint covered cases on both sides of the
  boundary. RojoRHI owns the execution-substrate half; the transient-resource cases belong to the
  layer that owns transients and stay with the consumer. The two filters together equal the frozen
  inventory by case name.
- **No rendered-output tier.** Luminex compares fixed-camera images and temporal sequences. RojoRHI
  renders no scene, so its strongest evidence is an exact-value readback under the validation layer.
