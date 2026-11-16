//----------------------------------------------------------------------------------------------------------------------
/// @file BarrierOracle.slang
/// @brief Conformance barrier oracle: a compute entry that, in each thread, spins for the root's count of steps and then
/// stores the root's value into one texel of a 2D storage view, so an earlier, slow dispatch that a barrier fails to wait
/// for leaves its value behind a later, fast one. The root names the view, the spin count, a mask that is always zero,
/// the extent in texels and the value.
//----------------------------------------------------------------------------------------------------------------------

import RojoRHI;

struct DelayRoot
{
    uint view;
    uint spins;
    uint mask;
    uint pad;
    uint width;
    uint height;
    uint pad0;
    uint pad1;
    float4 value;
};

[shader("compute")]
[numthreads(8, 8, 1)]
void delayedFillMain(uint3 id : SV_DispatchThreadID)
{
    DelayRoot root = rojoRHI::root<DelayRoot>();
    if (id.x >= root.width || id.y >= root.height)
    {
        return;
    }
    // A linear congruential step per spin. The mask is zero, which the compiler cannot know, so the loop stays and the
    // stored value does not change.
    uint state = id.x * 7919u + id.y;
    for (uint step = 0; step < root.spins; ++step)
    {
        state = state * 1664525u + 1013904223u;
    }
    float4 value = root.value;
    value.x = asfloat(asuint(value.x) | (state & root.mask));
    rojoRHI::wTexture2D(root.view).Store(id.xy, value);
}
