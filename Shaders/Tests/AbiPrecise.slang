//----------------------------------------------------------------------------------------------------------------------
/// @file AbiPrecise.slang
/// @brief A multiply followed by an add of loaded fp32 values, built under slang.precise_files.
//----------------------------------------------------------------------------------------------------------------------

import RojoRHI;
import PreciseTest;

// The operands are loaded, so no compiler folds them; only contraction into a fused multiply-add could change the
// result, and the precise policy forbids it.
[shader("compute")]
[numthreads(1, 1, 1)]
void preciseMain()
{
    PreciseRoot root = rojoRHI::root<PreciseRoot>();
    float4 input = rojoRHI::load<float4>(root.input, 0);
    rojoRHI::store<float>(root.output, 0, input.x * input.y + input.z);
}
