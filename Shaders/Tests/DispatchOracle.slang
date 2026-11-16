//----------------------------------------------------------------------------------------------------------------------
/// @file DispatchOracle.slang
/// @brief Conformance dispatch oracle: a compute entry with a threadgroup larger than one in every axis that stores its
/// global thread identifier, and a one, at the thread's linear index in the grid. The root names the output address and
/// the grid's width and height in threads.
//----------------------------------------------------------------------------------------------------------------------

import RojoRHI;

struct GridRoot
{
    rojoRHI::Addr output;
    uint width;
    uint height;
};

[shader("compute")]
[numthreads(8, 4, 2)]
void gridMain(uint3 id : SV_DispatchThreadID)
{
    GridRoot root = rojoRHI::root<GridRoot>();
    uint index = (id.z * root.height + id.y) * root.width + id.x;
    rojoRHI::store<uint4>(root.output, index, uint4(id, 1));
}
