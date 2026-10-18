//----------------------------------------------------------------------------------------------------------------------
/// @file FrameChecks.cpp
/// @brief Checks the composed frame's shared facts: the scene, the culling rule, the layouts, the passes and the shaders.
//----------------------------------------------------------------------------------------------------------------------
#include "Common/ArmResult.h"
#include "Common/CounterFields.h"
#include "Common/FrameLayout.h"
#include "Common/FrameRoots.h"
#include "Common/Registry.h"
#include "Common/RunOptions.h"
#include "Common/Scene.h"
#include "Native/NativeContext.h"
#include "Native/NativeFrame.h"
#include "Native/NativeFrameBarriers.h"
#include "Rojo/RojoTrace.h"
#include "ShaderLayout.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <initializer_list>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace rojoRHI;
using namespace rojoRHI::bench;

namespace {
    //==================================================================================================================
    template<class T> bool sameBytes(const T& a, const T& b)
    {
        return std::memcmp(&a, &b, sizeof(T)) == 0;
    }

    //==================================================================================================================
    // The clip-space depth of a world point under a view-projection, after the divide by w.
    float clipDepth(const Transform& viewProjection, Float4 world)
    {
        const Float4 clip = transformPoint(viewProjection, world);
        return clip.z / clip.w;
    }

    //==================================================================================================================
    Float4 clipNdc(const Transform& viewProjection, Float4 world)
    {
        const Float4 clip = transformPoint(viewProjection, world);
        return Float4{ clip.x / clip.w, clip.y / clip.w, clip.z / clip.w, 1.0f };
    }
} // namespace

//======================================================================================================================
TEST_CASE("the scene is the same bytes on every call", "[host]")
{
    for (uint32_t box = 0; box < kBoxCount; box += 7)
    {
        CHECK(sameBytes(instanceTransform(box), instanceTransform(box)));
    }
    for (uint32_t mesh = 0; mesh < kMeshCount; ++mesh)
    {
        CHECK(sameBytes(meshVertices(mesh), meshVertices(mesh)));
        CHECK(sameBytes(meshIndices(mesh), meshIndices(mesh)));
    }
    CHECK(sameBytes(cameraViewProjection(), cameraViewProjection()));
    CHECK(sameBytes(cameraInverseViewProjection(), cameraInverseViewProjection()));
    for (uint32_t cascade = 0; cascade < kCascadeCount; ++cascade)
    {
        CHECK(sameBytes(cascadeViewProjection(cascade), cascadeViewProjection(cascade)));
    }
    for (uint32_t index = 0; index < kForwardBoxCount; ++index)
    {
        const ForwardBox a = forwardBox(index);
        const ForwardBox b = forwardBox(index);
        CHECK(a.box == b.box);
        CHECK(sameBytes(a.transform, b.transform));
        CHECK(sameBytes(a.color, b.color));
    }
    CHECK(allInstances().size() == kBoxCount);
    CHECK(allVertices().size() == kMeshCount * kMeshVertexCount);
    CHECK(allIndices().size() == kMeshCount * kMeshIndexCount);
}

//======================================================================================================================
TEST_CASE("every instance transform has the bottom row 0 0 0 1 and a uniform scale", "[host]")
{
    for (uint32_t box = 0; box < kBoxCount; ++box)
    {
        const Transform t = instanceTransform(box);
        REQUIRE(t.row3.x == 0.0f);
        REQUIRE(t.row3.y == 0.0f);
        REQUIRE(t.row3.z == 0.0f);
        REQUIRE(t.row3.w == 1.0f);
        const float scale = t.row1.y;
        REQUIRE(std::abs(std::hypot(t.row0.x, t.row0.z) - scale) < 1e-4f);
    }
}

//======================================================================================================================
TEST_CASE("the culling rule keeps 6000 boxes per view with their records at the stated indices", "[host]")
{
    const std::vector<DrawIndexedIndirectArgs> records = cullRecords();
    REQUIRE(records.size() == kKeptPerView);

    uint32_t kept = 0;
    for (uint32_t box = 0; box < kBoxCount; ++box)
    {
        if (!isKept(box))
        {
            continue;
        }
        ++kept;
        const uint32_t index = 3 * (box / 5) + box % 5;
        CAPTURE(box);
        REQUIRE(recordIndex(box) == index);
        REQUIRE(records[index].drawId == box);
        REQUIRE(records[index].indexCount == kMeshIndexCount);
        REQUIRE(records[index].instanceCount == 1);
        REQUIRE(records[index].firstIndex == kMeshIndexCount * (box % kMeshCount));
        REQUIRE(records[index].baseVertex == static_cast<int32_t>(kMeshVertexCount * (box % kMeshCount)));
        REQUIRE(records[index].firstInstance == 0);
    }
    CHECK(kept == kKeptPerView);
}

//======================================================================================================================
TEST_CASE("every mesh index stays below its mesh's 24 vertices and the draws reach each mesh's range", "[host]")
{
    for (uint32_t mesh = 0; mesh < kMeshCount; ++mesh)
    {
        for (const uint16_t index : meshIndices(mesh))
        {
            REQUIRE(index < kMeshVertexCount);
        }
    }
    const std::vector<uint16_t> indices = allIndices();
    for (const DrawIndexedIndirectArgs& record : cullRecords())
    {
        REQUIRE(record.firstIndex + record.indexCount <= indices.size());
        REQUIRE(static_cast<uint32_t>(record.baseVertex) + kMeshVertexCount <= kMeshCount * kMeshVertexCount);
    }
}

//======================================================================================================================
TEST_CASE("a mesh's normals are unit length and wound outward", "[host]")
{
    const std::array<MeshVertex, kMeshVertexCount> vertices = meshVertices(5);
    const std::array<uint16_t, kMeshIndexCount> indices = meshIndices(5);
    for (uint32_t triangle = 0; triangle < kMeshIndexCount / 3; ++triangle)
    {
        const MeshVertex& a = vertices[indices[triangle * 3]];
        const MeshVertex& b = vertices[indices[triangle * 3 + 1]];
        const MeshVertex& c = vertices[indices[triangle * 3 + 2]];
        const float abx = b.position.x - a.position.x, aby = b.position.y - a.position.y, abz = b.position.z - a.position.z;
        const float acx = c.position.x - a.position.x, acy = c.position.y - a.position.y, acz = c.position.z - a.position.z;
        const float nx = aby * acz - abz * acy, ny = abz * acx - abx * acz, nz = abx * acy - aby * acx;
        CHECK(nx * a.normal.x + ny * a.normal.y + nz * a.normal.z > 0.0f);
        CHECK(std::abs(a.normal.x * a.normal.x + a.normal.y * a.normal.y + a.normal.z * a.normal.z - 1.0f) < 1e-6f);
    }
}

//======================================================================================================================
TEST_CASE("the forward pass draws 500 alpha-blended boxes of the scene in index order", "[host]")
{
    uint32_t previous = 0;
    for (uint32_t index = 0; index < kForwardBoxCount; ++index)
    {
        const ForwardBox box = forwardBox(index);
        REQUIRE(box.box < kBoxCount);
        REQUIRE((index == 0 || box.box > previous));
        REQUIRE(box.color.w < 1.0f);
        REQUIRE(box.color.w > 0.0f);
        REQUIRE(sameBytes(box.transform, instanceTransform(box.box)));
        previous = box.box;
    }
    CHECK(kForwardDraws == 500);
}

//======================================================================================================================
TEST_CASE("the camera and the cascades use reverse Z with the origin in view", "[host]")
{
    const Float4 origin{ 0.0f, 0.0f, 0.0f, 1.0f };
    const Transform camera = cameraViewProjection();
    const Float4 centre = clipNdc(camera, origin);
    CHECK(std::abs(centre.x) < 1.0f);
    CHECK(std::abs(centre.y) < 1.0f);
    CHECK(centre.z > 0.0f);
    CHECK(centre.z < 1.0f);
    // Nearer the eye is a larger depth.
    CHECK(clipDepth(camera, Float4{ 0.0f, 0.0f, 100.0f, 1.0f }) > clipDepth(camera, Float4{ 0.0f, 0.0f, -100.0f, 1.0f }));

    for (uint32_t cascade = 0; cascade < kCascadeCount; ++cascade)
    {
        const Transform view = cascadeViewProjection(cascade);
        const Float4 inside = clipNdc(view, origin);
        CAPTURE(cascade);
        CHECK(std::abs(inside.x) < 1.0f);
        CHECK(std::abs(inside.y) < 1.0f);
        CHECK(inside.z > 0.0f);
        CHECK(inside.z < 1.0f);
        const Float4 light = lightDirection();
        const Float4 towardLight{ -light.x * 50.0f, -light.y * 50.0f, -light.z * 50.0f, 1.0f };
        CHECK(clipDepth(view, towardLight) > inside.z);
    }
}

//======================================================================================================================
TEST_CASE("the inverse camera matrix rebuilds a world position from a depth", "[host]")
{
    const Transform camera = cameraViewProjection();
    const Transform inverse = cameraInverseViewProjection();
    for (const Float4 world : { Float4{ 0.0f, 0.0f, 0.0f, 1.0f }, Float4{ 30.0f, 2.0f, -40.0f, 1.0f }, Float4{ -50.0f, 1.0f, 60.0f, 1.0f } })
    {
        const Float4 clip = transformPoint(camera, world);
        const Float4 back = transformPoint(inverse, clip);
        CHECK(std::abs(back.x / back.w - world.x) < 5e-2f);
        CHECK(std::abs(back.y / back.w - world.y) < 5e-2f);
        CHECK(std::abs(back.z / back.w - world.z) < 5e-2f);
    }
}

//======================================================================================================================
TEST_CASE("every root block is flat: aligned, a multiple of 16 bytes and within 64 KiB", "[host]")
{
    const std::array<size_t, kRootStructCount> sizes{ sizeof(CullRoot),    sizeof(GeometryRoot), sizeof(ShadowRoot),  sizeof(AoRoot),
                                                      sizeof(BlurRoot),    sizeof(LightingRoot), sizeof(ForwardRoot), sizeof(BloomDownRoot),
                                                      sizeof(BloomUpRoot), sizeof(ToneMapRoot) };
    for (const size_t size : sizes)
    {
        CAPTURE(size);
        CHECK(size % 16 == 0);
        CHECK(size <= 65536);
    }
    CHECK(alignof(ForwardRoot) == 16);
    CHECK(sizeof(ForwardRoot) == 96);
    CHECK(sizeof(Transform) == 64);
    CHECK(sizeof(MeshVertex) == 32);
}

//======================================================================================================================
TEST_CASE("the probe's generated assertions cover every root and record", "[host]")
{
    // 10 roots, the transform and vertex records, and the chapter 08 indexed record.
    CHECK(kShaderLayoutStructCount == 13);
}

//======================================================================================================================
TEST_CASE("the record layout of a view is 256-byte aligned and holds 10,000 records", "[host]")
{
    CHECK(sizeof(DrawIndexedIndirectArgs) == 24);
    CHECK(kViewStride % 256 == 0);
    CHECK(kViewStride >= kRecordsOffset + uint64_t{ kRecordsPerView } * sizeof(DrawIndexedIndirectArgs));
    CHECK(kViewStride - 256 < kRecordsOffset + uint64_t{ kRecordsPerView } * sizeof(DrawIndexedIndirectArgs));
    CHECK(kRecordsBytes == 4 * kViewStride);
    CHECK(viewCountOffset(2) == 2 * kViewStride);
    CHECK(viewRecordsOffset(3) == 3 * kViewStride + 16);
    CHECK(slotOf(0) == 0);
    CHECK(slotOf(4) == 1);
}

//======================================================================================================================
TEST_CASE("the passes, lists, dispatches and outputs are the pre-registered ones", "[host]")
{
    CHECK(kPasses.size() == 14);
    CHECK(kPasses[0].label == "cull");
    CHECK(kPasses[13].label == "readback");
    uint32_t perList[kListCount] = {};
    for (const PassSpec& pass : kPasses)
    {
        ++perList[static_cast<size_t>(pass.list)];
    }
    CHECK(perList[static_cast<size_t>(FrameList::Cull)] == 1);
    CHECK(perList[static_cast<size_t>(FrameList::Gbuffer)] == 1);
    CHECK(perList[static_cast<size_t>(FrameList::Ao)] == 1);
    CHECK(perList[static_cast<size_t>(FrameList::Shadows)] == 3);
    CHECK(perList[static_cast<size_t>(FrameList::Lighting)] == 8);
    CHECK(listSpec(FrameList::Cull).queue == QueueType::Compute);
    CHECK(listSpec(FrameList::Ao).queue == QueueType::Compute);
    CHECK(listSpec(FrameList::Gbuffer).queue == QueueType::Graphics);
    CHECK(listSpec(FrameList::Shadows).label == "shadows");

    CHECK(kCullGroups.width * kCullThreadsPerGroup == kViewCount * kBoxCount);
    CHECK(kImageGroups.width * kImageThreadsPerGroup.width == kFrameWidth);
    CHECK(kImageGroups.height * kImageThreadsPerGroup.height == kFrameHeight);

    std::set<std::string_view> names;
    for (const OutputSpec& output : kOutputs)
    {
        CHECK(output.bytes > 0);
        names.insert(output.name);
    }
    CHECK(names.size() == kOutputCount);
    CHECK(kReadbackBytes == 7'680);
    CHECK(textureSpec(FrameTexture::Normal).format == Format::RGBA16Float);
    CHECK(textureSpec(FrameTexture::Shadow).layers == 3);
    CHECK(textureSpec(FrameTexture::Bloom1).extent.height == 270);
}

//======================================================================================================================
TEST_CASE("the records dump holds each view's count and exactly the records it covers", "[host]")
{
    std::vector<std::byte> buffer(kRecordsBytes, std::byte{ 0xEE });
    const std::vector<DrawIndexedIndirectArgs> records = cullRecords();
    for (uint32_t view = 0; view < kViewCount; ++view)
    {
        std::memcpy(buffer.data() + viewCountOffset(view), &kKeptPerView, sizeof(uint32_t));
        std::memcpy(buffer.data() + viewRecordsOffset(view), records.data(), records.size() * sizeof(DrawIndexedIndirectArgs));
    }
    const std::vector<std::byte> dump = packRecords(buffer);
    std::vector<std::byte> expected;
    for (uint32_t view = 0; view < kViewCount; ++view)
    {
        const std::byte* count = reinterpret_cast<const std::byte*>(&kKeptPerView);
        expected.insert(expected.end(), count, count + sizeof(uint32_t));
        const std::byte* first = reinterpret_cast<const std::byte*>(records.data());
        expected.insert(expected.end(), first, first + records.size() * sizeof(DrawIndexedIndirectArgs));
    }
    CHECK(dump.size() == kRecordsDumpBytes);
    CHECK(dump == expected);

    // A count above the capacity is clamped, never read past the view.
    const uint32_t huge = 0xFFFFFFFFu;
    std::memcpy(buffer.data() + viewCountOffset(0), &huge, sizeof(huge));
    CHECK(packRecords(buffer).size() == kRecordsDumpBytes + (kRecordsPerView - kKeptPerView) * sizeof(DrawIndexedIndirectArgs));
}

//======================================================================================================================
TEST_CASE("texture readback pitches are multiples of 256 and repacking drops the padding", "[host]")
{
    for (uint32_t index = 0; index < kTextureCount; ++index)
    {
        const FrameTexture texture = static_cast<FrameTexture>(index);
        CHECK(dumpRowPitch(texture) % 256 == 0);
        CHECK(dumpRowPitch(texture) >= textureSpec(texture).extent.width * textureSpec(texture).bytesPerTexel);
    }
    CHECK(dumpRowPitch(FrameTexture::Ao) == 2048);
    CHECK(dumpRowPitch(FrameTexture::AoRaw) == 2048);
    CHECK(dumpRowPitch(FrameTexture::Albedo) == 7680);

    std::vector<std::byte> padded(3 * 8, std::byte{ 0xEE });
    for (uint32_t row = 0; row < 3; ++row)
    {
        std::fill_n(padded.begin() + row * 8, 5, static_cast<std::byte>(row + 1));
    }
    const std::vector<std::byte> packed = repackRows(padded, 5, 8, 3);
    REQUIRE(packed.size() == 15);
    CHECK(packed[0] == std::byte{ 1 });
    CHECK(packed[5] == std::byte{ 2 });
    CHECK(packed[14] == std::byte{ 3 });
    CHECK(std::none_of(packed.begin(), packed.end(), [](std::byte b) { return b == std::byte{ 0xEE }; }));
}

//======================================================================================================================
TEST_CASE("the render pass table fixes targets, depth state, blending and rectangles", "[host]")
{
    CHECK(kRenderPasses.size() == 10);
    for (const RenderPassSpec& pass : kRenderPasses)
    {
        CAPTURE(kPasses[pass.pass].label);
        REQUIRE(pass.pass < kPassCount);
        CHECK(pass.scissor.width == pass.viewport.width);
        CHECK(pass.scissor.height == pass.viewport.height);
        CHECK(pass.hasDepth == pass.depthStencil.depthTest);
        if (pass.hasDepth)
        {
            CHECK(pass.depthStencil.depthCompare == CompareOp::Greater);
        }
        for (uint32_t color = 0; color < pass.colorCount; ++color)
        {
            const TextureSpec& target = textureSpec(pass.colors[color].texture);
            CHECK(target.extent.width == pass.viewport.width);
            CHECK(target.extent.height == pass.viewport.height);
        }
        if (pass.hasDepth)
        {
            const TextureSpec& target = textureSpec(pass.depth.texture);
            CHECK(target.extent.width == pass.viewport.width);
        }
    }
    const auto byLabel = [](std::string_view label) -> const RenderPassSpec& {
        for (const RenderPassSpec& pass : kRenderPasses)
        {
            if (kPasses[pass.pass].label == label)
            {
                return pass;
            }
        }
        FAIL("no render pass " << label);
        return kRenderPasses[0];
    };
    CHECK(byLabel("geometry").depthStencil.depthWrite);
    CHECK(byLabel("geometry").depth.load == LoadOp::Clear);
    CHECK(byLabel("forward").depth.load == LoadOp::Load);
    CHECK(!byLabel("forward").depthStencil.depthWrite);
    CHECK(byLabel("forward").blend.srcColor == BlendFactor::SrcAlpha);
    CHECK(byLabel("bloomUp0").blend.dstColor == BlendFactor::One);
    CHECK(byLabel("bloomUp0").colors[0].load == LoadOp::Load);
    CHECK(byLabel("shadow2").depth.layer == 2);
    CHECK(byLabel("toneMap").colors[0].texture == FrameTexture::Ldr);
}

//======================================================================================================================
TEST_CASE("the composed frame is registered with arms R and N and no parameters", "[host]")
{
    const WorkloadEntry* entry = find("frame");
    REQUIRE(entry != nullptr);
    CHECK(entry->config.workload == "frame");
    CHECK(entry->config.arms == std::vector<Arm>{ Arm::Rojo, Arm::Native });
    CHECK(!entry->config.k.has_value());
    CHECK(entry->fixed == nullptr);
}

#if defined(ROJORHI_BENCH_BACKEND_NULL)
namespace {
    //==================================================================================================================
    // Three warm-up frames open each frame slot once, so the first measured frame reuses a slot whose lists and
    // arena pages already exist.
    RunOptions frameOptions(bool validation, bool trace)
    {
        RunOptions options = runOptions(Mode::Correctness);
        options.warmupFrames = 3;
        options.measuredFrames = 2;
        options.validation = validation;
        options.trace = trace;
        return options;
    }

    //==================================================================================================================
    ArmResult runRojoFrameArm(const RunOptions& options)
    {
        const WorkloadEntry* entry = find("frame");
        REQUIRE(entry != nullptr);
        ArmResult result = entry->runner(Arm::Rojo)(entry->config, options);
        INFO(result.error);
        REQUIRE(result.ok);
        REQUIRE(result.nativeCalls.size() == options.measuredFrames);
        return result;
    }

    //==================================================================================================================
    void checkEveryCounter(const Counters& measured, const Counters& expected)
    {
        for (const CounterField& field : kCounterFields)
        {
            CAPTURE(field.name);
            CHECK(measured.*field.member == expected.*field.member);
        }
    }

    //==================================================================================================================
    // One measured frame on Null, by the counter table of the Null backend's edges page. Every list, pass, barrier and
    // submission was made in an earlier frame of the same slot, so nothing is created, allocated or grown.
    //
    // nativeRecordCalls, one per state set, draw, dispatch, copy, pass begin and end, scope begin and end, and barrier
    // call (uploadFrameData and end make none):
    //   cull       beginScope, setPipeline, setRoot, dispatch, endScope                                   5
    //   gbuffer    barrier, 7 for the geometry pass (begin, pipeline, viewport, scissor, root, draw, end),
    //              the closing barrier                                                                      9
    //   ao         barrier, beginScope, 2 x (setPipeline, setRoot, dispatch), the aoRaw barrier, endScope,
    //              the closing barrier                                                                     11
    //   shadows    barrier, 3 x 7 for the cascade passes                                                  22
    //   lighting   lighting 6 (barrier, beginScope, setPipeline, setRoot, dispatch, endScope); forward 1006
    //              (barrier, begin, pipeline, viewport, scissor, 500 x (setRoot, drawIndexed), end); five
    //              full-screen passes of 8 (barrier, begin, pipeline, viewport, scissor, root, draw, end);
    //              readback 4 (barrier, beginScope, copyTextureToBuffer, endScope)                     1056
    //   total                                                                                           1103
    // nativeSubmitCalls: five submits, and one per fence wait or signal: 1 (cull signals), 2 (gbuffer waits and
    // signals), 2 (ao waits and signals), 0 (shadows), 1 (lighting waits): 11.
    // frameDataUploads and frameArenaBytes, one per pushRoot plus the camera upload, unpadded: cull 80; gbuffer 80; ao
    // 32 + 16; shadows 3 x 80; lighting 320 + 64 + 500 x 96 + 5 x 16. 514 uploads of 48,912 bytes. Scope labels count
    // in neither.
    // textureBarriers, the records of each batch: gbuffer 4 + 4; ao 2 + 1 + 1; shadows 1; lighting 2 + 2 + 2 + 2 + 2 +
    // 2 + 2 + 1: 28. nativeBarriers, one per barrier call: 2 + 3 + 1 + 8 = 14.
    // encodersOpened, one per beginRenderPass: geometry, three cascades, forward, two bloom down, two bloom up, tone
    // map: 10.
    Counters frameCounters()
    {
        return Counters{ .nativeRecordCalls = 1103,
                         .nativeSubmitCalls = 11,
                         .frameDataUploads = 514,
                         .frameArenaBytes = 48'912,
                         .textureBarriers = 28,
                         .nativeBarriers = 14,
                         .encodersOpened = 10 };
    }

    //==================================================================================================================
    // Appends one row of the call table, writing pushRoot's two calls for each "pushRoot".
    void appendRow(std::vector<std::string_view>& calls, std::initializer_list<std::string_view> row)
    {
        for (const std::string_view call : row)
        {
            if (call == "pushRoot")
            {
                calls.insert(calls.end(), { "uploadFrameData", "setRoot" });
            }
            else
            {
                calls.push_back(call);
            }
        }
    }

    //==================================================================================================================
    // One frame's RojoRHI calls, one row per pass of the pre-registration's table, in recording order.
    std::vector<std::string_view> frameCalls()
    {
        std::vector<std::string_view> calls;
        const auto row = [&calls](std::initializer_list<std::string_view> names) { appendRow(calls, names); };
        const std::initializer_list<std::string_view> fullScreen{ "barrier",    "beginRenderPass", "setPipeline", "setViewport",
                                                                  "setScissor", "pushRoot",        "draw",        "endRenderPass" };
        const std::initializer_list<std::string_view> cascade{ "beginRenderPass", "setPipeline",         "setViewport",  "setScissor",
                                                               "pushRoot",        "drawIndexedIndirect", "endRenderPass" };

        row({ "beginFrame", "acquireCommandList", "acquireCommandList", "acquireCommandList", "acquireCommandList", "acquireCommandList" });
        // cull (Compute)
        row({ "beginScope", "setPipeline", "pushRoot", "dispatch", "endScope" }); // 1 cull
        row({ "end" });
        // gbuffer (Graphics)
        row({ "barrier", "beginRenderPass", "setPipeline", "setViewport", "setScissor", "pushRoot", "drawIndexedIndirect", "endRenderPass" }); // 2 geometry
        row({ "barrier", "end" });                                                                                                             // closing batch
        // ao (Compute)
        row({ "barrier", "beginScope", "setPipeline", "pushRoot", "dispatch", "barrier", "setPipeline", "pushRoot", "dispatch", "endScope" }); // 3 ao
        row({ "barrier", "end" });                                                                                                             // closing batch
        // shadows (Graphics)
        row({ "barrier" }); // before shadow0 only
        row(cascade);       // 4 shadow0
        row(cascade);       // 5 shadow1
        row(cascade);       // 6 shadow2
        row({ "end" });
        // lighting (Graphics)
        row({ "barrier", "beginScope", "setPipeline", "pushRoot", "dispatch", "endScope" }); // 7 lighting
        row({ "uploadFrameData" });                                                          // the camera, read by every forward box
        row({ "barrier", "beginRenderPass", "setPipeline", "setViewport", "setScissor" });   // 8 forward
        for (uint32_t box = 0; box < kForwardDraws; ++box)
        {
            row({ "pushRoot", "drawIndexed" });
        }
        row({ "endRenderPass" });
        row(fullScreen);                                                     // 9 bloomDown0
        row(fullScreen);                                                     // 10 bloomDown1
        row(fullScreen);                                                     // 11 bloomUp1
        row(fullScreen);                                                     // 12 bloomUp0
        row(fullScreen);                                                     // 13 toneMap
        row({ "barrier", "beginScope", "copyTextureToBuffer", "endScope" }); // 14 readback
        row({ "end" });
        // Submissions 1 to 5
        row({ "submit", "submit", "submit", "submit", "submit", "endFrame" });
        return calls;
    }

    // One texture barrier record of the frame as the pass table writes it, in the order the barrier calls record them.
    struct ExpectedBarrier
    {
        uint32_t call; ///< The barrier call of the frame, counted from 0.
        FrameTexture texture;
        Layout oldLayout;
        Layout newLayout;
        Stage srcStage;
        Access srcAccess;
        Stage dstStage;
        Access dstAccess;
    };

    constexpr Access kDepthReadWrite = Access::DepthStencilRead | Access::DepthStencilWrite;
    constexpr Access kColorReadWrite = Access::ColorAttachmentRead | Access::ColorAttachmentWrite;

    // The pass table's batches, the closing batches of gbuffer and ao, and the barrier inside pass 3. Rows 2 and 4 end
    // at DepthStencilRead+DepthStencilWrite, since their passes test and write depth.
    constexpr std::array<ExpectedBarrier, 28> kExpectedTextureBarriers{ {
        // 2 geometry
        { 0,
          FrameTexture::Albedo,
          Layout::Undefined,
          Layout::ColorAttachment,
          Stage::ComputeShader,
          Access::None,
          Stage::ColorAttachment,
          Access::ColorAttachmentWrite },
        { 0,
          FrameTexture::Normal,
          Layout::Undefined,
          Layout::ColorAttachment,
          Stage::ComputeShader,
          Access::None,
          Stage::ColorAttachment,
          Access::ColorAttachmentWrite },
        { 0,
          FrameTexture::Material,
          Layout::Undefined,
          Layout::ColorAttachment,
          Stage::ComputeShader,
          Access::None,
          Stage::ColorAttachment,
          Access::ColorAttachmentWrite },
        { 0, FrameTexture::Depth, Layout::Undefined, Layout::DepthStencilWrite, Stage::DepthStencil, Access::None, Stage::DepthStencil, kDepthReadWrite },
        // gbuffer closing batch
        { 1,
          FrameTexture::Albedo,
          Layout::ColorAttachment,
          Layout::ShaderRead,
          Stage::ColorAttachment,
          Access::ColorAttachmentWrite,
          Stage::ComputeShader,
          Access::ShaderRead },
        { 1,
          FrameTexture::Normal,
          Layout::ColorAttachment,
          Layout::ShaderRead,
          Stage::ColorAttachment,
          Access::ColorAttachmentWrite,
          Stage::ComputeShader,
          Access::ShaderRead },
        { 1,
          FrameTexture::Material,
          Layout::ColorAttachment,
          Layout::ShaderRead,
          Stage::ColorAttachment,
          Access::ColorAttachmentWrite,
          Stage::ComputeShader,
          Access::ShaderRead },
        { 1,
          FrameTexture::Depth,
          Layout::DepthStencilWrite,
          Layout::ShaderRead,
          Stage::DepthStencil,
          Access::DepthStencilWrite,
          Stage::ComputeShader,
          Access::ShaderRead },
        // 3 ao, the barrier between its dispatches, and the ao closing batch
        { 2, FrameTexture::AoRaw, Layout::Undefined, Layout::ShaderWrite, Stage::None, Access::None, Stage::ComputeShader, Access::ShaderWrite },
        { 2, FrameTexture::Ao, Layout::Undefined, Layout::ShaderWrite, Stage::None, Access::None, Stage::ComputeShader, Access::ShaderWrite },
        { 3,
          FrameTexture::AoRaw,
          Layout::ShaderWrite,
          Layout::ShaderRead,
          Stage::ComputeShader,
          Access::ShaderWrite,
          Stage::ComputeShader,
          Access::ShaderRead },
        { 4, FrameTexture::Ao, Layout::ShaderWrite, Layout::ShaderRead, Stage::ComputeShader, Access::ShaderWrite, Stage::ComputeShader, Access::ShaderRead },
        // 4 shadow0
        { 5, FrameTexture::Shadow, Layout::Undefined, Layout::DepthStencilWrite, Stage::ComputeShader, Access::None, Stage::DepthStencil, kDepthReadWrite },
        // 7 lighting
        { 6,
          FrameTexture::Shadow,
          Layout::DepthStencilWrite,
          Layout::ShaderRead,
          Stage::DepthStencil,
          Access::DepthStencilWrite,
          Stage::ComputeShader,
          Access::ShaderRead },
        { 6, FrameTexture::Hdr, Layout::Undefined, Layout::ShaderWrite, Stage::FragmentShader, Access::None, Stage::ComputeShader, Access::ShaderWrite },
        // 8 forward
        { 7,
          FrameTexture::Hdr,
          Layout::ShaderWrite,
          Layout::ColorAttachment,
          Stage::ComputeShader,
          Access::ShaderWrite,
          Stage::ColorAttachment,
          kColorReadWrite },
        { 7,
          FrameTexture::Depth,
          Layout::ShaderRead,
          Layout::DepthStencilWrite,
          Stage::ComputeShader,
          Access::None,
          Stage::DepthStencil,
          Access::DepthStencilRead },
        // 9 bloomDown0
        { 8,
          FrameTexture::Hdr,
          Layout::ColorAttachment,
          Layout::ShaderRead,
          Stage::ColorAttachment,
          Access::ColorAttachmentWrite,
          Stage::FragmentShader,
          Access::ShaderRead },
        { 8,
          FrameTexture::Bloom0,
          Layout::Undefined,
          Layout::ColorAttachment,
          Stage::FragmentShader,
          Access::None,
          Stage::ColorAttachment,
          Access::ColorAttachmentWrite },
        // 10 bloomDown1
        { 9,
          FrameTexture::Bloom0,
          Layout::ColorAttachment,
          Layout::ShaderRead,
          Stage::ColorAttachment,
          Access::ColorAttachmentWrite,
          Stage::FragmentShader,
          Access::ShaderRead },
        { 9,
          FrameTexture::Bloom1,
          Layout::Undefined,
          Layout::ColorAttachment,
          Stage::FragmentShader,
          Access::None,
          Stage::ColorAttachment,
          Access::ColorAttachmentWrite },
        // 11 bloomUp1
        { 10,
          FrameTexture::Bloom1,
          Layout::ColorAttachment,
          Layout::ShaderRead,
          Stage::ColorAttachment,
          Access::ColorAttachmentWrite,
          Stage::FragmentShader,
          Access::ShaderRead },
        { 10, FrameTexture::Bloom0, Layout::ShaderRead, Layout::ColorAttachment, Stage::FragmentShader, Access::None, Stage::ColorAttachment, kColorReadWrite },
        // 12 bloomUp0
        { 11,
          FrameTexture::Bloom0,
          Layout::ColorAttachment,
          Layout::ShaderRead,
          Stage::ColorAttachment,
          Access::ColorAttachmentWrite,
          Stage::FragmentShader,
          Access::ShaderRead },
        { 11, FrameTexture::Hdr, Layout::ShaderRead, Layout::ColorAttachment, Stage::FragmentShader, Access::None, Stage::ColorAttachment, kColorReadWrite },
        // 13 toneMap
        { 12,
          FrameTexture::Hdr,
          Layout::ColorAttachment,
          Layout::ShaderRead,
          Stage::ColorAttachment,
          Access::ColorAttachmentWrite,
          Stage::FragmentShader,
          Access::ShaderRead },
        { 12, FrameTexture::Ldr, Layout::Undefined, Layout::ColorAttachment, Stage::Copy, Access::None, Stage::ColorAttachment, Access::ColorAttachmentWrite },
        // 14 readback
        { 13,
          FrameTexture::Ldr,
          Layout::ColorAttachment,
          Layout::CopySrc,
          Stage::ColorAttachment,
          Access::ColorAttachmentWrite,
          Stage::Copy,
          Access::CopyRead },
    } };

    //==================================================================================================================
    // Every traced texture barrier record equals its row: call, layouts, stages, accesses and the whole texture. A
    // handle is opaque, so the resource is checked by identity: each name of the table is always the same texture, and
    // two names are never the same one.
    void checkFrameBarriers(std::span<const TracedTextureBarrier> traced)
    {
        REQUIRE(traced.size() == kExpectedTextureBarriers.size());
        std::array<std::optional<uint64_t>, kTextureCount> handles{};
        for (size_t index = 0; index < traced.size(); ++index)
        {
            const ExpectedBarrier& expected = kExpectedTextureBarriers[index];
            const TextureBarrier& barrier = traced[index].barrier;
            CAPTURE(index, textureSpec(expected.texture).label);
            CHECK(traced[index].call == expected.call);
            CHECK(barrier.oldLayout == expected.oldLayout);
            CHECK(barrier.newLayout == expected.newLayout);
            CHECK(barrier.srcStage == expected.srcStage);
            CHECK(barrier.srcAccess == expected.srcAccess);
            CHECK(barrier.dstStage == expected.dstStage);
            CHECK(barrier.dstAccess == expected.dstAccess);
            CHECK(barrier.range.baseMip == 0);
            CHECK(barrier.range.mipCount == kAllMips);
            CHECK(barrier.range.baseLayer == 0);
            CHECK(barrier.range.layerCount == kAllLayers);
            std::optional<uint64_t>& handle = handles[static_cast<size_t>(expected.texture)];
            if (!handle)
            {
                handle = barrier.texture.bits;
            }
            CHECK(*handle == barrier.texture.bits);
        }
        std::set<uint64_t> distinct;
        uint32_t named = 0;
        for (const std::optional<uint64_t>& handle : handles)
        {
            if (handle)
            {
                distinct.insert(*handle);
                ++named;
            }
        }
        CHECK(named == kTextureCount);
        CHECK(distinct.size() == named);
    }

    // Collects every message RojoRHI emits while it is alive; the default sink would print them.
    class MessageCapture
    {
      public:
        MessageCapture();
        ~MessageCapture();

        std::vector<std::string> lines;
    };

    //==================================================================================================================
    void collectMessage(MessageSeverity /*severity*/, std::string_view message, void* user)
    {
        static_cast<MessageCapture*>(user)->lines.emplace_back(message);
    }

    //==================================================================================================================
    MessageCapture::MessageCapture()
    {
        setMessageCallback(collectMessage, this);
    }

    //==================================================================================================================
    MessageCapture::~MessageCapture()
    {
        setMessageCallback(nullptr, nullptr);
    }
} // namespace

//======================================================================================================================
TEST_CASE("the composed frame through RojoRHI changes every Null counter of a measured frame by its documented count", "[null]")
{
    const ArmResult result = runRojoFrameArm(frameOptions(false, false));
    REQUIRE(result.frameCounters.has_value());
    checkEveryCounter(*result.frameCounters, frameCounters());
    CHECK(result.nativeCalls == std::vector<uint64_t>(2, 1103 + 11));
    CHECK(result.nativeBarriers == std::vector<uint64_t>(2, 14));
    REQUIRE(result.cpuAllocations.has_value());
    CHECK(*result.cpuAllocations == std::vector<uint64_t>(2, 0));
    CHECK(result.cpuNs == std::vector<std::optional<double>>(2));
    CHECK(result.gpuNs == std::vector<std::optional<double>>(2));
}

//======================================================================================================================
TEST_CASE("one measured frame of the composed frame through RojoRHI makes exactly the pass table's calls in order", "[null]")
{
    const ArmResult result = runRojoFrameArm(frameOptions(false, true));
    const std::vector<std::string_view> expected = frameCalls();
    CHECK(result.trace.size() == expected.size());
    CHECK(result.trace == expected);
    checkFrameBarriers(tracedTextureBarriers());
}

//======================================================================================================================
TEST_CASE("the composed frame through RojoRHI creates no pipeline after its first frame", "[null]")
{
    RunOptions options = frameOptions(false, false);
    options.warmupFrames = 1;
    options.measuredFrames = 4;
    const ArmResult result = runRojoFrameArm(options);
    REQUIRE(result.countersAfterWarmup.has_value());
    REQUIRE(result.countersEnd.has_value());
    CHECK(result.countersAfterWarmup->pipelinesCreated == 10);
    CHECK(result.countersEnd->pipelinesCreated == result.countersAfterWarmup->pipelinesCreated);
    REQUIRE(result.frameCounters.has_value());
    CHECK(result.frameCounters->pipelinesCreated == 0);
}

//======================================================================================================================
TEST_CASE("the composed frame through RojoRHI dumps the fifteen registered outputs in order", "[null]")
{
    const ArmResult result = runRojoFrameArm(frameOptions(false, false));
    REQUIRE(result.outputs.size() == kOutputCount);
    for (uint32_t index = 0; index < kOutputCount; ++index)
    {
        CAPTURE(kOutputs[index].name);
        CHECK(result.outputs[index].name == kOutputs[index].name);
        // Null runs no shader, so the counts the culling kernel writes, and with them the records dump's size, are not
        // the GPU's; every other output has its fixed size.
        if (kOutputs[index].name != "records")
        {
            CHECK(result.outputs[index].bytes.size() == kOutputs[index].bytes);
        }
    }
}

//======================================================================================================================
TEST_CASE("the composed frame through RojoRHI runs on a validating Null device without a message", "[null]")
{
#if defined(NDEBUG)
    SKIP("DeviceDesc::validation exists in Debug builds only");
#endif
    const MessageCapture messages;
    // Five frames, so every slot's records and readback buffers are written again after the frame that last read
    // them, and the dump frame after them: the tracker judges every batch, the cross-queue handoffs of the submission
    // table, and the transitions into the next frame.
    const ArmResult result = runRojoFrameArm(frameOptions(true, true));
    REQUIRE(result.frameCounters.has_value());
    checkEveryCounter(*result.frameCounters, frameCounters());
    CHECK(result.trace == frameCalls());
    checkFrameBarriers(tracedTextureBarriers());
    CHECK(messages.lines.empty());
}
#endif

namespace {
    //==================================================================================================================
    // A pipeline over one vertex entry and an optional fragment entry of a library, with the given color formats.
    std::expected<MTL::RenderPipelineState*, std::string> renderPipeline(
        NativeContext& context, MTL::Library* library, std::string_view vertex, std::string_view fragment, std::initializer_list<MTL::PixelFormat> formats
    )
    {
        MTL4::RenderPipelineDescriptor* descriptor = MTL4::RenderPipelineDescriptor::alloc()->init();
        descriptor->setVertexFunctionDescriptor(context.function(library, vertex));
        if (!fragment.empty())
        {
            descriptor->setFragmentFunctionDescriptor(context.function(library, fragment));
        }
        descriptor->setInputPrimitiveTopology(MTL::PrimitiveTopologyClassTriangle);
        NS::UInteger slot = 0;
        for (const MTL::PixelFormat format : formats)
        {
            descriptor->colorAttachments()->object(slot++)->setPixelFormat(format);
        }
        auto pipeline = context.renderPipeline(descriptor, vertex);
        descriptor->release();
        return pipeline;
    }
} // namespace

//======================================================================================================================
TEST_CASE("every composed-frame shader entry builds into a native pipeline", "[native]")
{
    NativeContext context;
    REQUIRE(context.open(runOptions(Mode::Correctness)).has_value());

    const struct
    {
        const char* library;
        const char* entry;
    } kernels[] = { { "BenchCull", "cullMain" }, { "BenchAo", "aoMain" }, { "BenchAo", "aoBlurMain" }, { "BenchLighting", "lightingMain" } };
    for (const auto& kernel : kernels)
    {
        CAPTURE(kernel.entry);
        const auto library = context.library(kernel.library);
        REQUIRE(library.has_value());
        const auto pipeline = context.computePipeline(*library, kernel.entry, kernel.entry);
        INFO((pipeline.has_value() ? std::string() : pipeline.error()));
        CHECK(pipeline.has_value());
    }

    const auto gbuffer = context.library("BenchGBuffer");
    REQUIRE(gbuffer.has_value());
    const auto geometry = renderPipeline(
        context, *gbuffer, "gbufferVertex", "gbufferFragment", { MTL::PixelFormatRGBA8Unorm, MTL::PixelFormatRGBA16Float, MTL::PixelFormatRGBA8Unorm }
    );
    INFO((geometry.has_value() ? std::string() : geometry.error()));
    CHECK(geometry.has_value());

    const auto shadowLibrary = context.library("BenchShadow");
    REQUIRE(shadowLibrary.has_value());
    const auto shadow = renderPipeline(context, *shadowLibrary, "shadowVertex", "", {});
    INFO((shadow.has_value() ? std::string() : shadow.error()));
    CHECK(shadow.has_value());

    const auto forwardLibrary = context.library("BenchForward");
    REQUIRE(forwardLibrary.has_value());
    const auto forward = renderPipeline(context, *forwardLibrary, "forwardVertex", "forwardFragment", { MTL::PixelFormatRGBA16Float });
    INFO((forward.has_value() ? std::string() : forward.error()));
    CHECK(forward.has_value());

    const auto postLibrary = context.library("BenchPost");
    REQUIRE(postLibrary.has_value());
    for (const auto& [fragment, format] : { std::pair<const char*, MTL::PixelFormat>{ "bloomDownFragment", MTL::PixelFormatRGBA16Float },
                                            { "bloomUpFragment", MTL::PixelFormatRGBA16Float },
                                            { "toneMapFragment", MTL::PixelFormatRGBA8Unorm } })
    {
        CAPTURE(fragment);
        const auto post = renderPipeline(context, *postLibrary, "fullscreenVertex", fragment, { format });
        INFO((post.has_value() ? std::string() : post.error()));
        CHECK(post.has_value());
    }
}

namespace {
    //==================================================================================================================
    // The output named `name`, or null.
    const Output* findOutput(const std::vector<Output>& outputs, std::string_view name)
    {
        const auto found = std::find_if(outputs.begin(), outputs.end(), [&](const Output& output) { return output.name == name; });
        return found == outputs.end() ? nullptr : &*found;
    }

    //==================================================================================================================
    const Config& frameConfig()
    {
        static const Config config{ .id = "frame", .workload = "frame" };
        return config;
    }
} // namespace

//======================================================================================================================
TEST_CASE("the native frame lowers each barrier batch to the stage barrier the Metal mapping makes of it", "[host]")
{
    const auto lowering = [](FrameBatch batch) -> const NativeBarrier& { return nativeBarrier(batch); };

    // A batch whose source stage is None makes no barrier; a batch closing a list makes none, since no encoder follows.
    CHECK(lowering(FrameBatch::AoStart).form == NativeBarrierForm::None);
    CHECK(lowering(FrameBatch::GbufferClose).form == NativeBarrierForm::None);
    CHECK(lowering(FrameBatch::AoClose).form == NativeBarrierForm::None);

    // A source in the open encoder is the encoder-level form.
    CHECK(lowering(FrameBatch::AoRaw).form == NativeBarrierForm::Encoder);
    CHECK(metalStages(lowering(FrameBatch::AoRaw).source) == MTL::StageDispatch);

    // Every other batch is the consumer form on the encoder that runs its destination stages.
    CHECK(lowering(FrameBatch::Geometry).form == NativeBarrierForm::Queue);
    CHECK(metalStages(lowering(FrameBatch::Geometry).source) == (MTL::StageDispatch | MTL::StageFragment));
    CHECK(metalStages(lowering(FrameBatch::Geometry).destination) == MTL::StageFragment);
    CHECK(metalStages(lowering(FrameBatch::Lighting).source) == MTL::StageFragment);
    CHECK(metalStages(lowering(FrameBatch::Lighting).destination) == MTL::StageDispatch);
    CHECK(metalStages(lowering(FrameBatch::ToneMap).source) == (MTL::StageFragment | MTL::StageBlit));
    CHECK(metalStages(lowering(FrameBatch::Readback).destination) == MTL::StageBlit);

    uint32_t barriers = 0;
    for (const NativeBarrier& barrier : kFrameBarriers)
    {
        CAPTURE(static_cast<int>(barrier.batch));
        CHECK(&nativeBarrier(barrier.batch) == &barrier);
        if (barrier.source == Stage::None)
        {
            CHECK(barrier.form == NativeBarrierForm::None);
        }
        if (barrier.form != NativeBarrierForm::None)
        {
            CHECK(barrier.visibility == MTL4::VisibilityOptionDevice);
            ++barriers;
        }
    }
    // 14 batches: the 11 before a pass, the one inside pass 3 and the two closing ones; all but the batch before pass 3
    // and the two closing ones make a barrier.
    CHECK(kFrameBarriers.size() == 14);
    CHECK(barriers == 11);
}

//======================================================================================================================
TEST_CASE("the native composed frame culls 6,000 boxes per view, draws, and spans its five commits", "[native]")
{
    NativeContext context;
    REQUIRE(context.open(runOptions(Mode::Correctness)).has_value());
    NativeFrame frame(context, frameConfig());
    INFO((frame.ready().has_value() ? std::string() : frame.ready().error()));
    REQUIRE(frame.ready().has_value());

    for (uint32_t index = 0; index < 3; ++index)
    {
        CAPTURE(index);
        const std::expected<FrameResult, std::string> result = frame.frame(index);
        INFO((result.has_value() ? std::string() : result.error()));
        REQUIRE(result.has_value());
        CHECK(!result->cpuNs.has_value());
        REQUIRE(result->gpuNs.has_value());

        const std::span<const CommitSpan> commits = context.collector().commits();
        REQUIRE(commits.size() == kListCount);
        double longest = 0.0;
        for (const CommitSpan& commit : commits)
        {
            CHECK(!commit.failed);
            longest = std::max(longest, (commit.gpuEndSeconds - commit.gpuStartSeconds) * 1e9);
        }
        CHECK(*result->gpuNs >= longest);
        CHECK(longest > 0.0);
    }

    const std::expected<std::vector<Output>, std::string> outputs = frame.readback();
    INFO((outputs.has_value() ? std::string() : outputs.error()));
    REQUIRE(outputs.has_value());
    REQUIRE(outputs->size() == kOutputCount);
    for (uint32_t index = 0; index < kOutputCount; ++index)
    {
        CAPTURE(kOutputs[index].name);
        CHECK((*outputs)[index].name == kOutputs[index].name);
        CHECK((*outputs)[index].bytes.size() == kOutputs[index].bytes);
    }

    // Every view holds the culling rule's 6,000 records in record order, behind a count of 6,000.
    const Output* records = findOutput(*outputs, "records");
    REQUIRE(records != nullptr);
    REQUIRE(records->bytes.size() == kRecordsDumpBytes);
    const std::vector<DrawIndexedIndirectArgs> expected = cullRecords();
    const uint64_t viewBytes = sizeof(uint32_t) + uint64_t{ kKeptPerView } * sizeof(DrawIndexedIndirectArgs);
    for (uint32_t view = 0; view < kViewCount; ++view)
    {
        CAPTURE(view);
        const std::byte* base = records->bytes.data() + view * viewBytes;
        uint32_t count = 0;
        std::memcpy(&count, base, sizeof(count));
        CHECK(count == kKeptPerView);
        CHECK(std::memcmp(base + sizeof(uint32_t), expected.data(), expected.size() * sizeof(DrawIndexedIndirectArgs)) == 0);
    }

    // Row 540 of the tone-mapped image holds more than one color: the frame drew something.
    const Output* row = findOutput(*outputs, "readback");
    REQUIRE(row != nullptr);
    REQUIRE(row->bytes.size() == kReadbackBytes);
    std::set<uint32_t> colors;
    for (uint64_t offset = 0; offset < row->bytes.size(); offset += sizeof(uint32_t))
    {
        uint32_t texel = 0;
        std::memcpy(&texel, row->bytes.data() + offset, sizeof(texel));
        colors.insert(texel);
    }
    CHECK(colors.size() > 1);
}

//======================================================================================================================
TEST_CASE("two consecutive native composed frames write identical bytes to every output", "[native]")
{
    NativeContext context;
    REQUIRE(context.open(runOptions(Mode::Correctness)).has_value());
    NativeFrame frame(context, frameConfig());
    INFO((frame.ready().has_value() ? std::string() : frame.ready().error()));
    REQUIRE(frame.ready().has_value());

    // Frames 3 and 4 use slots 0 and 1, so the per-slot records and readback buffers differ between the two dumps.
    for (uint32_t index = 0; index < 4; ++index)
    {
        REQUIRE(frame.frame(index).has_value());
    }
    const std::expected<std::vector<Output>, std::string> first = frame.readback();
    REQUIRE(first.has_value());
    REQUIRE(frame.frame(4).has_value());
    const std::expected<std::vector<Output>, std::string> second = frame.readback();
    REQUIRE(second.has_value());

    REQUIRE(first->size() == kOutputCount);
    REQUIRE(second->size() == kOutputCount);
    for (uint32_t index = 0; index < kOutputCount; ++index)
    {
        CAPTURE((*first)[index].name);
        CHECK((*first)[index].name == (*second)[index].name);
        CHECK((*first)[index].bytes == (*second)[index].bytes);
    }
}
