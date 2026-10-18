//----------------------------------------------------------------------------------------------------------------------
/// @file AbiFragment.slang
/// @brief Conformance fragment entry: samples a 2D view at the interpolated `uv` and adds the flat `drawId` to the
/// red channel. It is alone in its file because Slang passes every reserved binding that any entry of a file touches
/// to every entry, which would give a non-void vertex entry writable buffer parameters.
//----------------------------------------------------------------------------------------------------------------------

import RojoRHI;

struct FragmentRoot
{
    uint view;
    uint sampler;
    uint pad0;
    uint pad1;
};

struct FragmentInput
{
    float4 position : SV_Position;
    nointerpolation uint drawId : DRAWID;
    float2 uv : TEXCOORD0;
};

[shader("fragment")]
float4 fragmentMain(FragmentInput input) : SV_Target
{
    FragmentRoot root = rojoRHI::root<FragmentRoot>();
    float4 texel = rojoRHI::texture2D(root.view).Sample(rojoRHI::sampler(root.sampler), input.uv);
    return texel + float4(float(input.drawId), 0, 0, 0);
}
