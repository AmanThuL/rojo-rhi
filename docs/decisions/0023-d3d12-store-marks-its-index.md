# ADR 0023: The D3D12 store marks its own heap index non-uniform

**Status**: Accepted (2026-10-20)

## Context

[ADR 0018](0018-root-srv-and-reserved-bindings.md) states, in its "Mapping corrections", that the
emitted HLSL has one `NonUniformResourceIndex`, in the return of the helper behind `nonUniform`,
and that every heap indexing site, `store` included, is a plain heap index. It leaves to
verification whether DXC keeps the qualifier through that function return, the `uint2` pack and
unpack and the `+ 1` of the store's index.

Compiled DXIL answers it. The qualifier survives the function return and the pack and unpack: the
heap handles of `load`, texture and sampler fetches carry the non-uniform flag. It is lost through
the `+ 1`: a `store` to a divergent base gets a handle without the flag, and so does a plain
`ResourceDescriptorHeap[NonUniformResourceIndex(i) + 1]`. Only `NonUniformResourceIndex(i + 1)`
keeps it.

## Decision

- On the HLSL and DXIL targets, `rojoRHI::store` indexes
  `ResourceDescriptorHeap[NonUniformResourceIndex(index + 1)]` for every base. This amends the one
  sentence of ADR 0018 that makes `store` a plain heap index, as ADR 0018 amended ADR 0011; the rest
  of ADR 0018 is unchanged.
- `ROJORHI_SHADER_ABI_VERSION` does not change. No reserved binding of ADR 0018's table, the `Addr`
  encoding or the bytes a built-in reads or writes changes, and the Metal and SPIR-V output is
  identical.

## Consequences

- A store to a uniform base is marked too. That is correct, and costs at most a one-iteration
  scalarization on hardware that loops over distinct descriptors.
- The alternative, an `Addr` encoding without the `+ 1`, would keep uniform stores unmarked but
  change the encoding and so the ABI version.
- Chapter 05 states that the ABI version covers the bindings, the encoding and the bytes each
  built-in reads and writes, not the instructions a target emits for them.
