# Testing Conventions

**Status**: Accepted

Tests are Catch2 cases in one test tree per interface, built as one test target. Names and tags are
owned by [naming.md](naming.md). Today the v1 suite, including the `[checkpoint-a]` cases, lives
under `Legacy/Tests/` and builds as `RojoRHILegacyTests`. The top-level `Tests/` holds the v2
interface's cases and builds as `RojoRHITests`; its T0 (CPU contract) cases run as
`xmake test RojoRHITests/contract` (ADR 0017), its death tests as `xmake test RojoRHITests/death`
and its shader ABI cases, tagged `[abi]`, as `xmake test RojoRHITests/abi`. Built with the
default backend on macOS (`metal4`), the same binary also runs the `[conformance]` cases as
`xmake test RojoRHITests/conformance`.

## Tiers

**CPU contract tests** create no device. They pin the shape of the public vocabulary: the error
domain and `Result<T>`, descriptor defaults, size and alignment arithmetic, per-frame data
bookkeeping and any pure helper the interface exposes. They run anywhere, including a hosted runner
with no GPU, and a failure here is a contract change rather than a driver difference.

**T0 contract tests** of the v2 interface are backend-neutral and run on Null with no GPU, and on
Metal 4 under the validation layer. The test tree is split by backend. `Tests/Contract/` holds the
backend-neutral reference rules, one file per area, tagged `[contract]`: every expectation there
must hold for any backend, so it states only what the reference states, and it includes no backend
header. A case that loads a shader library takes its blob from `Support/Backend.h`, which supplies
each backend's standard library (`Shaders/Tests/Standard.slang`: `vsMain`, `fsMain`, `tsMain`,
`msMain` and `csMain`, which touch no reserved binding; Metal 4 reads the packaged
`Shaders/Standard.metal4lib` from the test directory), that library re-stamped with another ABI
version and a foreign library, a well-formed blob of the other backend, which the backend must
refuse. A case that needs a conformance shader reads it with `shaderLibraryFile`. `Tests/Null/`
holds what only the Null backend promises (the values it reports for limits and formats), tagged
`[null]`. `Tests/Metal4/` holds what only the Metal 4 backend makes (native call counts, residency
commits, the descriptors it builds), tagged `[metal4]`; it may include `Backends/Metal4/Source`
headers and metal-cpp. `Tests/Core/` holds the core's internals, such as the handle pool, tagged
`[core]`, and may include private headers; it builds with Null only, as `Tests/Null/` does.
`Tests/Support/` holds shared helpers, and `Tests/Support/Null/` and `Tests/Support/Metal4/` the
helpers of each backend (the Metal 4 ones create a surface, read Private memory back, report
`runsOnGpu` and guard counts that include timestamp writes): `Death.h`, `Null/NullLibrary.h` (builds
Null shader library blobs, including ones the backend must refuse), `PassKit.h` (a device with an
open frame, a library, a list, attachment textures and pipelines for recording tests; it loads its
library on first use) and `GpuKit.h` (a device with an open frame that runs lists to completion on
the GPU and reads buffers and textures back through public entries, for conformance cases). On Null
the `contract` entry runs `[contract]`, `[core]` and `[null]` and excludes `[death]`; on Metal 4 it
runs `[contract]` and `[metal4]`, with the Metal 4 entries running from the test directory under
`MTL_DEBUG_LAYER=1`. Release CI runs it too. A failing validation-layer message is a failure.

The Metal 4 surfaces (`test::surface`) are `CAMetalLayer`s outside any window, created on first
use and shared by the process; each starts with a drawable size of 256 x 256, as a Null surface
has an extent from the start, and a case that resizes a shared surface leaves it resized for the
rest of the process, so a case that changes the size either uses a layer of its own or restores it.
The `[capture]` cases of `Tests/Metal4/Metal4CaptureTests.cpp` depend on the launch environment:
the case that expects `Unsupported` runs only without `MTL_CAPTURE_ENABLED`, and the three that
write a `.gputrace` document, refuse a second capture and refuse a path below a regular file run
only with `MTL_CAPTURE_ENABLED=1`. Each skips itself otherwise, so a default run passes one and
skips three, and a run with the variable set passes three and skips one.

A Metal 4 run is also checked for `autoreleased with no pool` lines under
`OBJC_DEBUG_MISSING_POOLS=YES`, and the check is judged by their stacks: a line with a RojoRHI frame
is a failure. Presenting a drawable makes Apple's FramePacing and submission threads print such
lines with no RojoRHI frame, and so do the IOGPU completion threads, where the driver frees a
completed command buffer's resources. A RojoRHI frame that only pushes an autorelease pool, such as
a completion handler's pool, does not count: the push autoreleases nothing and reports an earlier
driver autorelease. A run with `MTL_CAPTURE_ENABLED=1` prints more from the capture
tool's XPC code; neither counts, so a count of zero holds only for a run that presents nothing and
captures nothing ([the Metal 4
edges](../architecture/metal4-backend-edges.md#presenting-under-objc_debug_missing_pools) lists the
sources).

Metal 4 builds every file of `Tests/Contract/`, as Null does. A contract case whose setup the
reference lets an implementation refuse, such as an upscaler extent, skips where creation returns
an error.

**Death tests** pin each Debug-tier valid-usage rule (ADR 0009). A death test is a `TEST_CASE` named
`"<identifier> <behaviour>"`, such as `"VU-rojo-device-0003 beginFrame runs while a frame is open"`,
tagged `[contract][death]` and stored in a file named `*DeathTests.cpp`. It holds one identifier and
its body is `ROJORHI_EXPECT_DEATH("<identifier>", [&] { ... })`. The helper re-executes the test
binary on that one case with `ROJORHI_DEATH_CHILD=1` (any other value of the variable in the
parent's environment fails the case without spawning); the child runs the lambda, which must
terminate the process, and the parent requires a non-zero exit and a standard-error report in which
the first `<identifier>:` is the expected one. A rule checked at several call sites gets one death
test per site, so each site is exercised and a deleted check fails a test; the coverage tool counts
the sites. Death tests exist only in
Debug builds: `ROJORHI_VU` is compiled out of Release, so the helper skips there. The harness's own
self-tests (`[death][!shouldfail]` cases that must fail, and `quoteWindowsArgument`) are the one
exception to the naming rule, since they test no identifier.

**The coverage tool** `Tools/check_vu_coverage.py` runs in the policy CI job. It collects every
non-retired identifier in `docs/spec/` and every identifier named by a `[death]` case in `Tests/`
(comments and `#if 0` blocks do not count, and the case's `ROJORHI_EXPECT_DEATH` argument must be
the identifier in its name), and fails when an identifier has no death test and is not listed in
`Tools/vu_pending.txt`. It also counts the `ROJORHI_VU` call sites of each identifier in `Source/`
and fails when an identifier has fewer death tests than sites, which enforces the per-site rule
above; it fails on any `ROJORHI_VU` under `Backends/`, since every check lives in the core, and on
an identifier that a check or a death test names but the reference does not define or has retired.
The pending list holds `<identifier> <part>` lines sorted by identifier, naming the part of the
work (`resources`, `recording`, `edges` or `tracker`) that will add the test; no line names
`resources` now that the resource entries exist.
The tool also fails when a listed identifier has a death test, so the list only shrinks, and when it
names an unknown identifier or part. A part removes its lines in the change that adds the tests.

**Conformance tests** (`[conformance]`, `Tests/Conformance/`) are the v2 interface's T1 cases (ADR
0009): cases that observe on a GPU what the API reference promises. They check fence values,
CPU-visible buffer memory, acquiring and presenting images across image counts, latency limits and
frame counts, and readback oracles of GPU-written data: `Copy` (every copy path), `InitialData`,
`Barrier` (each barrier kind, and that a consumer barrier holds back every later encoder), `Fill`,
`Clear` (render targets, depth and Storage-only textures of every type), `Handoff` (work chained
through the three queues by fences), render passes with their load and store actions, direct,
indirect and counted draws, mesh tasks and dispatches, `Sparse` (tile mapping and the measured
tile shapes), `Scope` (scope, pass and upscale timings against fixed bounds), `Upscale` (the MetalFX
upscaler and its mask), `Format` (every format class on the GPU) and `WriteOnly` (storage textures
written through write-only accessors), over `Tests/Support/GpuKit`, which runs lists
and reads results back through public entries and waits on the host after each run. They compile on
every backend and each starts with `test::requireGpu()` from `Tests/Support/Conformance.h`, which
skips the case where `test::runsOnGpu()` is false: always on Null, never on Metal 4, whose devices
need a GPU; Catch2 exits non-zero when every case skips, which is why Null has no `conformance`
entry. They use public entries only and run on Metal 4 under `MTL_DEBUG_LAYER=1`. A case whose
commands a kernel encodes into an indirect command buffer runs under API validation only:
`MTL_SHADER_VALIDATION` silently drops such commands, so a run under it proves nothing. On Metal 4
a case that leaks a device keeps its counter heaps, and Metal allows 32 per process or on the
system, so timed cases after it may report 0; a case destroys its devices on every path, as the
fixtures do. For the same reason no other GPU test process may run while timed cases, or the
`[metal4]` count pins that include timestamp writes, are checked. Those pins read a list's counts
through `test::timedCounts` or call `test::requireTimed` (`Tests/Support/Metal4/Metal4Timing.h`)
first, which fails the case, naming the refusal, when a scope the list recorded holds no timestamp
pair, rather than reporting a count two calls short.

**Validation tests (v1)** pin what the interface rejects. The predicates behind each
`ROJORHI_ASSERT` precondition are testable functions, so a case asserts that an invalid range, an
out-of-bounds subresource, a mismatched attachment or an impossible pipeline descriptor is reported
as invalid, and that the neighbouring valid case is accepted. They need no device: an interface is a
pure virtual class, so a fake implementation reporting a chosen shape is enough. These cases carry
the same CPU tag as the contract tier and run in the same places. They describe the v1 suite; the v2
equivalent is the death test.

**Shader ABI tests** are the hosted half of the T3 tier (ADR 0009). They need no GPU: every
conformance shader must compile to MSL, SPIR-V and HLSL, and the layout of each shared struct must
match what Slang reflects on every target, which the build checks with generated `static_assert`s.
The `[abi]` cases also check the header of every Metal shader library where the Metal toolchain
exists (skipped elsewhere), and that every SPIR-V heap fetch is decorated NonUniform. They run on
any hosted runner that has the pinned Slang. Compile-and-run of the ABI needs a backend that loads
shader libraries: on Metal 4 the `[conformance]` cases run every ABI vertex, fragment, compute and
texture kernel through the public entries and compare exact results, which is the GPU half of the
T3 tier.

**GPU conformance tests** run against a real device and are the only tier that observes what the
backend actually does. They run with `MTL_DEBUG_LAYER=1` so the platform validation layer speaks up,
and with the test binary's build directory as the working directory, because the backend resolves
shader library paths relative to the current directory.
[Recording GPU evidence](../guides/gpu-evidence.md) says where a run's raw output is kept and how
it is checksummed.

## The conformance filter

[ADR 0005](../decisions/0005-conformance-checkpoint.md) freezes eight semantic areas behind the
single tag `[checkpoint-a]`. The frozen set is exactly that tag as one filter, run from the test
build directory with the validation layer enabled (`Legacy/Tests/` holds the cases today). A future
backend is accepted only when every listed case passes unchanged — no edited assertion, no relaxed
tolerance.

Adding the tag to a further case needs no ADR; it adds evidence for an area already frozen.
Removing a listed case's tag, or retagging it to a different case, requires a superseding ADR.

## What belongs in this repository

A test translation unit belongs here if and only if it includes no project header outside RojoRHI
and its own test support. The rule is checkable, so placement is decided by reading includes rather
than by judgement. A test that reaches a consumer's renderer, scene or editor headers stays with
that consumer, even when the behaviour it observes is ultimately RojoRHI's.

**Benchmark checks** are Catch2 cases in `Benchmarks/Checks/`, run by `RojoRHIBench check` and
described in [the benchmark harness](../architecture/benchmarks.md). `[host]` cases need nothing
and run everywhere. `[null]` cases run every RojoRHI workload on the Null backend and assert its
counters and call sequence, and `xmake test RojoRHIBench/null` runs both tags in the macOS
interface CI job, in Debug and Release; the `metal4` job does not build the harness. `[native]`
cases check the native arm against CPU oracles under `MTL_DEBUG_LAYER=1` with
`xmake test RojoRHIBench/native`, on local Apple Silicon only; CI requires their inventory to be
non-zero. `[metal4]` holds the counter case of a `metal4` build, run by
`xmake test RojoRHIBench/metal4`. Timed runs are not tests: they follow ADR 0014.

Test support follows the same rule: the bootstrap that creates a device and a shader library for a
GPU case is RojoRHI's, while a fixture that builds a consumer's scene is not.

## Shaders for tests

- Test oracles and benchmark workloads are the only shaders this repository owns. Oracles live
  under `Shaders/Tests/` and the benchmark's workloads under `Shaders/Bench/`, prefixed `Bench`;
  v1's oracles live under `Legacy/Shaders/Tests/`. Each is compiled into its target's own
  directory.
- A smoke shader that both RojoRHI and a consumer need is copied, not shared. The duplication is
  deliberate: it buys independence, and a later divergence between the two copies is then a
  decision someone makes rather than a break someone suffers.
- Oracles compare exact values: integers held in floats that represent them exactly (`Format` has no
  integer format) or `k / 255` values. They fail if a command, a state or an execute were skipped. A
  value a shader reads from a normalized format is compared within half a step of its channel, which
  identifies the stored code; what a format stores is compared byte for byte. `DrawOracle` (draws,
  state and indirect draws), `MeshOracle` (task and mesh entries), `MeshRootOracle` (a mesh entry
  that reads the root), `PullOracle` (vertex pulling through the root), `DispatchOracle` (the
  compute grid) and `FormatOracle` (sampling, loading, storing and reading the samples of every
  format) hold the entries those cases use. A vertex entry reads no reserved binding unless it must
  pull its vertices: the pinned Slang lowers every reserved binding to a writable `device` pointer,
  and Metal warns "writable resources in non-void vertex function" on such an entry, which
  `PullOracle` and the benchmark's vertex shaders accept as chapter 05 documents.
- Shader basenames must be unique, including case, within a test target. The build rule rejects a
  collision. Uniqueness is scoped per target, so a copied smoke shader keeping its original
  basename in the consumer's target is not a collision.

## Discipline

- Never disable, skip or delete a failing test to make a run green. Fix the behaviour, or fix the
  test when the contract genuinely changed and say so in the commit.
- Branch on `NDEBUG` inside a test body, never around a definition.
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
