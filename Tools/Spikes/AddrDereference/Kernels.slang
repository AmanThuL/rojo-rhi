// Spike 1 kernel H1m: arm H1 written through the RojoRHI shader module, so the DXIL is what the
// module emits for D3D12. Throwaway spike source; see
// docs/research/2026-10-20-spike-1-d3d12-addr-dereference.md for what it measures and why.
//
// One kernel per `slangc -D ROJORHI_TARGET_HLSL -D MODE=<0 uniform, 1 wave-uniform, 2 divergent>
// -D ALU=<0 fetch, 1 alu>`. The root block and the output match H1's in Kernels.hlsl: 64 Addrs,
// entry k the raw view of slice k, and the sum stored through heap slot 2, the output's raw UAV.

#if !defined(MODE) || !defined(ALU)
#error "compile with -D MODE=0|1|2 -D ALU=0|1"
#endif

import RojoRHI;

struct Root
{
    rojoRHI::Addr bases[64];
};

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

// The shared body: 16 loads of 16-byte elements inside slice `base`, summed; the alu variant
// follows each load with 16 rounds of a 32-bit LCG.
uint4 sumSlice(rojoRHI::Addr base, uint t)
{
    uint4 sum = uint4(0u);
    [unroll]
    for (uint j = 0u; j < 16u; ++j)
    {
        uint e = (j << 12u) | (t & 4095u);
        uint4 v = rojoRHI::load<uint4>(base, e);
#if ALU
        [unroll]
        for (uint r = 0u; r < 16u; ++r)
            v = v * 1664525u + 1013904223u;
#endif
        sum += v;
    }
    return sum;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void spikeMain(uint3 id : SV_DispatchThreadID)
{
    uint t = id.x;
    uint k = sliceOf(t);
    rojoRHI::Addr b = rojoRHI::root<Root>().bases[k];
#if MODE == 2
    // A per-thread base must be marked; a wave-uniform one needs no marker.
    b = rojoRHI::nonUniform(b);
#endif
    rojoRHI::store<uint4>(rojoRHI::Addr(1u, 0u), t, sumSlice(b, t));
}
