//----------------------------------------------------------------------------------------------------------------------
/// @file BenchAo.slang
/// @brief The composed frame's ambient occlusion: an occlusion kernel from depth and normal, then a blur.
//----------------------------------------------------------------------------------------------------------------------

import RojoRHI;

struct AoRoot
{
    uint depth;
    uint normal;
    uint aoRaw;
    uint width;
    uint height;
    uint pad0;
    uint pad1;
    uint pad2;
};

struct BlurRoot
{
    uint aoRaw;
    uint ao;
    uint width;
    uint height;
};

static const int kRadius = 3;

// Eight taps at a fixed radius: a tap whose depth is nearer the camera than the centre (reverse Z, so larger) and
// lies in front of the centre's tangent plane occludes it.
[shader("compute")]
[numthreads(8, 8, 1)]
void aoMain(uint3 tid : SV_DispatchThreadID)
{
    AoRoot root = rojoRHI::root<AoRoot>();
    if (tid.x >= root.width || tid.y >= root.height)
    {
        return;
    }
    Texture2D<float> depth = rojoRHI::texture2D<float>(root.depth);
    Texture2D<float4> normals = rojoRHI::texture2D<float4>(root.normal);
    RWTexture2D<float> output = rojoRHI::rwTexture2D<float>(root.aoRaw);

    int2 centre = int2(tid.xy);
    float centreDepth = depth.Load(int3(centre, 0));
    float3 centreNormal = normals.Load(int3(centre, 0)).xyz;
    float occlusion = 0.0;
    for (int tap = 0; tap < 8; ++tap)
    {
        int2 offset = int2((tap & 1) != 0 ? kRadius : -kRadius, (tap & 2) != 0 ? kRadius : -kRadius);
        if (tap >= 4)
        {
            offset = int2((tap & 1) != 0 ? 2 * kRadius : 0, (tap & 2) != 0 ? 2 * kRadius : 0) - int2(kRadius, kRadius);
        }
        int2 position = clamp(centre + offset, int2(0, 0), int2(int(root.width) - 1, int(root.height) - 1));
        float tapDepth = depth.Load(int3(position, 0));
        float3 tapNormal = normals.Load(int3(position, 0)).xyz;
        float nearer = tapDepth > centreDepth * 1.0005 ? 1.0 : 0.0;
        float facing = 1.0 - saturate(dot(tapNormal, centreNormal));
        occlusion += nearer * (0.25 + 0.75 * facing);
    }
    output[tid.xy] = 1.0 - saturate(occlusion / 8.0);
}

// A 3x3 box blur of the raw occlusion.
[shader("compute")]
[numthreads(8, 8, 1)]
void aoBlurMain(uint3 tid : SV_DispatchThreadID)
{
    BlurRoot root = rojoRHI::root<BlurRoot>();
    if (tid.x >= root.width || tid.y >= root.height)
    {
        return;
    }
    Texture2D<float> source = rojoRHI::texture2D<float>(root.aoRaw);
    RWTexture2D<float> output = rojoRHI::rwTexture2D<float>(root.ao);

    float sum = 0.0;
    for (int dy = -1; dy <= 1; ++dy)
    {
        for (int dx = -1; dx <= 1; ++dx)
        {
            int2 position = clamp(int2(tid.xy) + int2(dx, dy), int2(0, 0), int2(int(root.width) - 1, int(root.height) - 1));
            sum += source.Load(int3(position, 0));
        }
    }
    output[tid.xy] = sum / 9.0;
}
