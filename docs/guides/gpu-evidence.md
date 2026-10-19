# Recording GPU Evidence

**Status**: Accepted

Hosted runners have no Metal 4 device, so every GPU run that a change relies on happens on local
Apple Silicon, and its raw output is kept outside the published tree
([documentation](../conventions/documentation.md)). This guide says where that output goes and how
it is recorded, so a later reader can check that a log is the one a pull request cited.

## Where evidence lives

Evidence lives in a plain directory beside the checkout, `../rojo-rhi-evidence/`, one directory per
topic and one per date under it. Run from a git worktree, the tool still finds the directory beside
the main checkout, not beside the worktree:

```
rojo-rhi-evidence/
  v1-gpu-conformance/2026-10-18/   gpu.txt  checkpoint-a.txt  unit.txt
                                   environment.json  SHA256SUMS
  spike-3-multi-draw-indirect/2026-10-12/  ...
```

The directory is not a repository and is not published. A pull request that cites a run names the
topic and date and quotes the `SHA256SUMS` line of the log it relies on.

## Recording a run

`Tools/gpu_evidence.py` runs one command, saves its combined output as `<label>.txt` with the
command line at the top and the exit code at the bottom, and rewrites `environment.json` (machine,
OS build, Xcode, Metal and clang versions, the metal-cpp and repository commits, and whether the
checkout had changes) and `SHA256SUMS` over every file of the directory at any depth, named by
relative path. It exits with the command's code, so a failing run still leaves its log.

```
xmake f -m debug -y && xmake build RojoRHILegacyTests
python3 Tools/gpu_evidence.py --topic v1-gpu-conformance --label gpu \
    --cwd build/macosx/arm64/debug/rojorhi-legacy-test \
    -- env MTL_DEBUG_LAYER=1 ./RojoRHILegacyTests "[gpu]~[.]"
```

`--root` names another evidence root and `--date` another date directory. A rerun with the same
label replaces that log and refreshes the checksums of the whole directory. `Tools/bench.py` writes
its runs under topic `t2`, checksummed the same way
([the benchmark harness](../architecture/benchmarks.md)).

## Rules

- Correctness runs use a Debug build with `MTL_DEBUG_LAYER=1`. Timed runs follow the protocol of
  [ADR 0014](../decisions/0014-overhead-measurement.md): Release, validation off, and a
  pre-registration fixed before the first timed run.
- A log is never edited. A run that must be repeated is recorded again, under the same label to
  replace it or under a new one to keep both.
- A run from a checkout with changes says so in `environment.json`; the pull request then states
  what the changes were.

## Recorded runs

| Topic | Date | Result |
|---|---|---|
| `v1-gpu-conformance` | 2026-10-18 | the frozen v1 suite on an Apple M3 Max, macOS 27.0.1: 58 `[gpu]` cases, the 16 `[checkpoint-a]` cases and 143 CPU cases pass under the validation layer, the first run since v1 moved under `Legacy/` |
| `t1` | 2026-10-18 | the first Metal 4 runs of the v2 interface under the validation layer, one log per `contract`, `death` and `conformance` entry; the results are in the log summaries |
| `v1-gpu-conformance` (`gpu-shared-metalcpp`) | 2026-10-18 | the frozen v1 suite rerun after `RojoRHILegacy` linked the shared metal-cpp unit: 58 `[gpu]` cases, 7416 assertions, all passed |
| `t2-native-smoke` | 2026-10-18 | the three runs `native-check`, `native-build` and `native-gate` of the benchmark harness native arm on an Apple M3 Max, macOS 27.0.1: the 21 `[native]` cases pass under the validation layer, and the N-only correctness pass runs all 63 configurations clean in Release and in Debug |
| `t1` (`finish-*`) | 2026-10-19 | the complete Metal 4 backend on an Apple M3 Max, macOS 27, under the validation layer: `contract`, `death`, `abi`, `conformance` (163 cases), `capture`, `nopool`, Release `contract`, the default backend and `RojoRHIBench/metal4`; the results are in the log summaries |
| `spike-2-texture-indexing` | 2026-10-12 | [spike 2](../research/2026-10-12-spike-2-texture-indexing.md) |
| `spike-3-multi-draw-indirect` | 2026-10-12 | [spike 3](../research/2026-10-12-spike-3-multi-draw-indirect.md) |
