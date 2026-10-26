//----------------------------------------------------------------------------------------------------------------------
/// @file AbiPrecise.slang
/// @brief Evaluates a dynamic fp32 multiply followed by an add under the selected precise compilation policy.
//----------------------------------------------------------------------------------------------------------------------

import RojoRHI;
import PreciseTest;

[shader("compute")]
[numthreads(1, 1, 1)]
void preciseMain()
{
    rojoRHI::test::PreciseRoot root = rojoRHI::root<rojoRHI::test::PreciseRoot>();
    float4 input = rojoRHI::load<float4>(root.input, 0);
    float result = input.x * input.y + input.z;
    rojoRHI::store<float>(root.output, 0, result);
}
