//----------------------------------------------------------------------------------------------------------------------
/// @file AbiTextures.slang
/// @brief Compile-only conformance kernel: every texture, storage-texture and sampler fetch of the module.
//----------------------------------------------------------------------------------------------------------------------

import RojoRHI;

struct FetchRoot
{
    uint view;
    uint sampler;
    uint comparison;
    uint pad;
    rojoRHI::Addr output;
};

[shader("compute")]
[numthreads(1, 1, 1)]
void computeMain(uint3 tid : SV_DispatchThreadID)
{
    FetchRoot root = rojoRHI::root<FetchRoot>();
    uint view = rojoRHI::nonUniform(root.view + tid.x);
    SamplerState linearClamp = rojoRHI::sampler(rojoRHI::nonUniform(root.sampler));
    SamplerComparisonState shadow = rojoRHI::comparisonSampler(root.comparison);

    float4 sum = rojoRHI::texture1D(view).SampleLevel(linearClamp, 0.5, 0);
    sum += rojoRHI::texture1DArray<float4>(view).SampleLevel(linearClamp, float2(0.5, 1), 0);
    sum += rojoRHI::texture2D(view).SampleLevel(linearClamp, float2(0.5, 0.5), 0);
    sum += rojoRHI::texture2DArray(view).SampleLevel(linearClamp, float3(0.5, 0.5, 1), 0);
    // The pinned Slang lowers a multisample `Load` on Metal with signed coordinates, which MSL rejects, so
    // Metal fetches the views and the other targets read them.
    Texture2DMS<float4> multisample = rojoRHI::texture2DMS(view);
    Texture2DMSArray<float4> multisampleArray = rojoRHI::texture2DMSArray<float4>(view);
    __target_switch
    {
    case metal:
        break;
    default:
        sum += multisample.Load(int2(1, 1), 0);
        sum += multisampleArray.Load(int3(1, 1, 1), 0);
        break;
    }
    sum += rojoRHI::texture3D(view).SampleLevel(linearClamp, float3(0.5, 0.5, 0.5), 0);
    sum += rojoRHI::textureCube(view).SampleLevel(linearClamp, float3(0, 0, 1), 0);
    sum += rojoRHI::textureCubeArray(view).SampleLevel(linearClamp, float4(0, 0, 1, 1), 0);
    sum.x += rojoRHI::texture2D<float>(root.view).SampleCmpLevelZero(shadow, float2(0.5, 0.5), 0.5);

    sum += rojoRHI::rwTexture1D(view)[1];
    sum += rojoRHI::rwTexture1DArray<float4>(view)[uint2(1, 1)];
    sum += rojoRHI::rwTexture2D(view)[uint2(1, 1)];
    sum += rojoRHI::rwTexture2DArray(view)[uint3(1, 1, 1)];
    sum += rojoRHI::rwTexture3D(view)[uint3(1, 1, 1)];
    rojoRHI::rwTexture2D<float4>(root.view)[tid.xy] = sum;
    rojoRHI::store<float4>(root.output, tid.x, sum);
}
