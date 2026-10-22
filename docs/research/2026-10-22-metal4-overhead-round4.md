# The Metal 4 backend's overhead against native code: round 4

**Status**: Accepted (2026-10-22)

This page is the text of the amendment of 2026-10-22 to [the pre-registration](
2026-10-18-metal4-overhead.md), made before any round-4 timed run; that note's Amendments section
points to it. As for rounds 2 and 3, the owner registers round 4 by amendment, so the note, with
[round 3's amendment](2026-10-21-metal4-overhead-round3.md), stays the authority on what round 4
measures and how it is judged, and this page states only what is new. Nothing in it changes a
budget, a workload, a `K`, an arm or a rule.

## Round 4

Round 4 measures the backend after the fix to round 3's one miss
([round 3's results](2026-10-21-metal4-overhead-round3-results.md)). It keeps everything round 3
registered, rules included: CPU time per operation judged by
[ADR 0025](../decisions/0025-cpu-budget-per-call-floor.md), a miss being an interval of R minus N
that excludes zero and a median delta above the larger of 5% of N's median and 5 ns times `c`, with
the note's 5%-only verdict reported beside it and deciding nothing; the allocations of C10 at
`maxCount` 1 (C10m1) reported only; N paying its own frame completion; R's `DeviceDesc` with
`scopeTimings` set in C8 and the composed frame only; and every budget, workload, `K`, arm, metric,
statistic, decision rule, procedure step and analysis as round 3 had them. The code changes listed
below leave each arm's native calls and allocations on Metal 4 the same; change 7 changes how R's
`setPipeline` selects a cached pipeline.

Round 3 missed one budget in one configuration: CPU time per operation at C3-k100, 13.96 ns
[13.33, 14.17] against a budget of 10 ns. C3 binds one of two pipelines, alternating, then draws.
An informal diagnosis after round 3, which is not judged, attributes the larger deltas of that
round to two effects of the shader change of pull request #100 (change 32 of round 3's list), whose
vertex entries read `[[base_vertex]]`:

- The driver's work per draw grew in both arms: N's C3 took about 20 ns more per operation than in
  the profiles before the change.
- R's `setPipeline` compared the bound handle with the newer of its two cached pipelines, then the
  older. When two pipelines alternate, that branch flips at every bind. While the driver's work
  between two binds was short the predictor followed the alternation; with the longer work it
  missed at every bind. RojoRHI's own instructions changed little (R minus N, instructions retired
  per operation, 109 before and 115 after), while R minus N in cycles grew from about 15 to about 35
  per operation.

Round 3's executable and fresh builds of its commit are byte-identical, so neither the binary nor
the method caused the difference. Change 7 below removes the branch.

## Procedure

Round 4 runs as round 3 did: from a clean checkout of the commit at the tip of `main` once this
amendment's pull request lands, whose hash the results page records, on AC power, with nothing else
built, tested or timed on the machine, and a check before each workload batch that waits until no
build, compiler, test or bench process and no foreign process above 30% of a core has run for 60 s,
however long that takes, while a watcher records desktop processes. It runs `Tools/bench.py` with
`build`, `correctness`, `timing` and `analyze` under the `t2` topic with the date argument
`<date>-round4`, so its raw results, logs, the load checks, `summary.json`, `analysis.txt` and a
SHA-256 manifest are kept in `rojo-rhi-evidence/t2/<date>-round4/`. The correctness gate runs at
that commit before anything is timed, and a workload that fails it is not timed.

After `analyze`, and after the driver's own output has been redirected into the evidence directory,
`bench.py seal` writes the manifest last. Rounds 2 and 3 hashed `load/driver-analyze.out` before
the redirect had written it, so their manifests carry the empty file's hash for it.

Round 4 reports on its own results page, `docs/research/<date>-metal4-overhead-round4-results.md`.
Rounds 1 to 3 stay recorded and unchanged; the note's Results section gains a Round 4 paragraph,
linking that page, when round 4 reports.

## Code changes since round 3

Round 3 ran at 378f219a. Since then `main` has changed `Source/`, `Include/`, `Backends/Metal4/`,
`Shaders/`, `Benchmarks/`, `Tools/bench.py` and `Tools/bench_analysis.py` through the eight commits
below, in order, each by what it changes for R or N on Metal 4. Commits that touch only
`Backends/D3D12/`, tests, CI, other tools or documentation change neither arm on Metal 4 and are not
listed.

1. bce0931d `tool: drive the overhead round on D3D12` (#107): `bench.py` builds, gates and times a
   `d3d12` harness, and the analysis takes the registered native-call column per backend, D3D12's
   beside Metal's. The Metal 4 and Null paths and Metal's column are unchanged.
2. 6c0556e5 `test: build the benchmark harness on D3D12` (#107): the harness builds on Windows. On
   macOS the shared files change only in how they write and read files (streams instead of
   `fopen`) and in that a failed output hash fails its arm; the run JSON names the backend, R loads
   the same `.metal4lib` libraries, and C10's point shader keeps its point size on Metal, the
   D3D12 target alone omitting it. Neither arm records or submits differently.
3. b8082323 `test: add the D3D12 native arms of the CPU operations and kernels` (#107): R's G1 and
   G2 outputs are created with `ShaderWrite`, which a D3D12 store needs and Metal 4 ignores, and
   G2's shader marks its texture index `nonUniform`, which leaves the Metal library unchanged.
   G1 and G2 are reported only.
4. 850070f2 `ci: run the D3D12 benchmark correctness gate on WARP` (#107): a `[d3d12]` count check,
   built on D3D12 only.
5. 98cfd24b `test: add the D3D12 native arm of the composed frame` (#120): D3D12's native arm only.
6. 6d8bd4d5 `d3d12: report submission times to the benchmark harness` (#120): R's `endFrame` in the
   harness calls a drain of the D3D12 timing seam, which does nothing on Metal 4, where commit
   feedback reports from Metal's completion thread as before; it lies outside every CPU span.
7. c2baaa52 `metal: select a resolved pipeline without a branch on its entry` (#124): a
   `setPipeline` that finds the handle in the list's last two pipelines tests both entries at once
   and picks the payload by index, so a hit takes one branch that always goes the same way. The
   pair, its order and what a miss reads are unchanged, and no counted call changes. C3 binds
   through this path at every repetition.
8. 229cc278 `tool: add bench.py seal to write the evidence manifest last` (#124): the `seal`
   command above.

## R's sequences and counts

Every per-operation and per-frame native-call count and every allocation count of R and N is round
3's: C1, C2 and C4 make 2 native calls per repetition, C3 4, C5 0, C6 2, C7 1, C8 13, C9 2, C10 3
and C10m1 1, and the composed frame 1,259 a frame against N's 1,256. The departures round 3's
amendment lists still stand.

## The informal re-check

A re-check of C3 with the round's own method, `bench.py timing --only C3` with 20 processes per
`K`, ran on `main` at 229cc278 (changes 1 to 8). It is informal and not judged. By ADR 0025, C3-k100
measured +8.54 ns [7.08, 9.69] against 10, within budget but without the margin the owner otherwise
asks for before registering (a median at most 0.8 of the budget); C3-k1000 +3.33 ns [3.09, 3.75],
C3-k10000 +3.11 ns [3.06, 3.30] and C3-k100000 +3.38 ns [3.17, 3.52]. By the 5% rule, which is
reported, C3-k100 (+6.41%) and C3-k100000 (+5.18%) would miss. The owner registers round 4 anyway:
a miss in it is reported as a miss, and no fifth round follows in this milestone.
