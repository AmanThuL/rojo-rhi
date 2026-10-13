//----------------------------------------------------------------------------------------------------------------------
/// @file AbiCompute.slang
/// @brief Compile-only conformance kernel: reads a root, loads and stores through its addresses.
//----------------------------------------------------------------------------------------------------------------------

import RojoRHI;

struct DrawRoot
{
    rojoRHI::Addr vertices;
    rojoRHI::Addr instances;
    uint albedo;
    uint linearClamp;
    uint drawId;
    uint pad;
};

[shader("compute")]
[numthreads(1, 1, 1)]
void computeMain(uint3 tid : SV_DispatchThreadID)
{
    DrawRoot root = rojoRHI::root<DrawRoot>();
    float4 v = rojoRHI::load<float4>(root.vertices, tid.x);
    float4 w = rojoRHI::load<float4>(rojoRHI::nonUniform(root.instances), root.drawId);
    rojoRHI::store<float4>(root.instances, tid.x, v + w);
    rojoRHI::store<float4>(rojoRHI::nonUniform(root.vertices), rojoRHI::nonUniform(tid.x), w);
    rojoRHI::store<uint>(root.instances, tid.x + 1, ROJORHI_SHADER_ABI_VERSION + root.albedo);
}
