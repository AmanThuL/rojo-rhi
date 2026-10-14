# The Validation Tracker Across Queues

**Status**: Implemented

This page describes how the validation tracker orders the three queues of a device against each
other, on the test-only Null backend. It continues [the tracker](validation-tracker.md), which
describes the log, replay and the checks that follow a resource on one queue;
[ADR 0021](../decisions/0021-validation-tracker-replays-at-submission.md) decides the shape of
both. The API reference owns the rules: chapters [09](../spec/09-synchronization-part2.md),
[10](../spec/10-queues-and-frames.md), [02](../spec/02-device.md), [11](../spec/11-presentation.md)
and [13](../spec/13-sparse.md), and this page never restates their semantics. The code is
`Source/Core/Tracker/Clocks.*`, `Tiles.*` and the `Tracker` class, and, as everywhere in the
tracker, it runs only in Debug on a device created with `DeviceDesc::validation`.

## Clocks

A `Clock` holds one `uint64_t` per `QueueType`: the position on that queue through which work is
ordered. A position is the ordinal replay gives each command of a queue, so a use carries its queue
and its position there, and a clock covers it when its component of that queue reaches the
position. Zero covers nothing. `join` is the componentwise maximum and `meet` the minimum.

- **Positions.** Every command takes the next position on its queue when replay reaches it. A
  submission takes one more after its lists, even when it has none, so a frame can join it, and a
  `mapTiles` call takes one for its mapping change.
- **The running clock** of a queue is the join of what its submissions waited on, with its own
  component set to its latest position. A signal records it. The running clock is not what
  coverage reads, since the own component would order same-queue submissions that need barriers.
- **Waits.** A submission's wait joins the clock that reaching the value orders into the queue's
  waited clock. `FenceClocks` keeps per fence the clock of each value submitted for signal, stored
  as the componentwise minimum of its own clock and those of every later value, since any of them
  may reach the value first. A wait on a value at most the fence's initial value orders nothing,
  as does a value above every signalled one.
- **The ordering clock** of a submission on queue Q is the join of Q's waited clock, with this
  submission's waits, and the host clock. Everything below checks coverage against it.
- **The host clock** is what the CPU saw complete: `Device::wait` that returns true and
  `completedValue` join what reaching the value orders, and `beginFrame` that retires a frame slot
  joins the graphics running clock `endFrame` recorded for that frame.
- **Pruning.** Each signal prunes its fence against the host clock: every value whose stored
  clock the host covers is dropped but the last, which stays as a floor that a wait on any older
  value reaches. A fence signalled once per frame keeps at most `framesInFlight + 1` values. The
  floor's clock can exceed the true minimum for a wait on an older value. The host covers it, so
  hazard checks gain nothing, but it can raise a running clock; see device-0006 below.

## A fence wait is a full dependency

A pair whose earlier use the later use's ordering clock covers needs no barrier on any queue: the
wait makes the earlier work's writes visible and ordered, and the same holds for a CPU wait. This
replaced the per-resource stamps of the first design. It is rule semantics, in chapter 10, so no
ADR records it. Concretely, `checkUse`, `recordUse` and the transition check take the component of
the later use's own queue as `fenced`, and an earlier use at or below it is ordered. Aliasing uses
the same coverage (placed resources, below).

## VU-rojo-queue-0005

Each texture subresource and buffer byte segment keeps, per queue, its last write and the reads
since. A use on queue Q is compared with the other queues' last write, and with their reads when
it writes: a pair with at least one write that the ordering clock does not cover reports
`VU-rojo-queue-0005`. Two reads need nothing.

- **Transitions are writes.** A texture barrier that changes the layout, or discards from
  `Undefined`, is a write of its subresource on its queue, and is compared against the other
  queues' uses the same way, whatever its destination accesses.
- **Heap bytes.** Placed resources are compared by memory. For a placed buffer's use, a placed
  texture's use, and a placed texture's transition, replay compares the other queues' last use of
  the heap bytes by another resource, at least one a write, with the same clock. This is a second
  site of the rule; the unit site and the heap site are separate `ROJORHI_VU`s.
- **Aliasing across queues.** A fence that covers another resource's use of the bytes replaces the
  sync-0009 global barrier and the discard's source coverage. A placed texture still owes a
  discard after any other resource's use, because `VU-rojo-sync-0017` also resets the layout. Its
  marks on the queue clear at its own next event there, and those on other queues clear when the
  clock covers them.
- **Precedence.** For a texture use: `sync-0017`, the layout rules, `sparse-0013`, `sync-0021` or
  `sync-0024`, the unit `queue-0005` (per subresource), then the heap `queue-0005`. For a buffer
  use: `sync-0009`, the heap `queue-0005`, the indirect rules, `sparse-0013`, `sync-0021` or
  `sync-0024`, then the unit `queue-0005`. A barrier reports `queue-0005` only for a transition.

## Layouts per queue and the handoff

A layout is an event: the layout, the queue that set it (none at creation or acquire, which
precede every submission) and its position. The tracker keeps the latest event of each subresource
and answers `layoutOn(event, Q)`: the event's layout when it has no queue, is on Q, or is covered
by Q's ordering clock, otherwise nothing. When an event on another queue is not covered, the layout
rules are skipped and `queue-0005` reports at the same event. The two never report together. The
latest event alone suffices: an uncovered transition on another queue is that queue's last write,
or lies below a later one.

- **Handoff.** `VU-rojo-sync-0025`, which retired `VU-rojo-sync-0018`, reports at a use or a
  texture barrier when the layout handed from one queue to another is not one both share, and at a
  barrier whose `oldLayout` is neither `Undefined` nor the handed layout. `General` is shared by
  every pair, and between Graphics and Compute so are `ShaderRead`, `ShaderWrite`, `CopySrc` and
  `CopyDst`. A discard from `Undefined` on the acquiring queue is exempt from the first condition,
  so it needs no accepted layout; the same exemption lets the next event follow an `Undefined`
  event. The identifier was retired, not edited, because the discard exemption changed its meaning.
  An event with no queue is held by the queue of the latest use or barrier since, so a texture
  created with data in `ShaderRead` and sampled on Graphics is handed over to Copy in that layout.
- **Present.** `VU-rojo-present-0018` is judged at `presentImage` from the graphics queue's view:
  the latest event must set `Present` and be on Graphics, covered by its clock, or have no queue.
  An image moved to `Present` on Copy and presented with no fence reports.
- **Aliasing.** Another resource's use of a placed texture's bytes sets every subresource of the
  texture to an `Undefined` event on that queue, visible to the others. A later use with no
  discard sees `Undefined` and the entry's rule reports (for example `VU-rojo-cmd-0007`), not
  `sync-0017`, whose bullet names the same queue.

## Placed textures as heap writes

A placed texture's layout change or discard writes its whole heap range on its queue. It is a heap
use with the barrier's destination as scope and counts as a write, even when its destination has
no write access. A later use of the bytes by another resource follows it only through a global
barrier whose source covers that destination, or through a chain, and on another queue through
`queue-0005`. It marks the other textures over the bytes and sets their layout event to
`Undefined`, as any heap use does.

## Frames: VU-rojo-device-0006

`beginFrame` snapshots the positions of the queues, so a submission belongs to the frame open when
it was made, and one made between frames to none. `endFrame` reports `device-0006` when, for
Compute and then Copy, the last submission of the frame is not covered by the graphics running
clock. That clock gathers the waits of every graphics submission, so the frame's graphics
submissions count together, and each reaches other queues through the fences it waits on and the
orderings behind them. A CPU wait does not join: the check reads the graphics running clock, not
the host clock. A mapping change is not a submission.

Pruning caveat: a graphics wait on a pruned value reaches the floor's clock, which can exceed the
true minimum, so it can count toward the frame join for work the CPU already saw complete.

## Sparse mapping: VU-rojo-sparse-0013

A sparse texture follows, per subresource and queue, its latest whole-subresource event, plus the
box and position of each later partial one, for its uses and for its mapping changes. A sparse
buffer follows byte ranges (`Tracker/Tiles.*`). Positions only grow on a queue, so comparing the
latest overlapping event covers every earlier one. Events at or below the host clock's component
are dropped when the next one is noted, and a partial event a later box contains is dropped too,
which bounds the state by the uncovered work.

- **At `mapTiles`.** `core::mapTiles` takes the queue type. Debug builds each change from the
  mappings: a region over its mip and layers with its box, a mip tail over `[mipTailFirst, mips)`
  whole, a buffer's byte range. The hook takes the tracker lock after every pool read, checks each
  against the other queues' uses, covered by the queue's ordering clock, and notes the change at a
  new position. A use on the mapping's own queue is never compared: the mapping applies in queue
  order.
- **At replay.** A use, and a transition as a whole-subresource use, is compared with the other
  queues' mapping changes. It reports before `queue-0005`, and a use that races both reports
  `sparse-0013`.
- **Granularity.** The four copy texture uses carry their texel box, so a copy is judged per tile:
  a mapping region is whole tiles clipped at the edge, so a box overlaps it exactly when it
  touches one. Other uses and transitions are judged per subresource, and a tail change covers each
  tail mip of its layers. Buffers are judged per byte range.

## Locking

The tracker mutex is held by `Queue::submit` from the wait check through replay to publishing the
signal values, and is taken once for their single calls by creation and destruction of tracked
resources, `presentImage` (twice, since one hold would deadlock), `Device::wait`,
`completedValue`, `beginFrame`, `endFrame`, destroying a fence and `mapTiles`. Each of the last
six takes it after its pool reads, with no pool lock held; none re-locks. The order is the tracker
lock before the fence and pool locks, never the reverse: a submission reads each signalled fence's
initial value under the tracker lock, through the fence pool lock, and no path takes the tracker
lock while holding a pool lock. Release recording takes no tracker lock.

## Known limits

- **Older reads of aliased bytes.** Heap state keeps the last use per queue. An older read of the
  bytes on another queue, with no fence, is missed once a later read by another resource replaced
  it, though any write is caught.
- **First use of a mapped tile.** Chapter 13 says the first use of a newly mapped tile follows the
  aliasing rule, but no identifier states it, so no rule checks it.
- **Whole-subresource uses.** Remapping any tile of a subresource needs a fence against a use
  with no texel box, or a transition, of that subresource on another queue.
- **Barriers.** A barrier that keeps the layout is neither a layout event nor compared with
  another queue or a mapping change; the next use reports.
- **Read pairs.** Two reads of aliased bytes on two queues with no fence report nothing.
- **Regions of a subresource.** Texture hazards, within one queue (`VU-rojo-sync-0021` and
  `VU-rojo-sync-0024`) and across queues (`VU-rojo-queue-0005`), are judged per subresource, so
  two disjoint regions of one subresource count as the same memory. Only the tile-mapping check
  judges copies by their texel box.
