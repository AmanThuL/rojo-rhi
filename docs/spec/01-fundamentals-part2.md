# 01. Fundamentals, part 2

**Status**: Accepted
**API version**: 0.1.0

This part continues [fundamentals](01-fundamentals.md) with labels, the message callback and the
version macros. Notation and cost classes follow [conventions](00-conventions.md).

## Entries

### `Label`
```cpp
std::string_view label;   // field of every descriptor, or parameter of every creation call, that names an object
```
Every GPU object RojoRHI creates carries a label, applied to the native object and used in
captures, debug groups, timings and messages. A caller-supplied label is copied at creation and
used verbatim, keeping the caller's own prefix; RojoRHI never rewrites, prefixes or truncates it.

An empty label is replaced by a default that follows the naming convention: lowercase,
dot-separated, the `rojorhi.` prefix, the object kind, then its state where one distinguishes it,
then `unnamed`. Examples: `rojorhi.buffer.unnamed`, `rojorhi.texture.placed.unnamed`,
`rojorhi.pipeline.unnamed`, `rojorhi.pass.unnamed`. The kinds are `device`, `buffer`, `texture`,
`view`, `sampler`, `pipeline`, `shaderlibrary`, `heap`, `fence`, `upscaler`, `swapchain`,
`commandlist`, `pass` and `scope`; the state `placed` marks a resource created in a `Heap`. Objects
RojoRHI creates for itself, such as frame arenas, are labelled `rojorhi.<subsystem>.<name>`.

#### Valid usage
- `VU-rojo-core-0006` A label is valid UTF-8.
- `VU-rojo-core-0007` A label contains no NUL character.

#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | `setLabel` on the object or, for pipelines and samplers, on its descriptor before creation | `ID3D12Object::SetName` | `vkSetDebugUtilsObjectNameEXT` |
| Cost | native | native: one UTF-16 conversion | native |

### `MessageCallback`
```cpp
enum class MessageSeverity : uint8_t {
    Info,      // routine progress or configuration detail
    Warning,   // a recoverable condition worth reporting
    Error,     // a failure, including the text of a violated valid-usage rule
};
using MessageCallback = void (*)(MessageSeverity severity, std::string_view message, void* user);
```
Every diagnostic RojoRHI emits leaves through one process-wide callback: backend messages,
validation-tracker reports, the text of a violated rule before the process terminates, and the
native API's own messages where the backend can route them. `message` is valid only for the
duration of the call; `user` is the pointer given to `setMessageCallback`. A message reporting a
valid-usage violation begins with its identifier.

A callback must return normally: an exception or a `longjmp` leaving it unwinds through RojoRHI
frames and is undefined behaviour. It may run on any thread, so it must be thread-safe.

#### Valid usage
- `VU-rojo-core-0008` A message callback calls no RojoRHI entry point, including
  `setMessageCallback`.

#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Native source | `MTL4::CommitFeedback` errors and backend messages; Metal API validation (`MTL_DEBUG_LAYER`) prints to the process log and is not routed (unverified that it cannot be) | `ID3D12InfoQueue1::RegisterMessageCallback` | `vkCreateDebugUtilsMessengerEXT` |
| Cost | native | native | native |

### `setMessageCallback`
```cpp
void setMessageCallback(MessageCallback callback, void* user);
```
Installs the process-wide message sink. `nullptr` restores the default, which writes each message
to stderr. The new callback receives every message emitted after the call returns. Free-threaded.

#### Valid usage
- None.

#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | none: RojoRHI state | none: RojoRHI state; the backend's `RegisterMessageCallback` forwards to it | none: RojoRHI state; the messenger forwards to it |
| Cost | native | native | native |

### `ROJORHI_VERSION_*`
```cpp
// <rojoRHI/Version.h>
#define ROJORHI_VERSION_MAJOR 0
#define ROJORHI_VERSION_MINOR 1
#define ROJORHI_VERSION_PATCH 0
```
The version of the interface the headers declare, following Semantic Versioning 2.0 for source
compatibility; RojoRHI is built from source, so no binary compatibility is promised. Version 2 of
the interface ships as `0.y.z` until the Metal 4 backend and the planned D3D12 backend both pass
conformance and the driving consumer has moved to it; a breaking change bumps the minor version.
`1.0.0` follows, after which a break needs a major version and a superseding decision record.
Releases are to be tagged `vX.Y.Z`, so a consumer can pin a tag rather than a commit, and a
`CHANGELOG.md` arriving with the first v2 code records breaking changes per minor version.

The shader ABI is versioned separately by `ROJORHI_SHADER_ABI_VERSION`, specified in
[shader ABI](05-shader-abi.md).

#### Valid usage
- None.

#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Native counterpart | none: compile-time constants | none | none |
| Cost | native: no run-time work | native: no run-time work | native: no run-time work |
