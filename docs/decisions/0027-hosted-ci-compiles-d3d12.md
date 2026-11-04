# ADR 0027: Hosted CI compiles D3D12 and executes nothing on it

**Status**: Accepted (2026-11-04)

**Supersedes**: the words "on hosted Windows CI" of [ADR 0026](
0026-d3d12-implemented-before-hardware.md)'s first decision, "The D3D12 backend is implemented in
this repository against WARP on hosted Windows CI, under the debug layer and GPU-based
validation." The rest of ADR 0026 stands.

## Context

Under ADR 0026, two hosted `windows-latest` jobs ran the D3D12 backend on WARP: `d3d12` built it in
three configures and ran its `abi`, `contract`, `death`, `conformance` and `imgui` entries with the
floor probe, and `d3d12-bench` ran the benchmark harness and its correctness gate. They were the
slowest jobs by far, about 36 and 18 minutes, while every other job finishes within about 12, and
they gated every pull request. Their runs on WARP are recorded: [CI run
39641225757](https://github.com/AmanThuL/rojo-rhi/actions/runs/39641225757) for T1 ([the T1 run
on WARP](../architecture/d3d12-backend-edges.md#the-t1-run-on-warp)) and [run
39711745231](https://github.com/AmanThuL/rojo-rhi/actions/runs/39711745231) for the benchmark gate.
WARP shows the lowering functionally right; it shows no hardware behaviour and no timing, which
only a Windows GPU machine can.

## Decision

- Hosted CI executes nothing on D3D12. The `d3d12` and `d3d12-bench` jobs are removed.
- A `d3d12-build` job on `windows-latest` compiles the D3D12 backend, its tests, the floor probes
  and the benchmark harness in Debug (with the debug layer) and Release (warning-free under `/WX`),
  and checks statically: every conformance shader has its DXIL and `.d3d12lib`,
  `Tools/check_dxil.py` reads the DXIL, and `Tools/check_cpp_layout.py` checks the D3D12 sources.
  It runs no test, probe or other executable, and configures the default `hardware` adapter.
- D3D12 stays "implemented, verified on WARP only, not on hardware". The WARP evidence is the
  recorded runs above; acceptance is unchanged (ADR 0009, ADR 0026).
- D3D12 is next verified by [the D3D12 hardware run](../guides/d3d12-hardware-run.md) on a Windows
  GPU machine (issue #21). `--rojorhi_d3d12_adapter=warp` stays, so a local Windows build can still
  run on WARP.
- The other Windows jobs stay: the `interface` job's Windows leg (MSVC build of the Null backend,
  link-time-optimization probe and DXIL compile), `tracker-stress` and `header-cost`.

## Consequences

- A change that stops the D3D12 sources compiling, or breaks their DXIL or layout, still fails a
  pull request. A change that compiles but misbehaves on D3D12 is found only by a local d3d12 run or
  the hardware run.
- The default `hardware` configure compiles the adapter selection the WARP configures never
  compiled, so the hardware run starts from a build hosted CI has seen.
- The floor probe's artifact and the gate's evidence are no longer produced per pull request; the
  recorded results in [the D3D12 build page](../architecture/d3d12-build.md#the-floor-probe) and
  [the benchmark harness](../architecture/benchmarks.md) stand as of their runs.
