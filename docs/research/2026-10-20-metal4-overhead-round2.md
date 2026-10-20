# The Metal 4 backend's overhead against native code: round 2

**Status**: Accepted (2026-10-20)

This page is the text of the amendment of 2026-10-20 to [the pre-registration](
2026-10-18-metal4-overhead.md), made before any round-2 timed run; that note's Amendments section
incorporates it, and the note's line budget keeps it on a page of its own. The note says a later
round is a new note. Its owner registers round 2 by this amendment instead, so the note stays the
authority on what round 2 measures and how it is judged, and this page states only what is new.
Nothing in it changes a budget, a workload, a `K` or a rule.

## Round 2

Round 2 measures the backend as fixed after round 1's misses
([round 1's results](2026-10-18-metal4-overhead-results.md)). It keeps everything the note fixes, as
amended on 2026-10-18 and 2026-10-19: the budgets, the workloads, the `K` values (C8 up to 10,000,
C9 at 1, 10 and 100, C10 up to 1,000), the arms and R's `DeviceDesc`, the six metrics, the
statistics (Release builds with link-time optimization, validation off for timed runs, one fresh
process per workload and `K` running both arms in alternating order, 20 processes per
configuration, 50 warm-up and 200 measured frames, medians of process medians, paired deltas with
two-sided 95% percentile bootstrap intervals of 10,000 resamples and seed 20261018, the recorded
environment and the thermal discard), the decision rules, the procedure and the analysis.

Round 2 runs from a clean checkout of the commit that merges this amendment, on AC power, with
nothing else built, tested or timed on the machine. It runs `Tools/bench.py` with its four
commands, `build`, `correctness`, `timing` and `analyze`, under the `t2` topic with the date
argument `<date>-round2`, so its raw results, logs, `summary.json`, `analysis.txt` and SHA-256
manifest are kept in `rojo-rhi-evidence/t2/<date>-round2/`. The correctness gate runs at that
commit before anything is timed, and a workload that fails it is not timed.

Round 2 reports on its own results page,
`docs/research/2026-10-20-metal4-overhead-round2-results.md`. Round 1's results page and the note's
round-1 Results paragraph stay recorded and unchanged; the note's Results section gains a Round 2
paragraph, linking that page, when round 2 reports.

## Code changes since round 1

Round 1 ran at 4f2e0d6. Since then `main` has changed `Backends/`, `Source/`, `Benchmarks/` and
`Tools/bench.py` only through the fixes of pull request #70, rebased onto it as the 14 commits
below, in order, each by what it changes for R:

1. d108532 `metal: create textures without format views unless a view reinterprets`: a texture
   carries `PixelFormatView` only when it has a stencil plane and is `Sampled`, so the `RGBA8Unorm`
   targets of C1 to C3 and C8 and the frame's `albedo`, `material` and `ldr` no longer carry it. No
   call changes.
2. 3aaee75 `metal: add alias visibility only while memory aliases`: a barrier adds `ResourceAlias`
   only while a placed resource overlaps another's memory or a sparse resource lives. No workload
   makes either, so C6's global barrier lowers with visibility `Device`, as N's does.
3. 09cfab2 `metal: send no message for cached addresses while recording`: draws, counted draws,
   fills, clears and `uploadFrameData` read buffer and arena-page addresses and lengths cached at
   creation instead of sending `gpuAddress` and `length`; the arena bump and the open render
   encoder's test are inline. No counted call changes.
4. 3c0f317 `metal: signal queue progress only when a fence needs it`: a submission signals its
   queue's progress event only when it is a compute or copy submission that names a fence and holds
   no list or signals more than one fence. No workload makes one.
5. 6d0ff59 `metal: open per-command pools only under the validation layer`: the device reads its
   class once at creation, and only a bare driver device turns the per-command pools off, so a timed
   run opens none around draws, dispatches, copies, fills, encoder ends or command-buffer
   timestamps.
6. 6ac7822 `metal: time render passes with encoder timestamps`: a render pass writes its timestamp
   pair on its own encoders instead of on the command buffer; scopes outside a pass keep the
   command-buffer form.
7. 4042209 `metal: carry a list's owed barrier without a trailing encoder`: a pass's last encoder
   stays open after `endRenderPass` until the next encoder, a scope boundary or `end`, and a list's
   last encoder carries what the list owes at `end` through the graphics queue's carry fence and a
   producer barrier when it covers it; otherwise a trailing encoder still emits it.
8. bbd8069 `metal: make every label inside an autorelease pool`: each call that gives Metal a label
   runs in a pool, once per list, scope, pass or encoder, never per command.
9. 94af1b9 `metal: keep a barrier on an open compute encoder`: a barrier after a compute encoder ran
   only copies stays on that encoder instead of ending it.
10. 664c369 `core: skip fence bookkeeping only checks read in Release`: a Release submit reads no
    fence record and takes no fence-pool lock.
11. 1cd4e65 `metal: set native labels only when they change or tools need them`: a list sets its
    command buffer's label only when the text changes and reuses its scope and pass strings;
    encoders carry labels only on a device created with `validation` or `enableCapture`, or under
    the validation layer.
12. 34777b5 `metal: trim recording work per list and per draw`: the pass descriptor's attachments
    are kept, so `end` empties them with no getter and no pool; pools are `objc_autoreleasePoolPush`
    and `objc_autoreleasePoolPop` pairs; a draw of one instance sends the three-argument
    `drawPrimitives` and an indexed draw at base vertex 0 the form without it, one call each as
    before.
13. a0d61ba `metal: make counted-draw buffers resident through the device set`: a list's indirect
    command buffers are resident through the device's residency set, which a submission updates only
    when the list made or replaced one, so no list sends `useResidencySet`.
14. 08ac348 `test: release the commit options after their feedback`: the instrumentation change
    below.

## R's sequences and counts

Each item states R's sequence or count against the one the note registers and the one its
2026-10-19 amendment states. The counts are those of the untimed correctness gate at 08ac348.
Per-operation counts of C1 to C7 and C10 are unchanged: C1, C2 and C4 make 2, C3 4, C5 0, C6 2, C7
1 and C10 3.

- **C8.** R makes 13 native calls per repetition without the validation layer and 14 under it, where
  the encoder's `setLabel` returns, so a frame of `K` passes makes `13K - 1`, the first pass having
  no barrier. The registered sequence had 13 and the amendment 14. In order: `endEncoding` of the
  previous pass's encoder, at `beginRenderPass` (the last pass's at `end`); `pushDebugGroup` on the
  command buffer; at the draw, `renderCommandEncoder`, `setLabel` under the layer only,
  `setArgumentTable`, the consumer `barrierAfterQueueStages`, then the begin `writeTimestamp` on the
  encoder after the vertex stage; then `setRenderPipelineState`, `setFrontFacingWinding`,
  `setViewport`, `setScissorRect` and `drawPrimitives`; at `endRenderPass`, the end `writeTimestamp`
  on the encoder after the fragment stage and `popDebugGroup` on the command buffer, the encoder
  staying open. Both timestamps are written at `TimestampGranularityPrecise` into the same per-slot
  heaps, so the pass's two `writeTimestampIntoHeap` calls on the command buffer are gone. `barrier`
  makes no call. The limit stays N's 12 plus `c` of 7.
- **C9.** R makes 2 native calls per submit, the `commit` and the fence's `signalEvent`, as N does,
  against the amendment's 3 and the limit of 3.
- **C6.** The barrier after the fill stays on the fill's compute encoder: R emits
  `barrierAfterQueueStages` there at once and holds the encoder-level part for the first dispatch,
  so the encoder no longer splits. The encoder waits once on the carry fence before its first
  dispatch-stage command, and at `end` it carries the last barrier with `updateFence` of the carry
  fence, so no trailing encoder closes the list. The per-repetition calls are unchanged; at `K` 1
  the gate counted 17 a frame without the layer and 18 under it, against N's 12.
- **C10.** The stages the target barrier leaves pending at `end` are carried on the pass's render
  encoder by `barrierAfterStages`, so no trailing encoder closes the list, and the list sends no
  `useResidencySet`. The pass's begin timestamp is on the counted draws' kernel encoder.
- **Composed frame.** `gbuffer`'s geometry encoder carries the list's closing batch with
  `updateFence` of the graphics queue's carry fence after its vertex and fragment stages, and no
  trailing encoder closes `gbuffer`, so the `shadows` list's counted-draw kernels no longer wait for
  `geometry`'s fragment work; they wait on no fence. The `lighting` dispatch's encoder waits once on
  the carry fence. `lighting` and `ao` still close with trailing encoders, `lighting`'s dispatch
  part through the fence. `gbuffer` and `shadows` send no `useResidencySet`, and no submission
  signals its queue's progress event, four fewer signals a frame. The ten render passes write their
  timestamps on their encoders; the scopes around passes 1, 3, 7 and 14 keep the command-buffer
  pair, as N's do. The gate counted 1,259 native calls a frame for R without the layer and 1,280
  under it, against the amendment's 1,293 and N's 1,254.
- **Pools, labels and the frame event.** The per-command pools open only under the validation layer,
  so the timed runs open none; an encoder's open, each label call and the retire hooks keep theirs.
  Encoders carry labels only with `validation`, `enableCapture` or the validation layer, so the
  timed runs set none and the correctness runs in Debug set them. The per-frame `signalEvent` of the
  frame event stays: one native call and two Metal allocations a frame that N does not make, on
  every workload.

R departures that still stand, from the amendment of 2026-10-19:

- CPU time with no counted call: an autorelease pool around each encoder's open and each label
  call; the pass descriptor configured for each encoder; a command-buffer label set when its text
  changes. Every render pass of R is still a timed scope, so C1 to C3 and C10 pay a debug group
  and two encoder timestamps per frame that their N arms do not write.
- Per frame: the winding set per encoder; the composed frame's readback copy lowered to
  `Blit | Dispatch | Fragment`; the `lighting` and `ao` trailing encoders and the carry fence's
  waits and updates; C6's queue-level barrier after its fill; and the frame event.
- N's departures stand as the amendment states them, except C6's visibility: R's global barrier
  now lowers with `Device`, as N's does.

## Instrumentation and the arms

The commit-timing seam releases each commit's `MTL4::CommitOptions` in its own feedback handler,
after the report, on Metal's notification thread inside the handler's pool. R's submit span no
longer holds that release, as N's never held it, since N releases its options after its frame
wait. A submission without timing attached commits as before. The block handler and the rule that
a line on standard error stops the round stand as the 2026-10-19 amendment states them.

N's arm is unchanged: no file of `Benchmarks/Native/`, `Benchmarks/Common/` or `Benchmarks/Rojo/`
changed, and N's sequences and counts are those the note registers. The `[metal4]` counter check
gives each measured list a label of its own and pins a C8 pass's allocations below N's, which
writes its timestamps on the command buffer.
