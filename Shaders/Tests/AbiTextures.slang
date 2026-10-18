//----------------------------------------------------------------------------------------------------------------------
/// @file AbiTextures.slang
/// @brief Conformance kernels, one per texture type: each fetches one view slot of the module at a fixed coordinate
/// and stores a single `float4` at `output`, so a readback checks one type at a time.
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
void texture1DMain()
{
    FetchRoot root = rojoRHI::root<FetchRoot>();
    SamplerState linearClamp = rojoRHI::sampler(root.sampler);
    float4 value = rojoRHI::texture1D(root.view).SampleLevel(linearClamp, 0.5, 0);
    rojoRHI::store<float4>(root.output, 0, value);
}

[shader("compute")]
[numthreads(1, 1, 1)]
void texture1DArrayMain()
{
    FetchRoot root = rojoRHI::root<FetchRoot>();
    SamplerState linearClamp = rojoRHI::sampler(root.sampler);
    float4 value = rojoRHI::texture1DArray<float4>(root.view).SampleLevel(linearClamp, float2(0.5, 1), 0);
    rojoRHI::store<float4>(root.output, 0, value);
}

[shader("compute")]
[numthreads(1, 1, 1)]
void texture2DMain()
{
    FetchRoot root = rojoRHI::root<FetchRoot>();
    SamplerState linearClamp = rojoRHI::sampler(rojoRHI::nonUniform(root.sampler));
    float4 value = rojoRHI::texture2D(root.view).SampleLevel(linearClamp, float2(0.5, 0.5), 0);
    rojoRHI::store<float4>(root.output, 0, value);
}

[shader("compute")]
[numthreads(1, 1, 1)]
void texture2DArrayMain()
{
    FetchRoot root = rojoRHI::root<FetchRoot>();
    SamplerState linearClamp = rojoRHI::sampler(root.sampler);
    float4 value = rojoRHI::texture2DArray(root.view).SampleLevel(linearClamp, float3(0.5, 0.5, 1), 0);
    rojoRHI::store<float4>(root.output, 0, value);
}

[shader("compute")]
[numthreads(1, 1, 1)]
void texture3DMain()
{
    FetchRoot root = rojoRHI::root<FetchRoot>();
    SamplerState linearClamp = rojoRHI::sampler(root.sampler);
    float4 value = rojoRHI::texture3D(root.view).SampleLevel(linearClamp, float3(0.5, 0.5, 0.5), 0);
    rojoRHI::store<float4>(root.output, 0, value);
}

[shader("compute")]
[numthreads(1, 1, 1)]
void textureCubeMain()
{
    FetchRoot root = rojoRHI::root<FetchRoot>();
    SamplerState linearClamp = rojoRHI::sampler(root.sampler);
    float4 value = rojoRHI::textureCube(root.view).SampleLevel(linearClamp, float3(0, 0, 1), 0);
    rojoRHI::store<float4>(root.output, 0, value);
}

[shader("compute")]
[numthreads(1, 1, 1)]
void textureCubeArrayMain()
{
    FetchRoot root = rojoRHI::root<FetchRoot>();
    SamplerState linearClamp = rojoRHI::sampler(root.sampler);
    float4 value = rojoRHI::textureCubeArray(root.view).SampleLevel(linearClamp, float4(0, 0, 1, 1), 0);
    rojoRHI::store<float4>(root.output, 0, value);
}

[shader("compute")]
[numthreads(1, 1, 1)]
void compareMain()
{
    FetchRoot root = rojoRHI::root<FetchRoot>();
    SamplerComparisonState shadow = rojoRHI::comparisonSampler(root.comparison);
    Texture2D<float> depth = rojoRHI::texture2D<float>(root.view);
    float4 value = float4(depth.SampleCmpLevelZero(shadow, float2(0.5, 0.5), 0.5), 0, 0, 0);
    rojoRHI::store<float4>(root.output, 0, value);
}

// The pinned Slang lowers a multisample `Load` on Metal with signed coordinates, which MSL rejects, so Metal reads
// the size of the fetched view and the other targets `Load` a sample. It is `[ForceInline]` because a helper that
// takes a texture parameter loses the NonUniform decoration in SPIR-V with the pinned Slang.
[ForceInline]
float4 readMultisample(Texture2DMS<float4> view)
{
    __target_switch
    {
    case metal:
        __intrinsic_asm "float4($0.get_width(), $0.get_height(), $0.get_num_samples(), 0)";
    default:
        return view.Load(int2(1, 1), 0);
    }
}

[ForceInline]
float4 readMultisample(Texture2DMSArray<float4> view)
{
    __target_switch
    {
    case metal:
        __intrinsic_asm "float4($0.get_width(), $0.get_height(), $0.get_array_size(), $0.get_num_samples())";
    default:
        return view.Load(int3(1, 1, 1), 0);
    }
}

[shader("compute")]
[numthreads(1, 1, 1)]
void texture2DMSMain()
{
    FetchRoot root = rojoRHI::root<FetchRoot>();
    float4 value = readMultisample(rojoRHI::texture2DMS(root.view));
    rojoRHI::store<float4>(root.output, 0, value);
}

[shader("compute")]
[numthreads(1, 1, 1)]
void texture2DMSArrayMain()
{
    FetchRoot root = rojoRHI::root<FetchRoot>();
    float4 value = readMultisample(rojoRHI::texture2DMSArray<float4>(root.view));
    rojoRHI::store<float4>(root.output, 0, value);
}

[shader("compute")]
[numthreads(1, 1, 1)]
void rwTexture1DMain()
{
    FetchRoot root = rojoRHI::root<FetchRoot>();
    float4 value = rojoRHI::rwTexture1D(root.view)[1];
    rojoRHI::store<float4>(root.output, 0, value);
}

[shader("compute")]
[numthreads(1, 1, 1)]
void rwTexture1DArrayMain()
{
    FetchRoot root = rojoRHI::root<FetchRoot>();
    float4 value = rojoRHI::rwTexture1DArray<float4>(root.view)[uint2(1, 1)];
    rojoRHI::store<float4>(root.output, 0, value);
}

[shader("compute")]
[numthreads(1, 1, 1)]
void rwTexture2DMain()
{
    FetchRoot root = rojoRHI::root<FetchRoot>();
    RWTexture2D<float4> image = rojoRHI::rwTexture2D(root.view);
    float4 value = image[uint2(1, 1)];
    image[uint2(0, 0)] = value;
    rojoRHI::store<float4>(root.output, 0, value);
}

[shader("compute")]
[numthreads(1, 1, 1)]
void rwTexture2DArrayMain()
{
    FetchRoot root = rojoRHI::root<FetchRoot>();
    float4 value = rojoRHI::rwTexture2DArray(root.view)[uint3(1, 1, 1)];
    rojoRHI::store<float4>(root.output, 0, value);
}

[shader("compute")]
[numthreads(1, 1, 1)]
void rwTexture3DMain()
{
    FetchRoot root = rojoRHI::root<FetchRoot>();
    float4 value = rojoRHI::rwTexture3D(root.view)[uint3(1, 1, 1)];
    rojoRHI::store<float4>(root.output, 0, value);
}
