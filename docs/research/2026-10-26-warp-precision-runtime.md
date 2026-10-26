# WARP changes a precise fp32 result

**Status**: Accepted (2026-10-26)

A controlled native D3D12 comparison produces `0xa8800000` on WARP 1.0.21 and exact zero on
WARP 1.0.13 from the same signed DXIL, executable, Agility runtime and command sequence. That
native result does not establish compatibility with the complete runtime. The required floor
probe on 1.0.13 crashes during its FSR dispatch path before producing a complete result.

Setup selects 1.0.20, the stable release immediately before 1.0.21 that includes documented
Agility 1.619 support, for a bounded compatibility and regression comparison. Its version and
documented features do not predict its precision or FSR results; the full normal gates remain
the acceptance authority. The original precision shader, inputs and eight assertions remain
unchanged. This is a software runtime choice; D3D12 remains implemented, verified on WARP only,
not on hardware.

## The comparison

The dynamic expression multiplies `1 + 2^-23` by `1 - 2^-23`, then adds `-1`. Separate fp32
operations produce zero; the nonzero result is compatible with retaining an unrounded
intermediate. No JIT machine instruction was captured, so the result does not identify a
particular instruction or establish a general vendor defect.

CI run [40341580130](https://github.com/AmanThuL/rojo-rhi/actions/runs/40341580130), in the
temporary `precision-native` job, performed the comparison on one Windows host. Both native
pipeline-stream creation and classic compute creation failed on 1.0.21. The stream variant
passed on 1.0.13. Each process recorded the private runtime modules after device creation and
dispatch, method and vtable ownership, resource and descriptor descriptions, root and input
bytes, raw output, and four completed fences. The native runtime directories differed only in
`d3d10warp.dll`; no compiler was invoked between the compared executions.

| Input or result | Identity |
| --- | --- |
| Original signed DXIL | 3,420 bytes; SHA-256 `2c6db40278e930da0749c2832ac7276b96c75942a2af6320f16aefbe03da2b5b` |
| Agility Core | 1.619.6.0; SHA-256 `37fa14281a58cc834076971873006feb8a8d25cddc908d1a345bda1b149ffc7d` |
| WARP 1.0.21 | SHA-256 `e79c10550449365adf0a9393d97a0df69941e671ab6e952a78d92da066517ca3`; result `0xa8800000` |
| WARP 1.0.13 | SHA-256 `0621056518e047fd2fa9f75f03a49fed42d7158dee77c11c441cd34d32e93638`; result `0` |

The 1.0.13 [NuGet package](https://www.nuget.org/packages/Microsoft.Direct3D.WARP/1.0.13)
is 12,992,499 bytes, with SHA-256
`63231c48b0573ba4c078f69cd10a4059a0fee3427107b8219e5e80ab75bd304b`. Those are the frozen
native control's package bytes, not the package selected for complete validation.

The original RojoRHI control on 1.0.21 failed one of eight assertions. A separate external
module observer detected a changed census and observation errors. Its experiment remained
noninterpretable and did not execute the RojoRHI 1.0.13 arm. The valid native contrast stands
separately; it does not turn that observer failure into success or substitute for the normal
RojoRHI tests.

## Complete-runtime boundary

The ordinary job in [CI run 40350441868](
https://github.com/AmanThuL/rojo-rhi/actions/runs/40350441868) attempts 1.0.13 with the unchanged
Agility 1.619.6 and FidelityFX 2.3 configuration. Both factory and informational exports probes
terminate with access violation `0xc0000005`; both output JSON files are empty. Their last
checkpoint follows accepted FSR dispatch recording and precedes a sequence containing command
list close, execution, signal, synchronous fence wait and device-removal query. No captured
fault address or stack identifies the failing call or module. Earlier checkpoint names do not
replace the missing complete floor verdict. The required factory gate correctly rejects this
attempt before the normal RojoRHI precision test runs.

The [publisher's changelog](https://www.nuget.org/packages/Microsoft.Direct3D.WARP/1.0.21)
places Agility 1.619 and shader model 6.9 support in 1.0.18 and records later code-generation
correctness work in 1.0.20. Version 1.0.20 is therefore one nearer regression boundary that
retains that documented support, rather than another pre-1.0.14 control. This does not identify
a precise-fp32 fix or certify its full compatibility. A further failed result requires a
specific investigation, not an automatic version sweep or a relaxed precision or FSR gate.

The selected 1.0.20 [NuGet package](
https://www.nuget.org/packages/Microsoft.Direct3D.WARP/1.0.20) is 15,319,749 bytes, with SHA-256
`e5fe5de661ce98b58ef9cfb736e73c0a7a2623d3bbf5f14839b2d55566d87e40`. Its amd64 DLL is
8,500,576 bytes, version 1.0.20.0, with SHA-256
`2a08692cba4c130593329255627fb915d666c90cda53d284594bc8438fd4f49d`. The official package
SHA-512 was checked before archive inspection; every member's CRC, size, hash and path/type
safety was checked. Setup verifies the selected package's SHA-256 before extraction.

The source and copied DLL hashes are checked before and after each normal Debug, Release and
layer-only test configuration. The existing live-device runtime case also checks the loaded
WARP path when the build selects the software adapter. These are on-disk identities and a live
module path in the contract process, not a census of each separate conformance process, an
in-memory code hash or file-identity proof.

## Precision and acceptance boundaries

The version-matched [DXIL precision contract](
https://github.com/microsoft/DirectXShaderCompiler/blob/0d3ee6b55/docs/DXIL.rst#precise-qualifier)
represents precise elementary arithmetic by the absence of fast-math flags. The observed DXIL
contains separate unflagged multiply and add operations. Another compiler switch, extra
metadata, changed input or relaxed expected value is not the correction supported by this
comparison.

The [WARP changelog](https://www.nuget.org/packages/Microsoft.Direct3D.WARP/1.0.21)
records x86 FMA use from 1.0.14 when precise operations are not requested. That motivated the
pre-1.0.14 control; the changelog alone neither predicts the observed failure nor establishes
its exact mechanism.

The native comparison exercises a subset of device features. The factory floor probe must
still satisfy [ADR 0010](../decisions/0010-hardware-floor.md) unchanged. Every normal
Debug/GBV, Release and layer-only test, shader ABI, layout, LTO, stress and benchmark
correctness gate remains required, with the existing case identities and skip policy. A
passing native expression alone establishes none of those results. Hardware and T1 acceptance
remain separate under [ADR 0026](../decisions/0026-d3d12-implemented-before-hardware.md).

The original ZIPs, raw reports and checksum manifests remain outside the published tree under
`rojo-rhi-evidence/luminex-port/p6-shader-precision/2026-10-26-ci12-m4pro/`. Independent intake
checked all 1,105 ZIP members and all 1,103 producer checksum entries from the two compared
artifacts. Raw SDK/header bytes were not uploaded; their build-closure hashes are producer
records. Historical results on 1.0.21 remain unchanged.
