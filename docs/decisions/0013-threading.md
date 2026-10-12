# ADR 0013: RojoRHI creates no threads

**Status**: Accepted (2026-10-12)

## Context

An RHI can own worker threads for pipeline compilation, resource uploads and deferred destruction,
or it can leave all threading to its consumer. The scheduling facts that bear on the choice:

- Only the consumer knows scheduling priorities such as this frame, the next level and background
  cache fill.
- A pool inside the RHI would oversubscribe cores alongside the consumer's job system and the
  drivers' own compiler threads.
- D3D12 pipeline creation compiles on the calling thread, and Metal 4 compiles out of process, so
  RHI-owned threads add nothing for either backend.
- A library without threads has deterministic tests, which the Null backend and the contract suite
  rely on.
- NRI, Wicked Engine, SakuraEngine's CGPU and Granite follow the same rule, and engine-level
  pipeline precaching lives in the engine.

## Decision

RojoRHI creates no threads. It states a thread-safety contract for every entry point in the API
reference, and the consumer decides which threads call them.

- `Device` creation and destruction functions are free-threaded and may lock internally.
- A `CommandList` is recorded by one thread at a time; many lists record in parallel.
- Submission is serialized per `Queue`.
- Recording takes no locks.

Destruction is deferred rather than blocking: `Device::destroy(handle)` releases the native object
once the last frame that could have used it has retired, so a thread that destroys a resource
never waits and never tracks GPU progress.

A consumer-provided job callback, `TaskSink`, is reserved. It is the dispatcher pattern Jolt and
PhysX use: the consumer hands the library a function that runs a job on the consumer's scheduler.
It is reserved for a future backend that needs internal parallelism. The interface defines no
`TaskSink` type and no entry point accepts one; version 0.1.0 has no such backend.

## Consequences

- The consumer is responsible for serializing submission on each queue and for not recording one
  command list from two threads at once. The valid-usage rules state these as rules, and the
  validation tier ([0009](0009-conformance-and-valid-usage.md)) reports a violation it can observe.
- Asynchronous pipeline creation is the consumer's to schedule: it calls creation from its own
  worker threads, which the free-threaded creation functions allow.
- Adding `TaskSink` later is an additive interface change and does not alter the contracts above.
  Adding a library-owned thread is not additive and needs a superseding ADR.
