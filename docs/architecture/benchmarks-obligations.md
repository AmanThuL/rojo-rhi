# Backend Obligations of the Native Arm

**Status**: Accepted

This page belongs to [the benchmark harness](benchmarks.md). It lists what the N arm encodes of the
backends' lowering.

N is the native minimum. Where R's lowering may legitimately do more, the difference is measured
overhead and not a defect of the harness. In particular:

- A barrier still pending at a list's end (the closing batches of the gbuffer and ao lists) lowers
  to no native barrier in N.
- N closes the compute encoder at a scope's end, while the design's M7 keeps it open until a render
  pass, an upscale or `end`; R may therefore emit an encoder barrier at the ao closing batch that N
  does not. What a list of R still owes at its end is R's own native calls: its last encoder carries
  it (`updateFence` of the graphics queue's carry fence, `barrierAfterStages`, or both) when that
  encoder waited for every source, as gbuffer's geometry encoder, C6's dispatch encoder and C10's
  pass encoder do, and otherwise a trailing encoder emits it, as for the lighting and ao lists; each
  compute encoder of a graphics list that dispatches waits on the fence once, as lighting's does.
  A graphics list uses the fence only once a counted draw's kernels have run in an earlier frame:
  the composed frame's lists do from its second frame on, while C4, C6, G1 and G2, which draw no
  counted draw, never do, so C6's dispatch encoder carries its last barrier with
  `barrierAfterStages` alone and no encoder of C4, C6, G1 or G2 waits on the fence.
  Both are measured overhead; in the integrated gate R makes 1280 native calls per composed frame
  with the validation layer on (Debug) and 1259 without it (Release) to N's 1256; the figure depends
  on the layer because encoder labels are native calls. They are per frame, so they change no
  per-operation count. A render pass's last encoder ends at the next encoder, a scope boundary or
  `end` rather than at `endRenderPass`, after the pass's pop, and its end timestamp is written on
  that encoder: C8's sequence keeps its count and changes its order.
- `Copy` on a Graphics list lowers to `Blit | Dispatch | Fragment` on Metal 4, so that a copy waits
  and signals for every stage that may touch its buffers; N's copy is a blit alone. The wider stage
  set is measured overhead.
- C6's barrier after its fill stays on the fill's compute encoder, which has run no dispatch: R
  emits `barrierAfterQueueStages` there at once, for earlier lists' copies, and the encoder-level
  barrier before the first dispatch, so its frame opens one compute encoder, as N's does. N's
  barrier after its fill is encoder-level only; the queue-level one is measured overhead. The
  per-repetition count is unchanged.
- A list's indirect command buffers are resident through the device's residency set, which the
  submission that follows their making updates, so a list with counted draws sends its command
  buffer no `useResidencySet`: one native call and two Metal allocations fewer per such list, C10's
  frame one and the composed frame two (`gbuffer`, `shadows`).
- Allocator resets count in `nativeHostCalls`, outside every timed window and per-operation count,
  though inside the composed frame's per-frame count; they are not native-call targets.
- C9's count window spans the same `K` submit calls as its timed span, but in R it sums only the
  deltas of `nativeSubmitCalls` and `nativeHostCalls`: the lists' record calls and barriers, which
  fold into the counters at submission, are left out. Any submit-time encoding the backend does
  must therefore count as submit calls.
- `CommitTiming.h` promises exactly one commit per `Queue::submit` and no backend-internal commit on
  a queue while timing is attached to it: `RojoGpuTiming` fails a frame on a stray or
  non-contiguous serial. The seam's feedback must also coexist with a commit feedback handler the
  backend sets for its own waits.

Where the Metal 4 backend departs from N today, each difference is measured overhead and not a
defect:

- An encoder's open runs in an autorelease pool, as do the label a list sets when its text changes
  and each pass's debug group. Without the validation layer, encoders carry no label: one native
  call and one Metal allocation less per encoder. Every native draw, dispatch, copy and fill, the
  end of a render encoder and each timestamp written on a command buffer run in a pool only when the
  Metal validation layer wraps the device, so a run without the layer pays none per command. A pool
  is not a message to a command buffer, encoder, argument table or scaler, so it is no native call
  and changes no count above; it shows only in CPU time.
- R's device sets `DeviceDesc::scopeTimings` (`RunOptions::scopeTimings`) only for C8 and the
  composed frame, whose N arms write a debug group and two timestamps per pass, as the note
  registers; in every other workload R's passes keep their debug group, which N does not write,
  write no timestamp and resolve none. R writes a timed pass's pair on its encoders, the begin first
  on its first encoder and the end on its last render encoder before `endEncoding`, while N writes
  it on the command buffer around the pass's encoders, so R's C8 and composed-frame sequences have
  the begin after the encoder's open and the end before its `endEncoding`; their counts hold.
- A pipeline that culls, tests stencil or has a fragment entry makes the backend set the winding
  once per encoder it opens, one native call more than N's sequence. An encoder carries a label only
  under the validation layer (or on a device created with `validation` or `enableCapture`), one call
  the registered 13 includes, so C8 makes 13 native calls per repetition in a timed run and 14 under
  the layer, against N's 12 and the budget of 12 plus `c` of 7.
- The correctness gate of 2026-10-19 (Release and Debug, validation on in Debug) measured, as
  calls per repetition, R and N, and the budget N plus `c`: C1 2 and 2 (5), C2 2 and 2 (5), C3 4
  and 4 (6), C4 2 and 2 (5), C5 0 and 0 (1), C6 2 and 2 (4), C7 1 and 1 (2), C8 14 and 12 (19),
  C9 3 and 2 (3), C10 3 and 3 (4). C10 at `maxCount` 1 (R 3, N 1; R 1 since its writer kernels run
  as one dispatch a pass) is outside the pre-registered budget table, and reported only.
- C9 made three native calls per submit in that gate, the third the queue's own progress
  `signalEvent`. A submission with lists and at most one signal no longer signals progress, since
  the frame's retirement covers it, so R makes two per submit, the commit and the fence's
  `signalEvent`, as N does, against a budget of 3. The composed frame loses the progress signals of
  its `cull`, `gbuffer`, `ao` and `lighting` submissions, four native calls per frame. `endFrame`'s
  frame-event signal remains, and N makes the same signal and wait. A change to `Queue::submit`
  reruns C9 first.
