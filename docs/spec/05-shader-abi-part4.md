# 05. Shader ABI (continued)

**Status**: Accepted
**API version**: 0.1.0
**Entries**: none

This part defines the per-file floating-point policy of the xmake shader rule, which
[part 1](05-shader-abi.md#shader-build-abi-version-and-slang-upgrades) describes; parts
[2](05-shader-abi-part2.md) and [3](05-shader-abi-part3.md) define the module's functions. It
adds no entry to the interface.

## Precise floating point

A target of the shader rule may name entry files in `slang.precise_files`, each as `add_files`
names it without `.slang`; the default is none. A name that is not an entry file of the target
fails the build before any compile. Every entry of a named file compiles with the flags below, and
every other file with the compilers' defaults, which may contract a multiply and an add into one
fused operation.

| Output | Flags | Effect |
|---|---|---|
| SPIR-V | Slang `-fp-mode precise` | arithmetic decorated `NoContraction` |
| MSL, HLSL | Slang `-fp-mode precise` | none in the text; the metallib and DXIL compiles decide |
| metallib | `-fno-fast-math -ffp-contract=off` | no fast-math optimization and no contraction |
| DXIL | Slang `-fp-mode precise`, DXC `-Gis` | strict IEEE: arithmetic without fast-math flags |

The flags keep a multiply followed by an add as two rounded operations; they do not make results
identical across targets. No output's time says which flags built it, so the rule records each
file's policy beside its outputs in `<name>.precision`: the compiler's path and version, the `-I`
directories in order and the flags. A missing or different record removes the record and every
output of the file, which are rebuilt and the record written once all succeed, so a changed
policy, or a build that failed partway, never leaves outputs of two policies.

The `[precision]` conformance case runs `AbiPrecise` on `(1 + 2^-23) * (1 - 2^-23) - 1`, which is 0
when the product is rounded and -2^-46 when it is fused, and passes on Metal 4. D3D12 builds the
precise DXIL, which WARP 1.0.13 runs exactly and WARP 1.0.21, the pinned software adapter, runs
fused ([evidence](../research/2026-10-26-warp-precision-runtime.md)). On WARP the case therefore
accepts either result and skips on the fused one; the exact result waits for a hardware adapter
([the hardware run](../guides/d3d12-hardware-run.md)).
