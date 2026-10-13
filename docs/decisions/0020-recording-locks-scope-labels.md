# ADR 0020: Recording takes no locks in Release, except to claim a frame-arena page

**Status**: Accepted (2026-10-14)

**Supersedes**: [ADR 0019](0019-recording-locks.md), whose decision it restates with a second cause
for the page claim. The clause "Recording takes no locks" of [ADR 0013](0013-threading.md) stays
superseded; the rest of ADR 0013 stands.

## Context

[ADR 0019](0019-recording-locks.md) permits one lock in Release recording: the claim of a new
frame-arena page, which it names for `CommandList::uploadFrameData` alone. Implementing the
diagnostics of chapter 14 added a second reason to claim a page.

- **Scope labels.** A debug scope's label is a view the caller owns only for the call, and
  `Device::scopeTimings` reports it after the frame retires. The label of a scope, including the
  scopes that render passes and upscales record, is therefore copied into the frame-arena page of
  the recording list, with the same shared cursor as `uploadFrameData`. A list whose page is full
  or missing claims a page from the source every list of the frame shares, which is the lock of
  ADR 0019, for a different call.
- **Frame-arena growth and Debug-tier checks.** Both reasons of ADR 0019 still hold: the claim
  needs a lock because lists of one frame record in parallel, and the lookups that feed Debug-tier
  checks read records under their pools' locks.

An accepted ADR is immutable, and ADR 0019 names only `uploadFrameData`, so a decision that
widens it is a new ADR.

## Decision

- In Release builds, recording takes no locks, except when a command list claims a new frame-arena
  page, for `uploadFrameData` or for the label of a debug scope (including the scopes of render
  passes and upscales). That claim may also create the page in the buffer pool and stamp it with
  the claiming frame.
- In Debug builds, a check of the Debug tier may take the lock of the pool whose record it reads.
  The lookups and loops that only feed such checks are compiled out of Release builds.
- A native backend reads the records it needs while recording without taking a lock. Its design,
  which arrives with that backend, says how.
- [ADR 0014](0014-overhead-measurement.md) measures recording in Release builds, where this rule
  holds.
- The lock a device takes to add a submitted list's scopes to its log belongs to `Queue::submit`,
  which is a queue-class call and not recording.

## Consequences

- Chapter 01's threading table and chapter 07's introduction state this rule with both causes. The
  architecture pages state which locks recording takes and in what order.
- A scope that begins with a label claims a page when the list's page is full or missing, so a
  list that records any labelled scope claims at least one page per frame, and a frame of many
  long labels grows the arena like frame data does; the growth is counted in `frameArenaGrowths`.
- Debug recording is slower and serializes briefly on shared pools; Debug builds are not what
  ADR 0014 times.
- A Release recording entry that reads a record, or claims a page for any other reason, breaks
  this rule; a review of a recording change checks for one.
- If a native backend cannot record without reading shared records under a lock, a later ADR
  revisits this one.
