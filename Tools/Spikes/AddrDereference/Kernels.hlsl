// Spike 1 kernels F, H1 and H2, hand-written HLSL for D3D12. Throwaway spike source; see
// docs/research/2026-10-20-spike-1-d3d12-addr-dereference.md for what they measure and why.
//
// One kernel per `dxc -D ARM=<0 F, 1 H1, 2 H2> -D MODE=<0 uniform, 1 wave-uniform, 2 divergent>
// -D ALU=<0 fetch, 1 alu>`. Every arm runs the same body over a 1 MiB slice of the data buffer;
// only the way the slice's base is reached differs:
//   F   the data buffer itself is the root SRV, and the slice is the byte offset k << 20
//   H1  the root SRV holds 64 Addrs (heap index, byte offset); the slice is raw view Addr k
//   H2  the root SRV holds Addr A of a table of the same 64 Addrs; the slice is the table's entry k

#if !defined(ARM) || !defined(MODE) || !defined(ALU)
#error "compile with -D ARM=0|1|2 -D MODE=0|1|2 -D ALU=0|1"
#endif

// The root SRV at t0, space 0: the data buffer for F, the root block for H1 and H2.
ByteAddressBuffer gRootBytes : register(t0);

// The slice thread t reads: one for the dispatch, one per 64 consecutive threads, or one per thread.
uint sliceOf(uint t)
{
#if MODE == 0
    return 0u;
#elif MODE == 1
    return (t >> 6u) & 63u;
#else
    return t & 63u;
#endif
}

// The shared body: 16 loads of 16-byte elements inside the slice that starts at byte `base` of
// `data`, summed; the alu variant follows each load with 16 rounds of a 32-bit LCG.
uint4 sumSlice(ByteAddressBuffer data, uint base, uint t)
{
    uint4 sum = 0u;
    [unroll]
    for (uint j = 0u; j < 16u; ++j)
    {
        uint e = (j << 12u) | (t & 4095u);
        uint4 v = data.Load<uint4>(base + e * 16u);
#if ALU
        [unroll]
        for (uint r = 0u; r < 16u; ++r)
            v = v * 1664525u + 1013904223u;
#endif
        sum += v;
    }
    return sum;
}

#if ARM == 1 || ARM == 2
// The Addr of slice k: entry k of the root block (H1), or entry k of table A, whose Addr is the
// root block's first entry (H2).
uint2 sliceAddr(uint k)
{
#if ARM == 1
    return gRootBytes.Load2(8u * k);
#else
    uint2 a = gRootBytes.Load2(0u);
    ByteAddressBuffer table = ResourceDescriptorHeap[a.x];
    return table.Load2(a.y + 8u * k);
#endif
}
#endif

[numthreads(64, 1, 1)]
void spikeMain(uint3 id : SV_DispatchThreadID)
{
    uint t = id.x;
    uint k = sliceOf(t);
#if ARM == 0
    uint4 sum = sumSlice(gRootBytes, k << 20u, t);
#elif ARM == 1 || ARM == 2
    uint2 b = sliceAddr(k);
#if MODE == 2
    // A per-thread heap index must be marked; a wave-uniform one needs no marker.
    ByteAddressBuffer data = ResourceDescriptorHeap[NonUniformResourceIndex(b.x)];
#else
    ByteAddressBuffer data = ResourceDescriptorHeap[b.x];
#endif
    uint4 sum = sumSlice(data, b.y, t);
#else
#error "ARM must be 0, 1 or 2"
#endif
    // Heap slot 2 is the output's raw UAV.
    RWByteAddressBuffer output = ResourceDescriptorHeap[2];
    output.Store<uint4>(t * 16u, sum);
}
