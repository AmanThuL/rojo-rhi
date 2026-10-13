# The Null Backend and the Shared Core

**Status**: Accepted

This page describes the v2 implementation as it stands: a backend-neutral core, the Null backend
that implements the public entries on top of it, and the Debug tier that checks valid usage. The
Null backend of [ADR 0008](../decisions/0008-backends-and-vulkan-oracle.md) renders nothing and
needs no GPU; it exists so the contract of
[ADR 0009](../decisions/0009-conformance-and-valid-usage.md) can be tested on any machine. The API
reference under `docs/spec/` stays the authority on every entry's semantics; this page records how
the code realises it.

## What exists

| Area | Where | Content |
|---|---|---|
| Public headers | `Include/rojoRHI/` | `Result.h`, `Handles.h`, `Geometry.h`, `Message.h`, `Device.h`, `DeviceDesc.h`, `Capabilities.h`, `Limits.h`, `UpscalerKind.h`, `Format.h`, `FormatCapabilities.h`, `SamplerIndex.h`, `ViewIndex.h`, `Swapchain.h`, `Addr.h`, `IndirectArgs.h`, `Version.h`, and the umbrella `RHI.h` |
| Private base | `Source/Base/` | `ROJORHI_VU`, `ROJORHI_INTERNAL_ASSERT`, the message sink |
| Shared core | `Source/Core/` | `HandlePool`, `DeviceCore`, the format capability rules, label checks |
| Null backend | `Backends/Null/Source/` | `NullDevice`: the public `Device` entries, translating to the core |
| Tests | `Tests/Contract/`, `Tests/Core/`, `Tests/Null/`, `Tests/Support/` | see [testing](../conventions/testing.md) |

The implemented entries are the device (creation, destruction, frames, capabilities, format
capabilities, `destroy` of every handle type) and the process-wide message callback. Resources,
queues, shader libraries, pipelines, command recording, swapchains and the rest of the reference
are not implemented; each arrives with the work that adds it.

## Two layers

`Source/` holds the backend-neutral core: everything that decides whether a call is legal or what a
frame boundary means. `Backends/Null/Source/` defines the public member functions and does nothing
but translate them into core calls and a Null-specific answer. A backend has no virtual interface
([ADR 0012](../decisions/0012-language-and-dispatch.md)): one backend is linked per build, selected
by the `rojorhi_backend` xmake option, and the public classes wrap an opaque pointer to the
backend's `DeviceImpl`.

`DeviceImpl` embeds one `DeviceCore`. The core owns the handle pools, the frame bookkeeping, the
label rules, deferred destruction and every Debug-tier check on the device. A check never lives in
a backend, so a later backend reuses all of them unchanged and adds only the translation; a
backend must call the core checks from every entry it defines, or the rule goes unchecked there. The
core reaches a device through `coreOf(Device)`, which each backend defines; the contract tests use
the same seam.

`RojoRHI` is a static library of `Source/**/*.cpp` and the selected backend's sources. The public
headers include nothing from either directory.

## The Debug tier

`ROJORHI_VU(cond, id, format, args...)` in `Source/Base/Assert.h` checks one valid-usage rule when
`NDEBUG` is not defined. On failure it sends `<id>: <text>` to the message callback at error
severity and calls `std::abort()`. In a Release build it expands to nothing and evaluates neither
the condition nor the arguments. `ROJORHI_INTERNAL_ASSERT(cond, msg)` guards invariants of the
library's own code that no caller can break; it reports `internal error: <msg> (<file>:<line>)` and
aborts, and is also absent from Release. A creation entry that can fail without misuse returns
`Result<T>` in every build.

The message sink is one process-wide callback behind a mutex. A message emitted while the callback
runs on the same thread goes to the default sink, which writes `[RHI] [<severity>] <text>` to
standard error, so a callback that triggers a check does not re-enter itself.
`setMessageCallback` called from inside the callback is itself a violation (`VU-rojo-core-0008`).

## Handles

A resource handle is a 64-bit value: a 32-bit slot index in bits 0 to 31, a 24-bit generation in
bits 32 to 55 and an 8-bit type tag in bits 56 to 63. Slot index 0 and tag 0 are never issued, so
the zero value is the null handle. Tags are fixed per type: Buffer 1, Texture 2, Pipeline 3,
ShaderLibrary 4, Heap 5, Fence 6, Upscaler 7.

Each device owns one `HandlePool` per resource type, each with its own mutex. A lookup reports the
first failing rule: `VU-rojo-core-0001` for a null handle where a live one is required,
`VU-rojo-core-0002` for a stale generation (destroyed or never issued), `VU-rojo-core-0003` for
another type's tag. Releasing a slot advances its generation at once, so the old handle is
rejected immediately. The slot returns to the free list only after the frame that released it
retires, and a record handed to the retire callback is cleared afterwards. A slot whose generation
would pass 2^24 - 1 is retired for good and never reused; its record still reaches the callback.
The `Device::destroy` overloads for `ViewIndex`, `SamplerIndex` and `Swapchain` only check the
device, since those objects do not exist yet; a default-constructed `Swapchain` is rejected
(`VU-rojo-core-0004`).

`Handles.h` holds the seven small handle structs and `Geometry.h` the four geometry structs
(`Extent2D`, `Extent3D`, `Offset3D`, `Rect2D`); each is a single-entry header for several types
that have no behaviour beyond their fields ([naming](../conventions/naming.md)).

## Frames and deferred destruction

Frames are numbered from one; frame N uses slot N modulo `framesInFlight`, which is 2 or 3
(`VU-rojo-device-0001`). `DeviceCore` keeps the frame number and an open flag in one atomic word,
so the free-threaded `destroy` reads a consistent pair while the frame class writes it. An object
destroyed while a frame is open is released with that frame's slot; one destroyed with no frame
open is released with the next frame opened. `beginFrame` retires the slot it reuses: every pool
releases the records destroyed at frames up to N minus `framesInFlight`, passing each to the
retire hook its backend registered (`RetireHooks`, one function pointer per handle type).
Frame misuse is `VU-rojo-device-0002` to `0004`. `destroyDevice` with validation selected warns
once per live object, by label, then passes every live and pending record through the same hooks
exactly once. The core also keeps the `Capabilities` and the format table, which the backend fills
at creation, so every capability and format query is one core path.

Labels are checked as UTF-8 without NUL (`VU-rojo-core-0006`, `0007`) and an empty device label
becomes `rojorhi.device.unnamed`; any other label is kept verbatim.

## Null semantics

The Null device is deterministic and does no work.

- `capabilities` reports the floor name `"Null"` and the adapter name `"Null"`, every optional
  feature as supported (upload in device memory, placement sparse, mesh task indirect,
  conservative rasterisation) and every upscaler kind except `Auto`.
- `Limits` takes the Floor column of the limits table in
  [formats and limits](../spec/15-formats-and-limits-part2.md) wherever that column gives a number.
  `maxBufferSize` has no number there, so the Null device reports 2^32 bytes, a value above the
  reach of an `Addr` that is not verified against any hardware. `maxViews` and `maxSamplers` in
  `Capabilities` repeat the limits.
- `formatCapabilities` reports, for each format, every capability the reference allows its class
  (`formatCeiling`): the class rules decide, for example, that an sRGB format has no storage use,
  a block-compressed format is sampled only and a depth format is a depth attachment. A real
  backend reports the intersection of that ceiling with its hardware, never more.
  `Format::Undefined` and a value that is not an enumerator are violations
  (`VU-rojo-format-0005`, `0001`).
- `DeviceDesc::validation` is `ErrorCategory::Unsupported` in a Release build. In Debug it creates
  a device that warns of leaks at destruction; the tracker proper arrives with the validation
  tracker work.
- A pipeline cache blob in `DeviceDesc` is accepted and ignored.

The Null backend reports the shape a Metal 4 or D3D12 device may report, but it reports a superset
of features that no real device reports together: every optional feature supported and every
format capability the class ceiling allows. It claims no capability the reference does not define.

## Threading

[ADR 0013](../decisions/0013-threading.md) applies unchanged: RojoRHI creates no threads. The
free-threaded `destroy` locks only the one pool it touches, with one mutex per pool, and reads the
frame word atomically under that lock; capability queries read data fixed at creation and touch no
pool. The frame class (`beginFrame`, `endFrame`) is
externally synchronised by the caller, as chapter 01 requires, and takes no lock of its own beyond
the pools it retires. The message sink has its own mutex, and the inside-callback flag is
thread-local.

## What remains

These arrive with the work that adds them; this page then describes them.

- Resources: buffers, heaps, textures, views, samplers, slot ranges, queues, fences and submission.
  Null submission will complete work inside `Queue::submit`, so fences signal there and waiting
  never blocks. Each handle type has its own record (`BufferRecord` and so on), which will grow
  the descriptor and state its resource needs.
- Recording: shader libraries, pipelines, command lists, passes, draws, copies, barriers and roots.
- Edges: swapchains, upscalers, sparse mapping, access declarations, counters, timings and capture.
- The validation tracker: the 28 identifiers only it checks, and the tracker hooks that
  `DeviceDesc::validation` selects.
- A Metal 4 v2 backend and T1 GPU conformance
  ([ADR 0009](../decisions/0009-conformance-and-valid-usage.md)).

`Tools/vu_pending.txt` lists each identifier whose death test is still to come, with the part that
will add it ([testing](../conventions/testing.md)).
