import RojoRHI;

static const uint kTaskThreads = 32;
static const uint kSide = 8;
static const uint kVertexCount = kSide * kSide;
static const uint kTriangleCount = (kSide - 1) * (kSide - 1) * 2;

struct Mat4
{
    float4 r0;
    float4 r1;
    float4 r2;
    float4 r3;
};

struct TerrainRoot
{
    Mat4 viewProj;
    float time;
    float patchSize;
    uint patchesPerSide;
    uint pad;
};

struct TaskPayload
{
    uint patches[kTaskThreads];
};

struct Varyings
{
    float4 position : SV_Position;
    float3 normal : NORMAL;
    float3 color : COLOR;
};

float4 transform(Mat4 m, float4 p)
{
    return float4(dot(m.r0, p), dot(m.r1, p), dot(m.r2, p), dot(m.r3, p));
}

float height(float2 p, float t)
{
    return 2.0 * sin(0.11 * p.x + t) * cos(0.09 * p.y + 0.7 * t) + 0.6 * sin(0.37 * p.x - 0.29 * p.y + 1.3 * t);
}

float2 patchOrigin(TerrainRoot root, uint patch)
{
    float2 cell = float2(patch % root.patchesPerSide, patch / root.patchesPerSide);
    return (cell - 0.5 * float(root.patchesPerSide)) * root.patchSize;
}

bool visible(Mat4 m, float3 center, float radius)
{
    float4 planes[5] = { m.r3 + m.r0, m.r3 - m.r0, m.r3 + m.r1, m.r3 - m.r1, m.r3 - m.r2 };
    for (uint i = 0; i < 5; ++i)
    {
        if (dot(planes[i].xyz, center) + planes[i].w < -radius * length(planes[i].xyz))
        {
            return false;
        }
    }
    return true;
}

float3 patchColor(uint patch)
{
    uint h = patch * 2654435761u;
    return float3(float((h >> 8) & 255), float((h >> 16) & 255), float((h >> 24) & 255)) / 255.0 * 0.6 + 0.4;
}

groupshared TaskPayload gPayload;
groupshared uint gCount;

[shader("amplification")]
[numthreads(kTaskThreads, 1, 1)]
void terrainTask(uint thread : SV_GroupThreadID, uint group : SV_GroupID)
{
    TerrainRoot root = rojoRHI::root<TerrainRoot>();
    if (thread == 0)
    {
        gCount = 0;
    }
    GroupMemoryBarrierWithGroupSync();

    uint patch = group * kTaskThreads + thread;
    float2 center = patchOrigin(root, patch) + 0.5 * root.patchSize;
    if (visible(root.viewProj, float3(center.x, 0.0, center.y), 0.75 * root.patchSize + 2.6))
    {
        uint slot;
        InterlockedAdd(gCount, 1, slot);
        gPayload.patches[slot] = patch;
    }
    GroupMemoryBarrierWithGroupSync();

    DispatchMesh(gCount, 1, 1, gPayload);
}

[shader("mesh")]
[outputtopology("triangle")]
[numthreads(kVertexCount, 1, 1)]
void terrainMesh(
    uint thread : SV_GroupThreadID,
    uint group : SV_GroupID,
    in payload TaskPayload payload,
    out vertices Varyings vertices[kVertexCount],
    out indices uint3 triangles[kTriangleCount])
{
    TerrainRoot root = rojoRHI::root<TerrainRoot>();
    SetMeshOutputCounts(kVertexCount, kTriangleCount);

    uint patch = payload.patches[group];
    float step = root.patchSize / float(kSide - 1);
    uint2 grid = uint2(thread % kSide, thread / kSide);
    float2 p = patchOrigin(root, patch) + float2(grid) * step;

    float h = height(p, root.time);
    float dx = height(p + float2(0.1, 0.0), root.time) - h;
    float dz = height(p + float2(0.0, 0.1), root.time) - h;

    Varyings vertex;
    vertex.position = transform(root.viewProj, float4(p.x, h, p.y, 1.0));
    vertex.normal = normalize(float3(-dx, 0.1, -dz));
    vertex.color = patchColor(patch) * (0.55 + 0.12 * h);
    vertices[thread] = vertex;

    if (grid.x < kSide - 1 && grid.y < kSide - 1)
    {
        uint first = 2 * (grid.y * (kSide - 1) + grid.x);
        uint second = first + 1;
        triangles[first] = uint3(thread, thread + 1, thread + kSide);
        triangles[second] = uint3(thread + 1, thread + kSide + 1, thread + kSide);
    }
}
