# ADR 0003: Thin, explicit, honest

**Status**: Accepted (2026-09-19)

**Restates**: Luminex ADR 0004, "RHI philosophy — thin, explicit, honest"

## Context

RojoRHI models the shared conceptual core of Metal 4, D3D12 and Vulkan. The classic failures of such
a layer are speculating ahead of real feature demand and retrofitting a binding model onto an
interface that assumed another one.

## Decision

The interface exposes what a consumer has demonstrably needed and grows feature by feature. A
capability is added when a consumer needs it, not because a backend offers it.

Public headers name no backend, windowing, ImGui or third-party type; only the standard library and
the library's own types appear. The backend is selected at build time behind a factory, and the two
backend-scoped public headers carry plain diagnostic values rather than backend handles. The
optional ImGui adapter is a separate target, not part of the interface.

Creation returns `Result<T>`, carrying either the value or an error with a stable category and a
diagnostic message. Misuse of the interface is not an error code: it is a fatal `ROJORHI_ASSERT`,
because a caller violating a precondition has a bug rather than a condition to recover from. Every
GPU object is created with a label, defaulted when the caller supplies none, so a capture names what
it shows. Binding is argument-table shaped — indexed buffer, texture and sampler slots — because the
target APIs converge there.

RojoRHI has no shader-toolchain duty: it loads a compiled shader library and reports a load failure,
while producing that library is the consumer's build system's business. Diagnostics leave through
one public message callback taking a severity and a message; when none is installed, messages go to
stderr. RojoRHI takes no logging dependency and knows nothing about the consumer's log sinks.

Two claims in the paragraph above go beyond the source ADR, which states neither. The public
message callback is new with RojoRHI: the component has no such callback today and logs through the
consumer's logging macros, so the callback is this ADR's own decision, made because standing alone
removes that dependency. The "no shader-toolchain duty" rule restates the consumer's later
practice — a compiled shader library is loaded and a load failure reported, while the build system
produces the library — rather than anything the source ADR says.

## Consequences

The interface stays smaller and more honest than a speculative complete abstraction, at the cost of
revisiting its shape as consumers and backends grow. A consumer wanting a capability has to ask for
it, which keeps every addition attached to a real need. Assertions on misuse mean an incorrect
caller aborts in development rather than producing a silently wrong frame.
