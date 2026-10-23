//----------------------------------------------------------------------------------------------------------------------
/// @file SharedImport.slang
/// @brief Imports `TestShared` from `Shaders/Tests/Modules` beside `RojoRHI`. Its one compute entry touches no reserved
/// binding (no `root`, `load`, `store`, texture or sampler call), as `Standard.slang`'s entries do, so every backend can
/// create a pipeline from it.
//----------------------------------------------------------------------------------------------------------------------

import RojoRHI;
import TestShared;

groupshared uint gShared;

[shader("compute")]
[numthreads(1, 1, 1)]
void sharedMain()
{
    gShared = rojoRHI::test::sharedValue() + ROJORHI_SHADER_ABI_VERSION;
}
