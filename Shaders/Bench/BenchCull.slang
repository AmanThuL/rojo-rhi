//----------------------------------------------------------------------------------------------------------------------
/// @file BenchCull.slang
/// @brief The composed frame's culling kernel: keeps box i in every view when i mod 5 is below 3 and writes its draw record.
//----------------------------------------------------------------------------------------------------------------------

import RojoRHI;

struct Transform
{
    float4 row0;
    float4 row1;
    float4 row2;
    float4 row3;
};

struct DrawIndexedIndirectArgs
{
    uint drawId;
    uint indexCount;
    uint instanceCount;
    uint firstIndex;
    int baseVertex;
    uint firstInstance;
};

struct CullRoot
{
    rojoRHI::Addr instances;
    rojoRHI::Addr records0;
    rojoRHI::Addr records1;
    rojoRHI::Addr records2;
    rojoRHI::Addr records3;
    rojoRHI::Addr counts0;
    rojoRHI::Addr counts1;
    rojoRHI::Addr counts2;
    rojoRHI::Addr counts3;
    uint pad0;
    uint pad1;
};

static const uint kBoxCount = 10000;
static const uint kMeshCount = 64;
static const uint kMeshVertexCount = 24;
static const uint kMeshIndexCount = 36;
static const uint kKeptPerView = 6000;

// Thread t of 40,000 handles view t / 10,000 and box t mod 10,000.
[shader("compute")]
[numthreads(64, 1, 1)]
void cullMain(uint3 tid : SV_DispatchThreadID)
{
    CullRoot root = rojoRHI::root<CullRoot>();
    uint view = tid.x / kBoxCount;
    uint box = tid.x % kBoxCount;
    if (view >= 4)
    {
        return;
    }

    rojoRHI::Addr records = root.records0;
    rojoRHI::Addr counts = root.counts0;
    if (view == 1)
    {
        records = root.records1;
        counts = root.counts1;
    }
    else if (view == 2)
    {
        records = root.records2;
        counts = root.counts2;
    }
    else if (view == 3)
    {
        records = root.records3;
        counts = root.counts3;
    }
    records = rojoRHI::nonUniform(records);
    counts = rojoRHI::nonUniform(counts);

    if (box == 0)
    {
        rojoRHI::store<uint>(counts, 0, kKeptPerView);
    }

    uint phase = box % 5;
    if (phase >= 3)
    {
        return;
    }

    // Every transform has a bottom row of (0, 0, 0, 1), so the test always passes; it keeps the instance read live.
    Transform instance = rojoRHI::load<Transform>(root.instances, box);
    if (instance.row3.w <= 0.0)
    {
        return;
    }

    uint mesh = box % kMeshCount;
    DrawIndexedIndirectArgs record;
    record.drawId = box;
    record.indexCount = kMeshIndexCount;
    record.instanceCount = 1;
    record.firstIndex = kMeshIndexCount * mesh;
    record.baseVertex = int(kMeshVertexCount * mesh);
    record.firstInstance = 0;
    rojoRHI::store<DrawIndexedIndirectArgs>(records, 3 * (box / 5) + phase, record);
}
