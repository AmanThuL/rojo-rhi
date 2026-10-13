# 00. Conventions

**Status**: Accepted
**API version**: 0.1.0
**Entries**: none

This chapter fixes how the RojoRHI API reference is written and read: the chapter head, the entry
grammar, the valid-usage notation and its identifiers, and the cost classes every mapping table
uses. It defines no interface of its own.

## Scope and backends

The reference specifies the public C++ interface of RojoRHI, namespace `rojoRHI`, headers rooted at
`<rojoRHI/...>`, at the API version stated in each chapter head. An entry's semantics and valid
usage bind every backend; its mapping table records how each backend lowers it.

Metal 4, through metal-cpp, is the only backend RojoRHI implements today. The D3D12 column of every
mapping table describes the designed mapping of a planned production backend, and the Vulkan column
describes a design oracle that has no code. Neither column states a capability RojoRHI has. Until
the Metal 4 backend implements the API version a chapter describes, its Metal 4 column is also a
designed mapping, checked against the native API rather than against shipped code.

The Null recording backend, which arrives with the validation tracker, implements every entry
without a GPU for contract tests. It is a test backend, never a shipping one, and has no column.

## Chapter head

Every chapter opens with the same head, within its first 30 lines:

```markdown
# NN. Title

**Status**: Proposed
**API version**: 0.1.0
```

- `NN` is the two-digit chapter number, or a capital letter for an appendix.
- `**Status**` takes the values the documentation convention defines. A chapter is `Proposed`
  while open for decision and `Accepted` once binding. A chapter that waits on a measurement names
  it after the value, as in `Proposed (until spike 2 of ADR 0014 reports)`.
- `**API version**` is the Semantic Versioning 2.0 version of the interface the chapter describes,
  the same version `ROJORHI_VERSION_*` reports (see [fundamentals](01-fundamentals.md)). A chapter
  that changes an entry states the version that change ships in.
- A chapter holding no entries (this one, the appendices, reserved chapters) adds
  `**Entries**: none` to its head.
- A chapter that would exceed the 300-line documentation budget is split into `NN-<topic>.md` and
  `NN-<topic>-part2.md`; both carry the full head.

## Entry grammar

An entry specifies one public function, one descriptor struct, one enumeration, or one symbol of
the Slang module `RojoRHI.slang`. Every entry is defined in exactly one chapter; other chapters
link to the owning chapter instead of repeating it. Entry names take three forms: `Type::function`
for a member function, a bare name for a type, a free function or a macro family, and
`rojoRHI::name` for a Slang symbol.

An entry consists of, in order:

1. A level-three heading holding only the entry name in code font.
2. A signature block fenced as `cpp`, or `slang` for shader symbols, holding the exact
   declaration. Enumerators and struct fields are listed with their defaults, and each is defined
   in the entry.
3. Semantics: what the call does, what it returns, when its effects become visible, which thread
   may call it, and which errors it returns.
4. A `#### Valid usage` section holding one bullet per rule, or the single bullet `- None.`.
5. A `#### Mapping` section holding a table whose header is exactly
   `| | Metal 4 | D3D12 | Vulkan |` and which includes a `Cost` row.

The skeleton, shown indented here so it is not itself an entry:

````markdown
  ### `Device::createBuffer`
  ```cpp
  Result<Buffer> Device::createBuffer(const BufferDesc& desc, std::span<const std::byte> initial = {});
  ```
  Semantics prose.
  #### Valid usage
  - `VU-rojo-buffer-0007` `initial.size()` is at most `desc.size`.
  #### Mapping
  | | Metal 4 | D3D12 | Vulkan |
  |---|---|---|---|
  | Call | `MTL::Device::newBuffer` | `ID3D12Device::CreateCommittedResource` | `vkCreateBuffer` |
  | Cost | native | native | native |
````

`Tools/check_spec.py` checks the `**API version**` line of each head, that a chapter holds entries
or declares `**Entries**: none`, each entry's signature block, valid-usage bullets and `Cost` row,
and that entry names and identifiers are unique across every chapter. The title and `**Status**`
lines are held by review and by the project policy check, not by this tool.

## Valid usage

A valid-usage rule is a condition the caller must establish before or during a call. Each rule is
a single statement about caller-visible state that one test can violate on purpose and see
diagnosed. A requirement no tier can diagnose, such as a data race or the use of a destroyed object
handle, is stated as normative prose in the entry, not as a rule.

- **Notation.** A rule is written `` - `VU-rojo-<area>-NNNN` <condition>. ``: `area` is the
  chapter's area token in lowercase letters, `NNNN` four decimal digits starting at `0001`.
- **Areas.** Each area token belongs to one chapter, as the chapter list below shows. A chapter
  numbers its rules in the order it first writes them.
- **Stability.** An identifier is never renumbered and never reused. A rule that is withdrawn keeps
  its identifier and its bullet, rewritten as `Retired in <version>:` followed by the former
  condition; a rule whose meaning changes is retired and replaced by a new identifier. A new rule
  takes the next number never used in its area.
- **Consequence.** Violating a rule is misuse of the interface, not an error. A tier that checks the
  rule reports the identifier and a message through the message callback and then terminates the
  process. Where no enabled tier checks it, the violation is undefined behaviour.
- **Who checks.** Rules about arguments, handle generations and scope are checked in non-Release
  (Debug) builds through `ROJORHI_ASSERT`. Rules that need the history of GPU accesses, barriers,
  queues or lifetimes are checked by the validation tracker, which non-Release (Debug) builds
  enable per device through `DeviceDesc::validation`; such a rule says so. The tiers are defined in
  [fundamentals](01-fundamentals.md).
- **Coverage.** Every identifier is to be named by at least one test that violates it and asserts
  the reported diagnostic. The tool that fails the build on an untested identifier arrives with the
  Null test backend.

## Mapping tables and cost classes

A mapping table names, per backend, the concrete native calls and types an entry lowers to:
metal-cpp names such as `MTL4::CommandQueue`, D3D12 names such as
`ID3D12Device::CreateCommittedResource`, Vulkan names such as `vkCreateBuffer`. A cell that rests
on a native behaviour not yet confirmed against the native documentation or a run says
`unverified` in its note.

The `Cost` row gives one cost class per backend. A cell starts with the class, optionally followed
by a colon and a short note. There are exactly four classes:

| Class | Meaning |
|---|---|
| `native` | The entry lowers one-to-one onto the backend's own call or type; the backend adds only argument translation. |
| `cheap` | The backend adds small, fixed work per call: a table lookup, one extra load or write, a deferred-release queue entry. Nothing scales with the number of resources or draws. |
| `costly` | The backend emulates: tracking, hidden pipeline variants, shader rewriting, extra dispatches or extra memory, with work that grows with use. The note says what the cost is and, where one exists, which measurement decides it. |
| `impossible` | The backend cannot express the entry. The entry is unavailable there, and the reference says how a caller learns that: a capability or an `ErrorCategory::Unsupported` error. |

A class describes the cost of the RojoRHI entry over the best hand-written native code for the
same work, on the CPU or the GPU, whichever is larger; the note says which side pays. Rows other
than `Cost` are free-form and name their subject in the first column (`Call`, `Object`, `Limit`).

## Terms

- **Caller**: the code calling RojoRHI, usually a renderer and its render graph.
- **Live**: an object that has been created and not passed to `Device::destroy`.
- **Null handle**: a handle whose value is all zero, written `{}`. An entry says where it accepts
  one.
- **Open frame**: the frame between `Device::beginFrame` and the matching `Device::endFrame`.
- **Retired**: a frame whose GPU work has completed, observed by a later `Device::beginFrame`.
- **Validation tracker**: the backend-neutral code, present only in non-Release (Debug) builds,
  that records every barrier and every access it can observe when `DeviceDesc::validation` is set
  ([diagnostics](14-diagnostics.md)).

## Chapters and area tokens

| Chapter | Area |
|---|---|
| [00 Conventions](00-conventions.md) | none |
| [01 Fundamentals](01-fundamentals.md), [part 2](01-fundamentals-part2.md) | `core` |
| [02 Device](02-device.md) | `device` |
| [03 Memory and buffers](03-memory-and-buffers.md), [part 2](03-memory-and-buffers-part2.md) | `buffer`, `heap` |
| [04 Textures and heaps](04-textures-and-heaps.md), [part 2](04-textures-and-heaps-part2.md) | `texture` |
| [05 Shader ABI](05-shader-abi.md), [part 2](05-shader-abi-part2.md), [part 3](05-shader-abi-part3.md) | `abi` |
| [06 Pipelines](06-pipelines.md), [part 2](06-pipelines-part2.md) | `pipeline` |
| [07 Command recording](07-command-recording.md), [part 2](07-command-recording-part2.md), [part 3](07-command-recording-part3.md) | `cmd` |
| [08 GPU-driven rendering](08-gpu-driven.md) | `indirect` |
| [09 Synchronization](09-synchronization.md), [part 2](09-synchronization-part2.md) | `sync` |
| [10 Queues and frames](10-queues-and-frames.md) | `queue` |
| [11 Presentation](11-presentation.md) | `present` |
| [12 Upscaling](12-upscaling.md) | `upscale` |
| [13 Sparse resources](13-sparse.md) | `sparse` |
| [14 Diagnostics](14-diagnostics.md) | `diag` |
| [15 Formats and limits](15-formats-and-limits.md), [part 2](15-formats-and-limits-part2.md) | `format` |
| [A Cost summary](A-cost-summary.md) | none |
| [B Reserved: ray tracing](B-reserved-ray-tracing.md) | none |
