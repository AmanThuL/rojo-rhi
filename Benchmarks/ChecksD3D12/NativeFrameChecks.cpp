//----------------------------------------------------------------------------------------------------------------------
/// @file NativeFrameChecks.cpp
/// @brief Checks the composed frame's native arm on D3D12: its barrier lowering, its pipelines, its records and its outputs.
//----------------------------------------------------------------------------------------------------------------------
#include "Common/ArmResult.h"
#include "Common/Config.h"
#include "Common/FrameLayout.h"
#include "Common/RunOptions.h"
#include "Common/Scene.h"
#include "NativeD3D12/NativeContext.h"
#include "NativeD3D12/NativeFrame.h"
#include "NativeD3D12/NativeFrameBarriers.h"
#include "NativeD3D12/NativeFrameResources.h"

#include <rojoRHI/RHI.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace rojoRHI::bench;

namespace {
    // One texture barrier as chapter 09's D3D12 rows lower RojoFrame's batches, written out independently of the arm's table.
    struct ExpectedBarrier
    {
        FrameTexture texture;
        D3D12_BARRIER_SYNC syncBefore;
        D3D12_BARRIER_SYNC syncAfter;
        D3D12_BARRIER_ACCESS accessBefore;
        D3D12_BARRIER_ACCESS accessAfter;
        D3D12_BARRIER_LAYOUT layoutBefore;
        D3D12_BARRIER_LAYOUT layoutAfter;
    };

    struct ExpectedBatch
    {
        FrameBatch batch;
        std::vector<ExpectedBarrier> textures;
    };

    //==================================================================================================================
    // Every batch in recording order. Sync keeps the scopes its accesses run in; a side with no access from
    // LAYOUT_UNDEFINED keeps its stage's sync, or takes the destination's when it names no stage; a side with no access out
    // of LAYOUT_SHADER_RESOURCE names the shader reads that layout admits; Copy on a Graphics list with no access is copy,
    // compute, render-target and depth-stencil.
    std::vector<ExpectedBatch> expectedBatches()
    {
        constexpr D3D12_BARRIER_SYNC kCompute = D3D12_BARRIER_SYNC_COMPUTE_SHADING;
        constexpr D3D12_BARRIER_SYNC kPixel = D3D12_BARRIER_SYNC_PIXEL_SHADING;
        constexpr D3D12_BARRIER_SYNC kRender = D3D12_BARRIER_SYNC_RENDER_TARGET;
        constexpr D3D12_BARRIER_SYNC kDepth = D3D12_BARRIER_SYNC_DEPTH_STENCIL;
        constexpr D3D12_BARRIER_ACCESS kNone = D3D12_BARRIER_ACCESS_NO_ACCESS;
        constexpr D3D12_BARRIER_ACCESS kRt = D3D12_BARRIER_ACCESS_RENDER_TARGET;
        constexpr D3D12_BARRIER_ACCESS kSrv = D3D12_BARRIER_ACCESS_SHADER_RESOURCE;
        constexpr D3D12_BARRIER_ACCESS kUav = D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
        constexpr D3D12_BARRIER_ACCESS kDsRead = D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ;
        constexpr D3D12_BARRIER_ACCESS kDsWrite = D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE;
        constexpr D3D12_BARRIER_LAYOUT kUndefined = D3D12_BARRIER_LAYOUT_UNDEFINED;
        constexpr D3D12_BARRIER_LAYOUT kRtLayout = D3D12_BARRIER_LAYOUT_RENDER_TARGET;
        constexpr D3D12_BARRIER_LAYOUT kDsLayout = D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE;
        constexpr D3D12_BARRIER_LAYOUT kSrvLayout = D3D12_BARRIER_LAYOUT_SHADER_RESOURCE;
        constexpr D3D12_BARRIER_LAYOUT kUavLayout = D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS;

        return {
            { FrameBatch::Geometry,
              { { FrameTexture::Albedo, kCompute, kRender, kNone, kRt, kUndefined, kRtLayout },
                { FrameTexture::Normal, kCompute, kRender, kNone, kRt, kUndefined, kRtLayout },
                { FrameTexture::Material, kCompute, kRender, kNone, kRt, kUndefined, kRtLayout },
                { FrameTexture::Depth, kDepth, kDepth, kNone, kDsRead | kDsWrite, kUndefined, kDsLayout } } },
            { FrameBatch::GbufferClose,
              { { FrameTexture::Albedo, kRender, kCompute, kRt, kSrv, kRtLayout, kSrvLayout },
                { FrameTexture::Normal, kRender, kCompute, kRt, kSrv, kRtLayout, kSrvLayout },
                { FrameTexture::Material, kRender, kCompute, kRt, kSrv, kRtLayout, kSrvLayout },
                { FrameTexture::Depth, kDepth, kCompute, kDsWrite, kSrv, kDsLayout, kSrvLayout } } },
            { FrameBatch::AoStart,
              { { FrameTexture::AoRaw, kCompute, kCompute, kNone, kUav, kUndefined, kUavLayout },
                { FrameTexture::Ao, kCompute, kCompute, kNone, kUav, kUndefined, kUavLayout } } },
            { FrameBatch::AoRaw, { { FrameTexture::AoRaw, kCompute, kCompute, kUav, kSrv, kUavLayout, kSrvLayout } } },
            { FrameBatch::AoClose, { { FrameTexture::Ao, kCompute, kCompute, kUav, kSrv, kUavLayout, kSrvLayout } } },
            { FrameBatch::Shadows, { { FrameTexture::Shadow, kCompute, kDepth, kNone, kDsRead | kDsWrite, kUndefined, kDsLayout } } },
            { FrameBatch::Lighting,
              { { FrameTexture::Shadow, kDepth, kCompute, kDsWrite, kSrv, kDsLayout, kSrvLayout },
                { FrameTexture::Hdr, kPixel, kCompute, kNone, kUav, kUndefined, kUavLayout } } },
            { FrameBatch::Forward,
              { { FrameTexture::Hdr, kCompute, kRender, kUav, kRt, kUavLayout, kRtLayout },
                { FrameTexture::Depth, kCompute, kDepth, kSrv, kDsRead, kSrvLayout, kDsLayout } } },
            { FrameBatch::BloomDown0,
              { { FrameTexture::Hdr, kRender, kPixel, kRt, kSrv, kRtLayout, kSrvLayout },
                { FrameTexture::Bloom0, kPixel, kRender, kNone, kRt, kUndefined, kRtLayout } } },
            { FrameBatch::BloomDown1,
              { { FrameTexture::Bloom0, kRender, kPixel, kRt, kSrv, kRtLayout, kSrvLayout },
                { FrameTexture::Bloom1, kPixel, kRender, kNone, kRt, kUndefined, kRtLayout } } },
            { FrameBatch::BloomUp1,
              { { FrameTexture::Bloom1, kRender, kPixel, kRt, kSrv, kRtLayout, kSrvLayout },
                { FrameTexture::Bloom0, kPixel, kRender, kSrv, kRt, kSrvLayout, kRtLayout } } },
            { FrameBatch::BloomUp0,
              { { FrameTexture::Bloom0, kRender, kPixel, kRt, kSrv, kRtLayout, kSrvLayout },
                { FrameTexture::Hdr, kPixel, kRender, kSrv, kRt, kSrvLayout, kRtLayout } } },
            { FrameBatch::ToneMap,
              { { FrameTexture::Hdr, kRender, kPixel, kRt, kSrv, kRtLayout, kSrvLayout },
                { FrameTexture::Ldr, D3D12_BARRIER_SYNC_COPY | kCompute | kRender | kDepth, kRender, kNone, kRt, kUndefined, kRtLayout } } },
            { FrameBatch::Readback,
              { { FrameTexture::Ldr, kRender, D3D12_BARRIER_SYNC_COPY, kRt, D3D12_BARRIER_ACCESS_COPY_SOURCE, kRtLayout, D3D12_BARRIER_LAYOUT_COPY_SOURCE } } },
        };
    }

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
TEST_CASE("the native frame lowers each barrier batch to the enhanced barrier the D3D12 mapping makes of it", "[host]")
{
    const std::vector<ExpectedBatch> expected = expectedBatches();
    REQUIRE(expected.size() == kFrameBatchCount);
    REQUIRE(kFrameBarriers.size() == kFrameBatchCount);
    for (const ExpectedBatch& want : expected)
    {
        CAPTURE(static_cast<int>(want.batch));
        const NativeBatch& lowered = nativeBatch(want.batch);
        CHECK(lowered.batch == want.batch);
        REQUIRE(lowered.count == want.textures.size());
        for (uint32_t index = 0; index < lowered.count; ++index)
        {
            CAPTURE(index);
            const NativeTextureBarrier& row = lowered.textures[index];
            const ExpectedBarrier& wanted = want.textures[index];
            CHECK(row.texture == wanted.texture);
            CHECK(row.syncBefore == wanted.syncBefore);
            CHECK(row.syncAfter == wanted.syncAfter);
            CHECK(row.accessBefore == wanted.accessBefore);
            CHECK(row.accessAfter == wanted.accessAfter);
            CHECK(row.layoutBefore == wanted.layoutBefore);
            CHECK(row.layoutAfter == wanted.layoutAfter);

            // The native barrier covers every layer of the texture's one mip and plane, and discards out of UNDEFINED.
            const uint32_t layers = textureSpec(row.texture).layers;
            const D3D12_TEXTURE_BARRIER native = textureBarrier(row, nullptr, layers);
            CHECK(native.Subresources.NumMipLevels == 1);
            CHECK(native.Subresources.NumArraySlices == layers);
            CHECK(native.Subresources.NumPlanes == 1);
            CHECK((native.Flags == D3D12_TEXTURE_BARRIER_FLAG_DISCARD) == (row.layoutBefore == D3D12_BARRIER_LAYOUT_UNDEFINED));
        }
    }

    // Each texture's barriers chain: every one leaves the layout the frame's previous barrier of it left, and its first
    // in a frame discards whatever the frame before left.
    std::map<FrameTexture, D3D12_BARRIER_LAYOUT> layouts;
    for (const NativeBatch& batch : kFrameBarriers)
    {
        for (uint32_t index = 0; index < batch.count; ++index)
        {
            const NativeTextureBarrier& row = batch.textures[index];
            CAPTURE(static_cast<int>(batch.batch), static_cast<int>(row.texture));
            const auto previous = layouts.find(row.texture);
            if (previous == layouts.end())
            {
                CHECK(row.layoutBefore == D3D12_BARRIER_LAYOUT_UNDEFINED);
            }
            else
            {
                CHECK(row.layoutBefore == previous->second);
            }
            layouts[row.texture] = row.layoutAfter;
        }
    }
    CHECK(layouts.size() == kTextureCount);
    CHECK(frameEndLayout(FrameTexture::Albedo) == D3D12_BARRIER_LAYOUT_SHADER_RESOURCE);
    CHECK(frameEndLayout(FrameTexture::Depth) == D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE);
    CHECK(frameEndLayout(FrameTexture::Shadow) == D3D12_BARRIER_LAYOUT_SHADER_RESOURCE);
    CHECK(frameEndLayout(FrameTexture::Ldr) == D3D12_BARRIER_LAYOUT_COPY_SOURCE);
}

//======================================================================================================================
TEST_CASE("every composed-frame shader entry builds into a native D3D12 pipeline", "[native]")
{
    NativeContext context;
    const std::expected<void, std::string> opened = context.open(runOptions(Mode::Correctness));
    INFO((opened.has_value() ? std::string() : opened.error()));
    REQUIRE(opened.has_value());

    const std::expected<NativeFramePipelines, std::string> pipelines = createFramePipelines(context);
    INFO((pipelines.has_value() ? std::string() : pipelines.error()));
    REQUIRE(pipelines.has_value());
    for (ID3D12PipelineState* pipeline : { pipelines->cull,
                                           pipelines->ao,
                                           pipelines->blur,
                                           pipelines->lighting,
                                           pipelines->geometry,
                                           pipelines->shadow,
                                           pipelines->forward,
                                           pipelines->bloomDown,
                                           pipelines->bloomUp,
                                           pipelines->toneMap })
    {
        CHECK(pipeline != nullptr);
    }
}

//======================================================================================================================
TEST_CASE("the native composed frame culls 6,000 boxes per view, draws, and makes its submissions", "[native]")
{
    NativeContext context;
    REQUIRE(context.open(runOptions(Mode::Correctness)).has_value());
    NativeFrame frame(context, frameConfig());
    INFO((frame.ready().has_value() ? std::string() : frame.ready().error()));
    REQUIRE(frame.ready().has_value());

    // Per frame: five ExecuteCommandLists, the three fences' signals and waits and the frame fence's signal; one Barrier
    // per batch; and the same recorded calls in every frame. N measures no GPU span on D3D12, so none is checked.
    std::vector<uint64_t> recorded;
    for (uint32_t index = 0; index < 3; ++index)
    {
        CAPTURE(index);
        const NativeCalls before = context.calls();
        const std::expected<FrameResult, std::string> result = frame.frame(index);
        INFO((result.has_value() ? std::string() : result.error()));
        REQUIRE(result.has_value());
        CHECK(!result->cpuNs.has_value());
        const NativeCalls& after = context.calls();
        CHECK(after.submit - before.submit == kListCount + 6 + 1);
        CHECK(after.barriers - before.barriers == kFrameBatchCount);
        recorded.push_back(after.record - before.record);
    }
    CHECK(recorded == std::vector<uint64_t>(recorded.size(), recorded[0]));

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
    const std::vector<rojoRHI::DrawIndexedIndirectArgs> expected = cullRecords();
    const uint64_t viewBytes = sizeof(uint32_t) + uint64_t{ kKeptPerView } * sizeof(rojoRHI::DrawIndexedIndirectArgs);
    for (uint32_t view = 0; view < kViewCount; ++view)
    {
        CAPTURE(view);
        const std::byte* base = records->bytes.data() + view * viewBytes;
        uint32_t count = 0;
        std::memcpy(&count, base, sizeof(count));
        CHECK(count == kKeptPerView);
        CHECK(std::memcmp(base + sizeof(uint32_t), expected.data(), expected.size() * sizeof(rojoRHI::DrawIndexedIndirectArgs)) == 0);
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
    INFO((first.has_value() ? std::string() : first.error()));
    REQUIRE(first.has_value());
    REQUIRE(frame.frame(4).has_value());
    const std::expected<std::vector<Output>, std::string> second = frame.readback();
    INFO((second.has_value() ? std::string() : second.error()));
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
