# The Validation Tracker

**Status**: Implemented

This page describes the validation tracker as built on the test-only Null backend: the checks that
follow a resource across commands, lists, submissions and queues. It is complete: every
valid-usage identifier of the API reference is checked and has a death test. It continues
[the edges of the Null backend](null-backend-edges.md). [ADR 0021](
../decisions/0021-validation-tracker-replays-at-submission.md) decides its shape and
[ADR 0020](../decisions/0020-recording-locks-scope-labels.md) keeps Release recording free of
locks; the [API reference](../spec/14-diagnostics.md) and chapters
[08](../spec/08-gpu-driven.md) and [09](../spec/09-synchronization-part2.md) are the
authority for every rule, and this page never restates their semantics. How the queues are ordered
against each other is on [its own page](validation-tracker-queues.md). The tracker lives in
`Source/Core/Tracker/`. Its objects are compiled into every build, but its paths run only in Debug,
on a device created with `DeviceDesc::validation`; the Release hooks are empty. The Null backend
adds two calls to `Queue::submit`, `lockTracker` and `replayLists`, and no check.

## The log

Each `CommandListCore` owns a `TrackerLog`, cleared by `beginList` with its capacity kept. It holds
a vector of events, each a `UseEvent` or one of the public barrier records unchanged (global,
buffer, texture). A `UseEvent` names a buffer byte range or a texture and its `SubresourceRange`,
the stage and access of the use, the set of layouts that permit it, the `UseRule` that a texture
use reports and, for an indirect read, the entry and arm that name its rule. `UseRule` is an
enumeration, not a string, because `ROJORHI_VU` needs an identifier literal at each site. A buffer
use carries no layouts and `UseRule::None`. Each event carries its command's ordinal, the list's
count of native recording calls, so replay gives each command one position on its queue. The log
also keeps the positions of the open pass's attachment events, which the draws complete, and the
sets of references the lifetime rules count.

**Hooks.** `logUse`, `logBarriers`, `logPass`, `logDraw` and `endPassLog` are inline, with bodies
empty under `NDEBUG` and an out-of-line `...Checked` function that Debug calls, the pattern of
`consumeAccesses`. They append nothing on a device without validation, and log after the entry's
own checks, where the resolved addresses and layout byte ranges exist:

- copies, `fillBuffer` and `clearTexture` log their endpoints at the stage `Copy` with
  `CopyRead` or `CopyWrite`;
- `drawIndexed` and `drawIndexedIndirect` log the index bytes read at `IndexInput`, and the four
  indirect commands log their arguments, and their count when it is not null, at `IndirectArgs`;
- `consumeAccesses` turns each declared access into a use at its declared stage and access;
- `upscale` logs each input in `ShaderRead` and the output in `ShaderWrite`, all as mip 0 of
  layer 0 (chapter 12);
- `beginRenderPass` logs one use per attachment: a colour attachment in `ColorAttachment` for
  writing, plus reading when `load` is `Load`; a resolve target at `Resolve`; a depth attachment
  for reading, plus writing when `load` or `store` discards or clears it, with the stencil
  operations counted only for a format that has stencil. `logDraw` runs after each draw and ORs the
  pipeline's effect into those events: `ColorAttachmentRead` for a blending target and
  `DepthStencilWrite` for a pipeline that writes depth or has a stencil test with an operation
  other than `Keep`. The pipeline record keeps `blendMask`, `depthWrite` and `stencilWrite`, so
  `logDraw` takes no lock;
- `barrier` logs every record of the batch, in the order globals, buffers, textures.

`Queue::present` is not a list entry and logs no event. Texture and buffer creation, swapchain
acquire, destruction and the release of a presented or destroyed swapchain's image call
`trackTexture`, `trackBuffer`, `forgetTexture` or `forgetBuffer`, which start and stop the tracker
following a resource. A frame-arena page is never followed.

**Replay and the mutex.** `Queue::submit` runs `submitLists`, which checks list state and folds
counters outside the lock, then `lockTracker`, `checkSubmit` (the wait check), `replayLists` and
`recordSignals`, and last, once the lock is released, `releaseSubmitted`, which removes the lists'
reference counts. The lock is held from the wait check to the publication of the signal values, and
is empty unless the build is Debug and the device validates. Under it a submission takes no pool
lock but the fences', and no path takes the tracker mutex while it holds a pool lock: the tracker
lock comes before the fence and pool locks, never after them. The counts live under their objects'
pool locks, so they go after it rather than add a lock order. Replay
reads every list before its counts go. `replayLists` visits the lists in span order and the events
of each list in log order, through one `std::visit` whose `static_assert` covers every event kind.

Creating a texture or buffer and acquiring a swapchain image take the same mutex for their single
call, after the pool call returns. Destroying a texture, buffer or heap takes it once, to stop
following the resource; its reference check and its pool release take only pool locks, never under
the tracker mutex. Destroying a swapchain takes it only to release an acquired image, and destroying
a pipeline, shader library, fence or upscaler does not take it. `presentImage` takes it to check the
image's layout, then again to release the image, since one hold would deadlock. A resource or
barrier destroyed between recording and submission is skipped at replay: the lifetime rules, not
these, judge it. `beginFrame`, `endFrame`, `Device::wait`, `completedValue`, destroying a fence and
`mapTiles` take it for their clock work, after their pool reads and with no pool lock held ([the
queues page](validation-tracker-queues.md)). Release recording takes no tracker lock.

## Layouts

`TextureState` holds the label, the mip and layer counts and one layout event per subresource,
indexed by mip and then layer, keyed in a map by the texture handle's bits. A texture starts in
`Undefined`, or in `ShaderRead` when created with initial data; an acquired swapchain image starts
in `Undefined` and is flagged until its first barrier. A Copy list finds a new texture in `General`
instead (09 `Layout`): `newOnCopy` accepts a subresource whose creation event left it `Undefined`
and that no use or barrier has reached since, of a texture neither placed, sparse nor an acquired
image, and the first use or barrier a Copy list makes of it sets that creation event to `General`
before its layout is read. The rule is per subresource, so a mip another queue never touched still
qualifies. A texture barrier that changes the layout or discards from `Undefined` sets every
subresource of its range to its `newLayout`, as an event on its queue; so does another resource's
use of a placed texture's memory, as an `Undefined` event.
The state persists across submissions and frames. Which layout a queue sees is the queues page's.

For each subresource of a use, replay reports the first rule that fails: `VU-rojo-sync-0017` for a
placed texture (see aliasing), `VU-rojo-sync-0006` (a use on a Copy list while the layout is not
`General`), then the entry's own rule (`VU-rojo-cmd-0007` for a colour or resolve attachment,
`0010`, `0054`, `0067`, `0069`, `0072`, or `VU-rojo-upscale-0016`) or, for a use no entry rule
names, `VU-rojo-sync-0004`, then the hazard rules. A texture barrier reports `VU-rojo-present-0014`
for the first barrier on an acquired image before `VU-rojo-sync-0013`. `VU-rojo-present-0018` is
judged at `presentImage`, before the image is released.

## Hazards and barrier coverage

Each texture subresource, and each byte segment of a buffer, keeps per queue type its last write
(or layout transition) and the reads since it, each with the destinations of the barriers replayed
over it since and whether each orders it (`Tracker/Hazards.*`). Uses of one command, the
attachment uses of one pass and the records of one `barrier` call share a position and are never
compared or chained with each other: two barriers of one call never order a pair together. The
coverage rule is `VU-rojo-sync-0024`, which retired `VU-rojo-sync-0023`:

- A source covers a use by its stages and the use's write accesses; reads need no source access.
  A barrier from `Undefined` needs no source access, and a source with no stage covers no use.
- A destination covers a use by its stages and, after a write, all the use's accesses; after a
  read, only its stages.
- A barrier whose source covers a use starts an execution chain over it. A later barrier, of
  another call, whose source stage shares a stage with the destination stage of a barrier of the
  chain extends it; its reach on the use is marked as ordering it, as a covering source's is. A
  later use is ordered when the destination of any barrier of the chain covers it.
- For a write after a write, a read between them also orders the pair when a chain orders the
  first write before the read and another the read before the second write. The chain from the
  first write alone needs a destination access that includes the second write's; the read needs
  only its stages, so the clause still orders pairs the chain does not.
- A layout transition is a write, ordered after the uses before it as a write is, with its own
  source as the last barrier of a chain. Its destination is the first reach of a chain over it, so
  the next use is covered by that destination or by a later barrier that extends the chain.

A use reports `VU-rojo-sync-0021` when no barrier lies between it and an earlier use it conflicts
with, and `VU-rojo-sync-0024` when barriers lie between and none orders the pair; a transition
reports `VU-rojo-sync-0024` when its own source neither covers a use before it nor extends a chain
over it. A draw or dispatch with a zero count makes no use, and a pipeline whose stencil write mask
is zero does not write stencil.

**Buffer segments.** A buffer's bytes are ordered disjoint segments that a use or buffer barrier
splits at its ends and that merge with their neighbours when alike and older than the current
command (`Tracker/ByteSegments.*`). The tracker follows a buffer from `createBuffer` to its
destruction, with its heap range when placed. `Tracker::lastWriteOrder` answers, for a buffer
range, how its last write is ordered before a given use.

## Indirect reads

The four indirect commands tag their argument and count reads with their entry and arm. Before the
hazards of such a read, replay asks `lastWriteOrder` about its bytes and reports the entry's
identifier (`VU-rojo-indirect-0005`, `0011`, `0017` or `0020`, one site per arm) when no barrier
since the last write on the queue has a destination stage and access that include `IndirectArgs`;
a barrier with that destination whose source misses the write is left to `VU-rojo-sync-0024`.
Bytes only the CPU wrote, initial data or frame data, have no write.

## Aliasing

Each heap holding a followed placed resource keeps ordered disjoint byte ranges
(`Tracker/OffsetMap.h`) that record per queue type the placed resource that last used them, that use
with the global barriers since (`Tracker/Aliasing.*`); `destroyHeap` drops it. A buffer's use maps
its bytes into the heap; a texture's use covers its whole heap range, its placement `heapSize`.

- A buffer's bytes last used on the queue by another resource report `VU-rojo-sync-0009` unless a
  global barrier since covers that use at its source and this one at its destination, a chain of
  global barriers does, or the buffer's clock covers that use.
- Every other placed texture over the bytes, and one placed later, gains on each subresource and
  that queue a mark per heap byte range: the latest foreign use of those bytes, which a newer one
  replaces, so marks stay as few as the ranges. Its next event there must be a texture barrier
  from `Undefined` whose source covers every mark, or `VU-rojo-sync-0017` reports. An event of the
  subresource on a queue clears its own marks there, and clears those on other queues that its
  clock covers. A covering discard clears its marks on every queue.
- Passing `sync-0009` or `sync-0017` resets the resource's own hazard state on the queue, since
  the aliasing chain orders its old uses and their contents are undefined.
- Precedence: `sync-0017` before `sync-0006` and the layout, entry and coverage rules; `sync-0009`
  before the indirect rules.
- A placed texture's layout change or discard is a write of its whole heap range on its queue, as
  on D3D12 it writes compression metadata. A later use of those bytes by another resource follows
  it only through a global barrier whose source covers its destination. Order between queues is
  `queue-0005`'s.

## References of unsubmitted lists

A list counts each object it references once, in the record's `ListReferences`, by kind: root,
address or other handle (`Tracker/References.*`). The hooks sit at `setRoot` (which stores the
bound root block), the draws and dispatches (which count that root as read, so a root moved before
any read is not counted), the indirect commands (arguments and count addresses), `drawIndexed`
(the index buffer, even at zero count), `bindPipeline`, `upscale`, and uses and barriers of
buffers and textures. A frame-arena page named by address, as frame data is, is not counted.
`Queue::submit` removes the counts after replay, outside the tracker lock (`releaseSubmitted`), and
`beginList` removes those of a list it recycles that was never submitted. `beginList` also clears
the swapchain images the list noted, so a recycled list names no image of an earlier frame.

`DeviceCore::destroy` checks the counts before releasing: `VU-rojo-abi-0003` for a root,
`VU-rojo-buffer-0019` for an address, then `VU-rojo-device-0007`; the core lifetime rules still
come first. `VU-rojo-present-0019` is judged at replay: an image the list noted and the tracker no
longer follows, since its present released it, reports before any event of the list. Heaps, shader
libraries and fences have no recording entry that names them, so their destruction runs the check
with zero counts; destroying a swapchain runs none, and an image it releases is judged by
`present-0019` at replay.

## Reporting precedence

For one event, replay reports in this order: `present-0019` for the list (before every event); for a
texture use, `sync-0017` on every subresource it names, then per subresource `sync-0006`, the
entry's rule or, for a use no entry rule names, `sync-0004`, then `sync-0021` or `sync-0024`; for a
texture barrier, `present-0014`, `sync-0017` on every subresource it names, then per subresource
`sync-0013`, then, for a transition, `sync-0024`; for a buffer use, `sync-0009`, then the indirect
rule, then `sync-0021` or `sync-0024`. The cross-queue rules slot in as the queues page says. A
global or buffer barrier reports nothing. At destruction the order is `abi-0003`, `buffer-0019`,
then `device-0007`. Each rule has one `ROJORHI_VU` site per arm and a death test that breaks only
that rule; tests pin `present-0014` before `sync-0013`, `sync-0006` before the entry's rule, and the
destruction order.

## Stress runs

Two hidden cases in `Tests/Core/TrackerStressTests.cpp` stress the tracker on the Null backend in
Debug. One runs 100,000 frames on three queues, each frame churning a transient texture, aliasing a
heap and joining the queues, and requires the fence clocks, heap segments and live textures to stay
bounded after a 1,000-frame warm-up. The other has every queue submit from its own thread, 16
batches of 1,000 rounds, while a fourth thread reads their fences. Both carry `[stress]` and `[.]`,
so no default entry runs them. After a Null Debug configure, `RojoRHITests "[stress]"` from the test
directory (or `xmake run RojoRHITests "[stress]"`) runs them; `--policies=build.sanitizer.thread` on
`xmake f` adds ThreadSanitizer on macOS.

On macOS (Apple M3 Max, plain Debug) the 100,000-frame case takes 5.2 s and the threaded case 1.1 s.
Earlier ad hoc runs were frame loops of up to 100,000 frames on a validating device and
ThreadSanitizer runs with every queue submitting from its own thread, all clean. ThreadSanitizer has
not been run on the committed cases.

On Windows the `tracker-stress` job (`windows-latest`, MSVC toolset 14.51.36231, AddressSanitizer)
passed on 2026-10-20: the 100,000-frame case took 92.914 s, the threaded case 15.425 s, and 21
assertions in 2 test cases held ([run
39440777819](https://github.com/AmanThuL/rojo-rhi/actions/runs/39440777819)). The job instruments
the project's targets only; Catch2 and the other packages are not instrumented, and the STL
container-overflow annotations are disabled on the targets to match them. The job checks that the
executable imports the AddressSanitizer runtime.

MSVC has no ThreadSanitizer, so the Windows run finds the memory errors a race causes when it
happens, not the race itself, and MSVC's AddressSanitizer has no leak detection. There is no other
difference.

## Known limits

The queues page lists those of cross-queue order. Besides them, a barrier that keeps the layout
is not compared with another queue, and the tracker never checks the memory of a frame-arena page.
