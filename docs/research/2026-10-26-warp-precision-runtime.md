# WARP 1.0.21 fuses a precise fp32 multiply and add

**Status**: Accepted (2026-10-26, WARP only, not hardware)

The `[precision]` conformance case ([shader
ABI](../spec/05-shader-abi-part4.md#precise-floating-point)) skips its exactness check on WARP
1.0.21, the pin of `xmake/setup.lua`, on this evidence: the precise DXIL is correct, and WARP
1.0.21 changes its result. Nothing here is hardware evidence.

## The expression

`AbiPrecise.preciseMain` loads `x = 1 + 2^-23`, `y = 1 - 2^-23` and `z = -1` and stores
`x * y + z`. The exact product is `1 - 2^-46`, which rounds to 1 in fp32, so separate rounded
operations store 0. A product kept unrounded, as a fused multiply-add keeps it, stores -2^-46,
bits `0xA8800000`.

## The native comparison

CI run [40341580130](https://github.com/AmanThuL/rojo-rhi/actions/runs/40341580130), on one
Windows host, dispatched the case's signed DXIL natively, without RojoRHI, with the same
executable, Agility runtime and command sequence on two WARP versions. The runtime directories
differed only in `d3d10warp.dll`, and no compiler ran between the compared executions.

| Input or result | Identity |
| --- | --- |
| Signed DXIL | 3,420 bytes; SHA-256 `2c6db40278e930da0749c2832ac7276b96c75942a2af6320f16aefbe03da2b5b` |
| Agility Core | 1.619.6.0; SHA-256 `37fa14281a58cc834076971873006feb8a8d25cddc908d1a345bda1b149ffc7d` |
| WARP 1.0.21 | SHA-256 `e79c10550449365adf0a9393d97a0df69941e671ab6e952a78d92da066517ca3`; result `0xA8800000` |
| WARP 1.0.13 | SHA-256 `0621056518e047fd2fa9f75f03a49fed42d7158dee77c11c441cd34d32e93638`; result `0` |

## Findings

- The same DXIL is exact on WARP 1.0.13 and fused on WARP 1.0.21, so the compiled shader holds
  two rounded operations and WARP 1.0.21 does not keep them apart.
- The DXIL holds separate multiply and add instructions without fast-math flags, which the
  version-matched [DXIL reference](
  https://github.com/microsoft/DirectXShaderCompiler/blob/0d3ee6b55/docs/DXIL.rst#precise-qualifier)
  defines as precise arithmetic.
- The [WARP changelog](https://www.nuget.org/packages/Microsoft.Direct3D.WARP/1.0.21) records
  x86 FMA use from 1.0.14 when precise operations are not requested. No JIT machine code was
  captured, so the instruction or component at fault is not identified.
- Through RojoRHI on 1.0.21 the case fails only its expected-zero check, reading `0xA8800000`.

## Older WARP is no substitute

WARP 1.0.13 does not run the whole suite: in CI run
[40350441868](https://github.com/AmanThuL/rojo-rhi/actions/runs/40350441868) the hardware floor
probe of [ADR 0010](../decisions/0010-hardware-floor.md) ends with access violation `0xc0000005`
after recording an FSR dispatch. WARP 1.0.20, in CI run
[40386158730](https://github.com/AmanThuL/rojo-rhi/actions/runs/40386158730), returns
`0xA8800000` too, and four upscaler death cases end with `0xc00000fd`. The pin stays 1.0.21.

## Conclusion

The precise compile path is kept. On the WARP adapter the case accepts 0 or `0xA8800000`, skips
with its reason on the latter and fails on any other value. The exact result waits for a hardware
adapter ([the hardware run](../guides/d3d12-hardware-run.md)), under
[ADR 0026](../decisions/0026-d3d12-implemented-before-hardware.md).
