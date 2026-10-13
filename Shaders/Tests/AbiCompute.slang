//----------------------------------------------------------------------------------------------------------------------
/// @file AbiCompute.slang
/// @brief Compile-only conformance kernel: writes the shader ABI version to a buffer.
//----------------------------------------------------------------------------------------------------------------------

import RojoRHI;

RWStructuredBuffer<uint> gOut;

[shader("compute")]
[numthreads(1, 1, 1)]
void computeMain()
{
    gOut[0] = ROJORHI_SHADER_ABI_VERSION;
}
