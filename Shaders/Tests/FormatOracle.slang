//----------------------------------------------------------------------------------------------------------------------
/// @file FormatOracle.slang
/// @brief Conformance format oracle: kernels that sample, load, store and read the samples of a 4 by 4 texture of
/// any format through the module's accessors and store what they read as `float4` values, and a fragment entry that
/// writes the color the root holds. The vertex stage comes from `DrawOracle.slang`, whose entries touch no reserved
/// binding: a vertex entry in this file would be given the root's buffer, which Metal warns about in a vertex
/// function that returns a value.
//----------------------------------------------------------------------------------------------------------------------

import RojoRHI;

// The edge of every texture the oracle reads or writes, in texels: one BC1 block.
static const uint kEdge = 4;

// The most samples `readSamplesMain` stores per texel.
static const uint kMaxSamples = 8;

struct ReadRoot
{
    uint view;
    uint sampler;
    uint samples;
    uint pad;
    rojoRHI::Addr output;
    float2 at;
};

struct WriteRoot
{
    uint view;
    uint pad0;
    uint pad1;
    uint pad2;
    float4 left;
    float4 right;
};

struct ColorRoot
{
    float4 color;
};

// Texel `id` of a 4 by 4 texture, as the index of its `float4` in the output.
uint texelIndex(uint2 id)
{
    return id.y * kEdge + id.x;
}

[shader("compute")]
[numthreads(4, 4, 1)]
void sampleTexelsMain(uint3 id : SV_DispatchThreadID)
{
    ReadRoot root = rojoRHI::root<ReadRoot>();
    float2 uv = (float2(id.xy) + 0.5) / float(kEdge);
    float4 value = rojoRHI::texture2D(root.view).SampleLevel(rojoRHI::sampler(root.sampler), uv, 0);
    rojoRHI::store<float4>(root.output, texelIndex(id.xy), value);
}

[shader("compute")]
[numthreads(1, 1, 1)]
void sampleAtMain()
{
    ReadRoot root = rojoRHI::root<ReadRoot>();
    float4 value = rojoRHI::texture2D(root.view).SampleLevel(rojoRHI::sampler(root.sampler), root.at, 0);
    rojoRHI::store<float4>(root.output, 0, value);
}

[shader("compute")]
[numthreads(4, 4, 1)]
void loadTexelsMain(uint3 id : SV_DispatchThreadID)
{
    ReadRoot root = rojoRHI::root<ReadRoot>();
    float4 value = rojoRHI::rwTexture2D(root.view)[id.xy];
    rojoRHI::store<float4>(root.output, texelIndex(id.xy), value);
}

[shader("compute")]
[numthreads(4, 4, 1)]
void storeTexelsMain(uint3 id : SV_DispatchThreadID)
{
    WriteRoot root = rojoRHI::root<WriteRoot>();
    rojoRHI::rwTexture2D(root.view)[id.xy] = id.x < kEdge / 2 ? root.left : root.right;
}

[shader("compute")]
[numthreads(4, 4, 1)]
void writeTexelsMain(uint3 id : SV_DispatchThreadID)
{
    WriteRoot root = rojoRHI::root<WriteRoot>();
    rojoRHI::wTexture2D(root.view).Store(id.xy, id.x < kEdge / 2 ? root.left : root.right);
}

// Sample `sample` of texel `coord`. The pinned Slang lowers a multisample `Load` on Metal with signed coordinates,
// which MSL rejects, so Metal reads through the texture's own `read`. It is `[ForceInline]` because a helper that
// takes a texture parameter loses the NonUniform decoration in SPIR-V with the pinned Slang.
[ForceInline]
float4 readSample(Texture2DMS<float4> view, uint2 coord, uint sample)
{
    __target_switch
    {
    case metal:
        __intrinsic_asm "$0.read($1, $2)";
    default:
        return view.Load(int2(coord), int(sample));
    }
}

[shader("compute")]
[numthreads(4, 4, 1)]
void readSamplesMain(uint3 id : SV_DispatchThreadID)
{
    ReadRoot root = rojoRHI::root<ReadRoot>();
    Texture2DMS<float4> view = rojoRHI::texture2DMS(root.view);
    for (uint sample = 0; sample < root.samples; ++sample)
    {
        float4 value = readSample(view, id.xy, sample);
        rojoRHI::store<float4>(root.output, texelIndex(id.xy) * kMaxSamples + sample, value);
    }
}

[shader("fragment")]
float4 colorFragmentMain() : SV_Target
{
    return rojoRHI::root<ColorRoot>().color;
}
