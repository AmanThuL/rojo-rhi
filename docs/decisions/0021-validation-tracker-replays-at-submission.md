# ADR 0021: The validation tracker records while recording and judges at submission

**Status**: Accepted (2026-10-14)

## Context

The Validation tier of the API reference ([chapter 14](../spec/14-diagnostics.md)) checks, on a
device created with `DeviceDesc::validation`, every rule whose bullet ends "Checked by the
validation tracker". Those rules read state that spans commands, lists and queues: the layout a
subresource was left in, the barrier between two uses, the order of two submissions.

- **Recording cannot judge them.** Command lists record in parallel, on any thread, and are
  submitted in any order, so while a list records, the state a rule needs does not yet exist. The
  layout a texture has at a use depends on the lists submitted before this one, which the recording
  call does not know.
- **Release recording is protected.** [ADR 0020](0020-recording-locks-scope-labels.md) allows no
  lock in Release recording except the claim of a frame-arena page, and the budget of
  [ADR 0014](0014-overhead-measurement.md) is measured on Release recording.
- **One violation can break several rules.** A texture left in the wrong layout for a copy on a
  Copy-queue list breaks the copy's own rule, the Copy queue's rule and the general layout rule at
  once. The process terminates at the first report, so which identifier it names decides which
  rule the author reads.

The Null backend builds these decisions in the layouts part of the tracker. An accepted ADR is
immutable, so they are recorded here.

## Decision

- **Record, then replay.** On a validating device, a recording entry appends events to its list's
  tracker log: each use the entry makes (resource, range, stage, access, the layouts that permit
  it, the identifier of the entry's own rule) and each barrier. `Queue::submit` replays the logs of
  its lists, in span order, against the device's tracker state, and a violation terminates there.
  Recording judges nothing that depends on another list.
- **One device mutex.** The tracker's state belongs to the device and is guarded by one mutex.
  `submit` holds it from its wait check through replay to publishing its signal values, so a
  submission on another queue never sees a value without the state it covers. Calls that change
  what the tracker follows, such as creating or destroying a texture or acquiring or presenting a
  swapchain image, take it for the change. The lock is a Debug-only lock on calls that are not
  recording, so it is outside the scope of ADR 0020.
- **One identifier per violation.** A violation is reported under one identifier, the most
  specific of those it breaks, and every identifier has a death test that reaches it alone. Replay
  judges in a fixed order. The first barrier on an acquired image reports `VU-rojo-present-0014`
  before `VU-rojo-sync-0013`. A use on a Copy-queue list reports `VU-rojo-sync-0006` first. Then
  comes the entry's own rule: `VU-rojo-cmd-0007`, `cmd-0010`, `cmd-0054`, `cmd-0067`, `cmd-0069`,
  `cmd-0070` or `VU-rojo-upscale-0016`. Then comes `VU-rojo-sync-0004`.
- **Hooks are inline and empty under `NDEBUG`.** Each recording hook is an inline function whose
  body is empty in a Release build, with its work in an out-of-line function that Debug calls, as
  `consumeAccesses` is. Release recording is unchanged and takes no tracker lock. A device without
  validation appends nothing in a Debug build either.

## Consequences

- The tracker's code lives in `Source/Core/Tracker/` and is backend-neutral. A backend calls two
  device hooks from `Queue::submit`, `lockTracker` and `replayLists`, and holds the lock the first
  returns; it adds no check of its own. [The edges](../architecture/null-backend-edges.md) describe
  the code as built.
- A rule that needs the state of a submission is judged late: the violation is reported when the
  list is submitted, not when the offending call is recorded, and its message names the list by
  label.
- The first part of the tracker holds one layout per texture subresource, which fixes the layout
  rules of chapters 07, 09, 11 and 12. Later work adds to the same log and state: hazards
  between uses on one queue, whether a barrier covers a use, and aliasing; counts of the
  references that unsubmitted lists hold on objects; and per-queue clocks, layouts per queue,
  handoff between queues, frames and sparse mapping. The log carries the stage and
  access of every use already, so that work adds replay rules and state, not recording changes.
- Barrier events keep no batch boundary yet; the hazard work adds one if the rules that chain
  the barriers of one batch need it.
- A rule that must look at the CPU's observations, such as a completed fence value, reads them at
  the same lock when that work lands.
- A change that reports from a recording hook what only replay can know, or lets a Release build
  reach the tracker, breaks this decision; a review of a tracker change checks for both.
