# ADR 0019: Recording takes no locks in Release, except to claim a frame-arena page

**Status**: Accepted (2026-10-13)

**Supersedes**: the clause "Recording takes no locks" of [ADR 0013](0013-threading.md); the rest of
ADR 0013 stands.

## Context

[ADR 0013](0013-threading.md) states that recording takes no locks. Building command recording on
the Null backend showed two places where a recording entry reaches state other lists share:

- **Frame-arena growth.** `CommandList::uploadFrameData` suballocates from the open frame's arena.
  A list bumps a cursor in a page it owns, which needs no lock, but a list whose page is full or
  missing takes a page from a source every list of the frame shares, and may create one in the
  device's buffer pool. Lists of one frame record in parallel, so that source needs a lock.
- **Debug-tier checks.** Most recording rules compare an argument with a resource's record: a
  copy's buffer size, an index buffer's usage, a pipeline's formats, an address's buffer. Those
  records live in the device's handle pools, which free-threaded creation and destruction change
  concurrently, so reading one takes the lock of its pool.

A Release build checks no rule, and the Null backend renders nothing, so its Release recording
needs no record. A native backend needs native objects (a buffer for a copy, a pipeline state for
a bind), and how it reaches them is part of that backend's design.

## Decision

- In Release builds, recording takes no locks, except when a command list claims a new frame-arena
  page for `uploadFrameData`. That claim may also create the page in the buffer pool and stamp it
  with the claiming frame.
- In Debug builds, a check of the Debug tier may take the lock of the pool whose record it reads.
  The lookups and loops that only feed such checks are compiled out of Release builds.
- A native backend reads the records it needs while recording without taking a lock. Its design,
  which arrives with that backend, says how.
- [ADR 0014](0014-overhead-measurement.md) measures recording in Release builds, where this rule
  holds.

## Consequences

- Chapter 01's threading table and chapter 07's introduction state this rule. The architecture
  pages state which locks recording takes and in what order.
- Debug recording is slower and serializes briefly on shared pools; Debug builds are not what
  ADR 0014 times.
- A Release recording entry that reads a record breaks this rule; a review of a recording change
  checks for one.
- If a native backend cannot record without reading shared records under a lock, a later ADR
  revisits this one.
