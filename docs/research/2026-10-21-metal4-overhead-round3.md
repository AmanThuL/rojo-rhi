# The Metal 4 backend's overhead against native code: round 3

**Status**: Accepted (2026-10-21)

This page is the text of the amendment of 2026-10-21 to [the pre-registration](
2026-10-18-metal4-overhead.md), made before any round-3 timed run; that note's Amendments section
points to it, and the note's line budget keeps it on a page of its own. As with
[round 2](2026-10-20-metal4-overhead-round2.md), the owner registers round 3 by amendment, so the
note stays the authority on what round 3 measures and how it is judged, and this page states only
what is new. Unlike round 2's, this amendment changes rules. Two of its changes, the CPU rule and
the allocation rule of C10 at `maxCount` 1, change how results are judged and were decided after
the results of rounds 1 and 2 and of the informal profiles that followed them had been seen. The
other two, N's frame completion and one field of R's `DeviceDesc`, change what is measured, and
were also decided after those results showed what each cost. Each is stated below with the
numbers that were known when it was decided.

## Round 3

Round 3 measures the backend as fixed after round 2's misses
([round 2's results](2026-10-20-metal4-overhead-round2-results.md)). It keeps everything the note
fixes, as amended on 2026-10-18 and 2026-10-19 and as round 2 kept it: the budgets other than the
CPU budget, the workloads, the `K` values (C8 up to 10,000, C9 at 1, 10 and 100, C10 up to 1,000),
the arms, the six metrics, the statistics (Release builds with link-time optimization, validation
off for timed runs, one fresh process per workload and `K` running both arms in alternating order,
20 processes per configuration, 50 warm-up and 200 measured frames, medians of process medians,
paired deltas with two-sided 95% percentile bootstrap intervals of 10,000 resamples and seed
20261018, the recorded environment and the thermal discard), the decision rules, the procedure and
the analysis, and the code changes listed below, which leave each arm's work the same, except the
four items below: the CPU rule, the allocation rule of C10 at `maxCount` 1, N's frame completion and
one field of R's `DeviceDesc`.

Round 3 runs from a clean checkout of the commit at the tip of `main` once this amendment's pull
request lands, whose hash the results page records, on AC power, with nothing else built, tested or
timed on the machine, and a check before each workload batch that waits until no build, compiler,
test or bench process and no foreign process above 30% of a core has run for 60 s, as round 2's
batches did; a batch waits for its check however long that takes, and a watcher records desktop
processes such as `avconferenced`, as in round 2. It runs `Tools/bench.py` with its four commands,
`build`, `correctness`, `timing` and `analyze`, under the `t2` topic with the date argument
`<date>-round3`, so its raw results, logs, the load checks, `summary.json`, `analysis.txt` and a
SHA-256 manifest are kept in `rojo-rhi-evidence/t2/<date>-round3/`. The correctness gate runs at
that commit before anything is timed, and a workload that fails it is not timed.

Round 3 reports on its own results page, `docs/research/<date>-metal4-overhead-round3-results.md`.
Rounds 1 and 2 stay recorded and unchanged, judged as they were; the note's Results section gains a
Round 3 paragraph, linking that page, when round 3 reports.

## The CPU rule: ADR 0025

Round 3 judges CPU time per operation by
[ADR 0025](../decisions/0025-cpu-budget-per-call-floor.md). A configuration at `K` of 100 or above
misses when the interval of R minus N excludes zero and the median delta exceeds the larger of 5% of
N's median and 5 ns times `c`, with `c` the RojoRHI calls per repetition that the note's workload
table states:

| Workload | C1 | C2 | C3 | C4 | C5 | C6 | C7 | C8 | C9 | C10 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `c` | 3 | 3 | 2 | 3 | 1 | 2 | 1 | 7 | 1 | 1 |

Everything else in the note's CPU rule stands: smaller `K` are reported, C10 is judged on CPU time
against arm E, and C10 at `maxCount` 1 is reported only. The 5%-only verdict of the note is
reported beside the rule in every judged configuration, the "5% rule" column of `analysis.txt`,
and decides nothing.

This rule was adopted after rounds 1 and 2 and two informal profiles had been seen, and its floor
was set with their numbers in view; ADR 0025 states those numbers and why it revises no earlier
verdict. Judged by it, round 1 would have missed 29 of 34 configurations instead of 33, and round 2
two (C3 and C6 at `K` 100) instead of 15. In the informal profile at c11fbdad, below, C3 misses the
5% rule at every `K`, by 6.0% to 7.1%, and passes ADR 0025's.

## The allocations of C10 at `maxCount` 1: reported only

C10 at `maxCount` 1 (C10m1) becomes a report-only row for allocations, as it already is for CPU
time, native calls and GPU time; it is judged on pipelines alone. This is a rule change made after
seeing results. The owner decided it on 2026-10-21, after the informal profile below and change 31
had left C10m1 one allocation a frame above N, which no lowering removes.

Why: the note registers C10m1 as "a report-only row" that reports what keeping the records' layout
costs Metal (issue #31, decided in the backend design), and its CPU rule leaves the row unjudged.
Its counts rule, which judges every workload's allocations, and the analysis, which applies that
rule per configuration since the amendment of 2026-10-19, still judged C10m1's. Round 2 judged them
a miss, R above N by 6 and 7 a frame.

What remains is one allocation a frame, at every `K`: the counted-draw kernels' compute encoder,
an object Metal allocates each time an encoder is made. N's C10m1 opens no compute encoder, since
its records are already native draws. R needs one, because its records keep RojoRHI's layout
(`drawId` first) and the shader reads `drawId` as the base instance, so a writer kernel rewrites
each record as a native draw ahead of the render encoder. No lowering removes it without changing
the record layout or the shader ABI. R over N, allocations a frame:

| Measurement | `K` 1 | `K` 10 | `K` 100 | `K` 1,000 |
|---|---:|---:|---:|---:|
| Round 2, at 0a0b53f | 18 / 12 | 18 / 12 | 19 / 12 | 19 / 13 |
| Informal profile, at c11fbdad | 15 / 14 | 15 / 14 | 16 / 14 | 16 / 15 |
| Diagnostic capture after change 31, one process each | 15 / 14 | 15 / 14 | 15 / 14 | 16 / 15 |

Change 31 removed the second allocation at `K` 100, which came from the writer dispatches' command
data. At `K` 1,000 both arms grow a command data buffer once, R its compute one and N its render
one. Every other workload's allocation rule is unchanged: R at most N, per configuration.

## N pays its own frame completion

A workload correction, which the owner decided on 2026-10-20 after round 2. N's arm, in every
workload that runs frames (C1 to C10, C10m1, G1, G2 and its arm F, the composed frame), signals a
shared frame event on the graphics queue after the frame's last commit and waits on the CPU, before
it resets a frame slot, for the value of the frame that last used the slot, frame `f − 3` with
three frames in flight, as R's `endFrame` and `beginFrame` do. It no longer paces its frames on the
commit feedback that the harness attaches to measure GPU time, which a renderer does not have.
ADR 0014 compares against careful native code, and a careful renderer pays for its frame
completion.

N makes two native calls a frame more, one queue call and one host call (the composed frame 1,256
instead of 1,254), and two Metal allocations more, from the signal; it waits at a slot's first use
too, for 0, so its frames count alike. No per-operation count changes, C9's count window included,
and neither arm's CPU span holds the frame's signal or wait. Commit feedback still gives each
frame's GPU span, and both arms' `endFrame` still waits for it, so both frame-event waits return at
once. Before the correction every workload's R allocated two blocks a frame more than N for the same
guarantee, which no correct backend could close: round 2's misses on C5, C7 and C9 were those two
alone.

## R's `DeviceDesc`

The note's fixed `DeviceDesc` gains `scopeTimings`, added by change 29 and set by the harness from
change 30: true in C8 and the composed frame, where N writes a debug group and two timestamps per
pass, and false in every other workload. This changes how R lowers those workloads, not what any
workload asks of either arm. It was decided after the profiles had shown what the timestamps cost.

C1 to C3, C10 and C10m1 no longer write their pass's two encoder timestamps or resolve them: four
native calls a frame fewer (the two `writeTimestamp`s and the frame wait's `resolveCounterRange` and
`invalidateCounterRange`) and, on C1 to C3, C10 and C10m1, three allocations a frame fewer (C10m1 18
to 15 at `K` 1). Their passes keep their debug group, so for round 3 the 2026-10-19 amendment's
departure "Every render pass of R is a timed scope, so C1 to C3 and C10 pay a debug group and two
timestamps per frame" reads: C1 to C3, C10 and C10m1 pay a debug group (push and pop) per pass that
their N arms do not write; the note's text stays as registered. C4 to C7, C9, G1 and G2 record no
pass or scope, and C8 and the composed frame are timed as before.

## Code changes since round 2

Round 2 ran at 0a0b53f. Since then `main` has changed `Source/`, `Include/`, `Backends/Metal4/`,
`Shaders/`, `Benchmarks/`, `Tools/bench.py` and `Tools/bench_analysis.py` through the 32 commits
below, in order, each by what it changes for R or N. Commits that touch only `Backends/D3D12/`,
tests, CI, other tools or documentation change neither arm on Metal 4 and are not listed.

1. 45a0190d `rhi: lower the portable Addr reach to 2^29 - 1 bytes` (#78): Metal 4 still reports
   `maxBufferLength` less one; a comment changes. Nothing changes for R.
2. 4e586965 `tool: flag statements that follow Catch2 FAIL in the policy check` (#84): two harness
   checks, `CpuChecks.cpp` and `FrameChecks.cpp`, use `FAIL_CHECK` and an explicit return instead
   of `FAIL`. No timed path changes; no expected timing effect.
3. 9ec98265 `rhi: clear textures only from CopyDst` (#81): a Debug-tier check; no workload calls
   `clearTexture`.
4. be432ce2 `shader: lower load and store to full-width raw accesses` (#80): HLSL only; the MSL both
   arms run is byte-identical.
5. 56894a5c `tool: check DXIL handle flags and raw access widths` (#80): a comment in the module and
   compile-only test shaders.
6. f46ce97d `rhi: check the first use of a newly mapped sparse tile` (#76): validation-tracker
   checks, Debug tier; no workload maps tiles.
7. 1692f7ca `metal: discard sparse textures with alias visibility` (#76): only barriers on sparse
   textures change; no workload has one.
8. 6ca29d85 `rhi: reserve leading view slots for a backend` (#86): Metal 4 reserves none.
9. ce78094c `core: keep the frame-data cursor as a host pointer` (#98): `uploadFrameData` bumps a
   host pointer; addresses and counts are unchanged. C1, C2, C4, C5 and the forward pass upload.
10. 5eac3301 `metal: use the carry fence only once counted draws have run` (#98): a graphics list
    uses the carry fence only after a counted draw's kernels have run in an earlier frame. C4, G1
    and G2 lose a `waitForFence` (one call and one allocation a frame); C6 replaces an `updateFence`
    and the next list's `waitForFence` with a `barrierAfterStages` on its closing encoder
    (`updateFence` and `waitForFence` out, `barrierAfterStages` in: one call net and two allocations
    fewer). C10 and the composed frame, which run counted draws, are unchanged after their first
    frame.
11. 7454137d `metal: record a repeated global barrier without a call` (#98): a batch of one global
    barrier that repeats the list's last reuses its lowering and is held inline; C6's calls are
    unchanged.
12. acd4b2a8 `metal: keep the common draw and pipeline bind inline` (#98): the one-instance draw
    and `setPipeline` inline into the caller; calls unchanged.
13. cfe10092 `metal: bind the list table to the mesh stages only for mesh pipelines` (#98): a pass
    binds the table to the vertex and fragment stages, the object and mesh stages only with its
    first mesh pipeline; no workload binds one, so counts are unchanged.
14. 9d5ff133 `metal: bind the untested depth-stencil state instead of null` (#98): C3's untested
    pipeline sends a depth-stencil state object, as N does, instead of null; the same calls, and one
    more state made at device creation.
15. a9e49bac `test: pin C8's pass allocations below the native arm's in every build` (#98): a
    harness check.
16. 51e7b3fe `metal: end a command buffer inside an autorelease pool` (#98): each list's `end`
    runs in one pool; no call.
17. 723dad6d `rhi: write fatal messages to stderr under a message callback` (#93): fatal paths only.
18. 765510c4 `rhi: keep a Copy-list texture barrier in General` (#106): a Debug-tier check; no
    workload records a Copy list.
19. cea5d8d4 `rhi: require Sampled for the DepthStencilRead layout` (#106): a Debug-tier check; no
    workload uses the `DepthStencilRead` layout.
20. 2bed3b90 `rhi: let a Copy list take a new texture in General` (#110): the validation tracker,
    Debug tier; no workload records a Copy list.
21. 7fbdbca7 `test: pace the native arm's frames with its own frame event` (#112): N's frame
    completion, above. The `[metal4]` pin of C4 to C7 requires R at most N.
22. 18ce2f76 `metal: resolve the list's last two pipelines without the handle table` (#112): a
    list keeps the payloads of its last two pipelines, so C3's switches read no handle table;
    calls unchanged.
23. d9e123bb `metal: set only the pass descriptor values that change` (#112): a pass's first
    encoder sets only the attachment values that differ from the descriptor's, and `end` nulls only
    the textures it holds; descriptor property sets are no native call.
24. d58bebed `core: reset a recycled command list in place` (#112): list acquisition clears its
    containers in place instead of building a new list.
25. 8691f659 `core: stamp a claimed arena page's frame in Debug builds only` (#112): a Release
    page claim takes the arena's lock alone.
26. 289becd9 `metal: release ended encoders when their frame slot is reused` (#112): a list keeps
    the encoders it ended until its frame slot is reused, then releases them in a pool; calls
    unchanged. Since this change C8 at `K` 1,000 and 10,000 allocates one block more in about two
    frames of three (4,021 and 40,030 against round 2's 4,020 and 40,029), still far below N; its
    source was not found.
27. 01483244 `metal: open a pass's render encoder in the pool around its descriptor` (#112): one
    pool push and pop fewer per pass.
28. 02df6f70 `tool: judge per-operation CPU time with the per-call floor` (#114): the analysis
    judges CPU by ADR 0025 and reports the 5% verdict beside it.
29. 6a8e1477 `rhi: make scope timing opt-in through DeviceDesc::scopeTimings` (#115): the
    interface; the harness's R device sets the flag, so nothing changes for R at this commit.
30. c11fbdad `test: time passes only where both arms do` (#115): R's `DeviceDesc`, above.
31. 7a5b0020 `metal: run consecutive counted-draw writer kernels as one dispatch` (#117): C10m1's
    writer kernels of consecutive draws run as one dispatch of a thread per draw, so C10m1 makes 1
    native call per draw instead of 3, N's 1; its per-frame calls are unchanged and it allocates one
    block a frame over N at every `K` instead of two at `K` 100. C10 and the composed frame, whose
    counted draws run encode kernels, are unchanged.
32. 426c0c40 `shader: exclude the base vertex from vertexIndex` (#100): `vertexIndex` excludes the
    draw's base vertex, so every vertex shader that reads it subtracts `[[base_vertex]]` on Metal,
    in both arms, and the shader ABI version becomes 2. The composed frame's geometry and shadow
    vertex entries add their mesh's first vertex, and each forward root names its mesh's first
    vertex, in both arms; each arm reads the same vertices as before.

The analysis change of this amendment, which reports C10m1's allocations, lands with it.

## R's sequences and counts

The counts are those of the untimed correctness gates of changes 30 and 31 and of the informal
profile, without the validation layer. Per-operation counts: C1, C2 and C4 make 2, C3 4, C5 0, C6
2, C7 1, C8 13 (14 under the validation layer), C9 2, C10 3, all as in round 2, and C10m1 1 (3 in
round 2). N's are unchanged.

- **Per frame.** C1 to C3, C10 and C10m1 make 4 calls fewer and three allocations fewer (R's
  `DeviceDesc`); C4, G1 and G2 one fewer, and C6 one fewer with one native barrier more (change 10).
  The composed frame makes 1,259 against N's 1,256, which was 1,254 in round 2. C8's sequence is
  round 2's.
- **Allocations**, from the informal profile: R equals N on C1 to C7, C9, C10, G1 and G2; R is
  below N on C8 and the composed frame (157 against 174); C10m1 is above, as stated.
- **Departures that still stand.** In C1 to C3, C10 and C10m1, a debug group per pass. Per frame,
  the winding set per encoder, the composed frame's readback copy lowered to
  `Blit | Dispatch | Fragment`, the `lighting` and `ao` trailing encoders and the carry fence's
  waits and updates, C6's queue-level barrier after its fill, and C10m1's compute encoder. CPU time
  with no counted call: a pool around each encoder's open, each label call, each pass's debug
  group and each list's `end`; a pass's first encoder setting the attachment values that changed;
  a command-buffer label set when its text changes. The frame event's signal and wait is no longer
  a departure, since N makes both. N's departures stand as the 2026-10-19 amendment and round 2
  state them.

## The informal profile

An informal profile of every workload on `main` at c11fbdad (changes 1 to 30), ten paired processes
per judged configuration, ran on 2026-10-21 under the measurement lock, with `avconferenced` using
33% to 49% of a core through most of it. It is not a registered round and is not judged. Under ADR
0025 every judged configuration was within budget, but two lacked the margin the owner asks for
before registering (a median at most 0.8 of the budget and an upper bound more than 1 ns under it):
C3-k100, +7.9 ns [upper bound 9.2] against 10, and C10-k100, +40.1 ns [upper bound 44.0] against
45.6, +4.4%, where the profile before had +3.1%. Allocations were as stated above; the composed
frame's GPU time was −1.79% [−2.06, −1.52].

A quiet re-check of those two and C10-k1000 followed on `main` at 7a5b0020 (changes 1 to 31), ten
paired processes each with a load check before each pass, also informal and not judged. C10-k100
measured +36.2 ns per operation [upper bound 40.0] against 45.4, +3.98% [3.40, 4.41], within budget
with margin; C10-k1000 −90.4 ns. C3-k100 measured +6.0 ns [upper bound 16.0] against 10, +5.06%
[2.62, 13.44]: its median is within the budget but its interval reaches above it, so it still lacks
margin. The owner registers round 3 accepting that C3-k100 lacks margin; if round 3 finds it over
budget, the results report it as a miss.

After change 32, the composed frame alone was measured on `main` at f7d79b27, ten paired processes
with a load check before each pass, informal and not judged: GPU time −2.12% [−3.10, −1.44],
allocations R 157 against N 174. The untimed correctness gate passed every workload at that commit.
