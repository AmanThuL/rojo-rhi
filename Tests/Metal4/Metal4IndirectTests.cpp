//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4IndirectTests.cpp
/// @brief Tests how Metal 4 records counted draws: kernels ahead of the render encoder, splits, call counts and the list's ICBs.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/CommandLists.h"
#include "Core/Counters.h"
#include "Metal4Barriers.h"
#include "Metal4Device.h"
#include "Metal4DrawState.h"
#include "Metal4Encoders.h"
#include "Metal4Icb.h"
#include "Metal4Kernels.h"
#include "Support/Backend.h"
#include "Support/GpuKit.h"
#include "Support/Metal4/Metal4Timing.h"

#include <catch2/catch_test_macros.hpp>

#include <Metal/Metal.hpp>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <set>
#include <span>
#include <vector>

using namespace rojoRHI;

namespace {
    /// The viewport and scissor that cover the 16-texel square targets of these cases.
    constexpr Viewport kViewport{ .width = 16.0f, .height = 16.0f };
    constexpr Rect2D kScissor{ 0, 0, 16, 16 };

    //==================================================================================================================
    /// The calls a pass's render encoder makes when it opens with no barrier owed: the factory, the label when
    /// `list`'s encoders carry one, and the list's table.
    uint64_t openCalls(CommandList list)
    {
        return 2 + test::encoderLabel(list);
    }

    //==================================================================================================================
    /// The calls the compute encoder of a pass's counted draws makes when it opens with no barrier owed: the factory and the
    /// label when `list`'s encoders carry one. It runs only kernels, which bind the kernel table, so it binds no list table.
    uint64_t batchOpenCalls(CommandList list)
    {
        return 1 + test::encoderLabel(list);
    }

    /// The calls a pass's render encoder makes for a pipeline of the standard library and the viewport and scissor: the
    /// pipeline with its counter-clockwise winding, the viewport and the scissor.
    constexpr uint64_t kStateCalls = 4;

    /// The call of the begin timestamp a pass's first encoder writes right after it opens: the kernels' encoder when the pass's
    /// counted draws run first, else its render encoder.
    constexpr uint64_t kPassBegin = 1;

    /// The records each counted draw of these cases reads.
    constexpr uint32_t kRecords = 16;

    using Kind = metal4::PrefixKind;

    //==================================================================================================================
    /// The encoders of `list`.
    const metal4::Encoders& encodersOf(CommandList list)
    {
        return CommandListImpl::of(list).encoders;
    }

    //==================================================================================================================
    /// The graphics state of `list`.
    const metal4::DrawState& drawOf(CommandList list)
    {
        return CommandListImpl::of(list).draw;
    }

    //==================================================================================================================
    /// What `list` has counted so far; fails the case when a scope it recorded holds no timestamp pair, since the pinned
    /// counts include both timestamp writes.
    const core::RecordCounts& countsOf(CommandList list)
    {
        return test::timedCounts(list);
    }

    //==================================================================================================================
    /// The native calls `list` has recorded so far.
    uint64_t callsOf(CommandList list)
    {
        return countsOf(list).nativeRecordCalls;
    }

    //==================================================================================================================
    /// The backend kernels of `device`.
    const metal4::Kernels& kernelsOf(Device device)
    {
        return DeviceImpl::of(device).kernels;
    }

    //==================================================================================================================
    /// The kinds of `list`'s prefix, in order.
    std::vector<Kind> prefixKinds(CommandList list)
    {
        std::vector<Kind> kinds;
        for (const metal4::PrefixCommand& command : drawOf(list).prefix)
        {
            kinds.push_back(command.kind);
        }
        return kinds;
    }

    //==================================================================================================================
    /// The GPU address `address` names.
    uint64_t gpuAddressOf(Addr address)
    {
        return uint64_t{ address.lo } | uint64_t{ address.hi } << 32;
    }

    //==================================================================================================================
    /// Creates a vertex pipeline of the standard library over one `RGBA8Unorm` target.
    Pipeline vertexPipeline(Device device, ShaderLibrary library)
    {
        const std::array colors{ ColorTarget{ .format = Format::RGBA8Unorm } };
        return *device.createPipeline(
            GraphicsPipelineDesc{ .vertex = { library, "vsMain" }, .fragment = { library, "fsMain" }, .colors = colors, .label = "test.pipeline" }
        );
    }

    //==================================================================================================================
    /// Creates a 16-texel square `RGBA8Unorm` render target.
    Texture makeTarget(Device device)
    {
        return *device.createTexture(
            TextureDesc{
                .type = TextureType::Tex2D, .format = Format::RGBA8Unorm, .extent = { 16, 16, 1 }, .usage = TextureUsage::RenderTarget, .label = "test.target" }
        );
    }

    //==================================================================================================================
    /// A pass over `colors` that renders to the whole 16-texel square.
    RenderPassDesc passOf(std::span<const ColorAttachment> colors)
    {
        return RenderPassDesc{ .colors = colors, .area = { 0, 0, 16, 16 }, .label = "test.pass" };
    }

    /// What a case draws with: the standard library's pipeline, a target, records of each kind and an index buffer, all on
    /// one device.
    struct Fixture
    {
        //==============================================================================================================
        /// Creates the objects on `created`.
        explicit Fixture(Device created) : device(created)
        {
            const std::vector<std::byte> blob = test::standardLibrary();
            library = *device.loadShaderLibrary(blob, "test.library");
            pipeline = vertexPipeline(device, library);
            other = vertexPipeline(device, library);
            target = makeTarget(device);
            std::vector<DrawIndirectArgs> draws;
            std::vector<DrawIndexedIndirectArgs> indexedDraws;
            for (uint32_t i = 0; i < kRecords; ++i)
            {
                draws.push_back(DrawIndirectArgs{ .drawId = i, .vertexCount = 3, .instanceCount = 1 });
                indexedDraws.push_back(DrawIndexedIndirectArgs{ .drawId = i, .indexCount = 3, .instanceCount = 1 });
            }
            records = *device.createBuffer(
                BufferDesc{ .size = sizeof(DrawIndirectArgs) * kRecords, .usage = BufferUsage::IndirectArgs, .label = "test.records" },
                std::as_bytes(std::span(draws))
            );
            indexedRecords = *device.createBuffer(
                BufferDesc{ .size = sizeof(DrawIndexedIndirectArgs) * kRecords, .usage = BufferUsage::IndirectArgs, .label = "test.indexedRecords" },
                std::as_bytes(std::span(indexedDraws))
            );
            const std::array<uint32_t, 3> values{ 0, 1, 2 };
            indices = *device.createBuffer(
                BufferDesc{ .size = sizeof(values), .usage = BufferUsage::Index, .label = "test.indices" }, std::as_bytes(std::span(values))
            );
        }

        //==============================================================================================================
        /// Begins a pass over the target on `list`, cleared, with `store`, and binds the pipeline, the viewport and the scissor.
        void begin(CommandList list, StoreOp store = StoreOp::Store) const
        {
            const std::array colors{ ColorAttachment{ .texture = target, .load = LoadOp::Clear, .store = store } };
            list.beginRenderPass(passOf(colors));
            list.setPipeline(pipeline);
            list.setViewport(kViewport);
            list.setScissor(kScissor);
        }

        //==============================================================================================================
        /// Records a counted draw of `maxCount` of the fixture's records on `list`.
        void drawIndirect(CommandList list, uint32_t maxCount = kRecords) const
        {
            list.drawIndirect(Device(device).address(records), maxCount);
        }

        //==============================================================================================================
        /// Records a counted indexed draw of `maxCount` of the fixture's indexed records on `list`, with `UInt32` indices.
        void drawIndexedIndirect(CommandList list, uint32_t maxCount = kRecords) const
        {
            list.drawIndexedIndirect(indices, IndexType::UInt32, Device(device).address(indexedRecords), maxCount);
        }

        Device device;         ///< The device.
        ShaderLibrary library; ///< The standard library.
        Pipeline pipeline;     ///< A vertex pipeline over one `RGBA8Unorm` target.
        Pipeline other;        ///< A second pipeline like `pipeline`.
        Texture target;        ///< The pass's target.
        Buffer records;        ///< `kRecords` draw records of three vertices and one instance.
        Buffer indexedRecords; ///< `kRecords` indexed records of three indices and one instance.
        Buffer indices;        ///< Three `UInt32` indices.
    };

    /// The edge of the draw oracle's targets, in pixels.
    constexpr uint32_t kOracleEdge = 256;

    //==================================================================================================================
    /// The half-precision bits of `value`, which is zero or a normal half-precision value held exactly.
    uint16_t halfOf(float value)
    {
        if (value == 0.0f)
        {
            return 0;
        }
        const auto bits = std::bit_cast<uint32_t>(value);
        const uint32_t sign = (bits >> 16) & 0x8000u;
        const auto exponent = static_cast<uint32_t>(static_cast<int32_t>((bits >> 23) & 0xFFu) - 127 + 15);
        return static_cast<uint16_t>(sign | (exponent << 10) | ((bits >> 13) & 0x3FFu));
    }

    /// The draw oracle over one `kOracleEdge` square target of a format, cleared by the pass the case begins.
    struct OracleTarget
    {
        //==============================================================================================================
        /// Loads the draw oracle on `kit`'s device and creates its pipeline and target of `targetFormat`.
        OracleTarget(test::GpuKit& kit, Format targetFormat) : format(targetFormat)
        {
            const std::vector<std::byte> blob = test::shaderLibraryFile("DrawOracle");
            const ShaderLibrary library = *kit.device.loadShaderLibrary(blob, "DrawOracle");
            const std::array colors{ ColorTarget{ .format = format } };
            pipeline = *kit.device.createPipeline(
                GraphicsPipelineDesc{
                    .vertex = { library, "oracleVertexMain" }, .fragment = { library, "oracleFragmentMain" }, .colors = colors, .label = "test.oracle" }
            );
            target = *kit.device.createTexture(
                TextureDesc{ .type = TextureType::Tex2D,
                             .format = format,
                             .extent = { kOracleEdge, kOracleEdge, 1 },
                             .usage = TextureUsage::RenderTarget,
                             .label = "test.oracleTarget" }
            );
        }

        //==============================================================================================================
        /// Begins a pass on `list` that clears the target to zero, and binds the pipeline, a full viewport and a full scissor.
        void begin(CommandList list) const
        {
            const std::array colors{ ColorAttachment{ .texture = target, .load = LoadOp::Clear } };
            list.beginRenderPass(RenderPassDesc{ .colors = colors, .area = { 0, 0, kOracleEdge, kOracleEdge }, .label = "test.pass" });
            list.setPipeline(pipeline);
            list.setViewport(Viewport{ .width = static_cast<float>(kOracleEdge), .height = static_cast<float>(kOracleEdge) });
            list.setScissor(Rect2D{ 0, 0, kOracleEdge, kOracleEdge });
        }

        //==============================================================================================================
        /// Reads the target back.
        std::vector<std::byte> read(test::GpuKit& kit) const
        {
            return kit.readTexture(target, TextureRegion{ .extent = { kOracleEdge, kOracleEdge, 1 } }, format);
        }

        Format format;     ///< The target's format.
        Pipeline pipeline; ///< The oracle's vertex and fragment pipeline.
        Texture target;    ///< The target.
    };

    //==================================================================================================================
    /// Counts the texels of `size` bytes each where `actual` differs from `expected`.
    size_t texelsDiffering(const std::vector<std::byte>& actual, const std::vector<std::byte>& expected, size_t size)
    {
        REQUIRE(actual.size() == expected.size());
        size_t differing = 0;
        for (size_t at = 0; at < expected.size(); at += size)
        {
            differing += std::memcmp(&actual[at], &expected[at], size) != 0 ? 1 : 0;
        }
        return differing;
    }

    //==================================================================================================================
    /// Ends `list`, submits it to the graphics queue, waits for it on the host and ends the frame.
    void finish(Device device, CommandList list)
    {
        list.end();
        const Fence fence = *device.createFence(0, "test.done");
        const std::array<FenceValue, 1> signal{ FenceValue{ fence, 1 } };
        device.queue(QueueType::Graphics).submit(std::array{ list }, {}, signal);
        CHECK(device.wait({ fence, 1 }, UINT64_MAX));
        device.endFrame();
    }
} // namespace

//======================================================================================================================
TEST_CASE("a counted draw before the pass's first direct draw runs its kernel ahead of the render encoder", "[metal4]")
{
    test::GpuKit kit(2, true);
    const Fixture fixture(kit.device);
    CommandList list = kit.list(QueueType::Graphics, "test.indirect");
    fixture.begin(list);
    fixture.drawIndirect(list);
    // The kernel runs on a compute encoder; the execute waits in the prefix, after the state recorded before it.
    CHECK(encodersOf(list).kind() == metal4::EncoderKind::Compute);
    CHECK(encodersOf(list).kernelPipeline() == kernelsOf(kit.device).encodeDraw);
    CHECK(countsOf(list).encodersOpened == 1);
    CHECK(prefixKinds(list) == std::vector{ Kind::Pipeline, Kind::Viewport, Kind::Scissor, Kind::Execute });
    // The render encoder owes a barrier from the kernel's dispatch stage to the vertex stage, where the execute reads.
    CHECK((encodersOf(list).barriers().pendingSource & MTL::StageDispatch) != 0);
    CHECK((encodersOf(list).barriers().pendingDestination & MTL::StageVertex) != 0);
    const uint64_t barriers = countsOf(list).nativeBarriers;
    list.draw(3);
    CHECK(encodersOf(list).kind() == metal4::EncoderKind::Render);
    CHECK(countsOf(list).encodersOpened == 2);
    // The render encoder's opening barrier waits for the kernel's dispatch stage.
    CHECK(countsOf(list).nativeBarriers == barriers + 1);
    CHECK((encodersOf(list).barriers().openSource & MTL::StageDispatch) != 0);
    CHECK((encodersOf(list).barriers().openDestination & MTL::StageVertex) != 0);
    CHECK(encodersOf(list).barriers().pendingDestination == 0);
    CHECK(drawOf(list).prefix.empty());
    list.endRenderPass();
    // The execute ran in the stages its barrier covered, so nothing is owed after the pass.
    CHECK(encodersOf(list).barriers().pendingDestination == 0);
    CHECK(countsOf(list).encodersOpened == 2);
    kit.run(std::array{ list });
}

//======================================================================================================================
TEST_CASE("the kernels of a pass's leading counted draws share one compute encoder", "[metal4]")
{
    test::GpuKit kit(2, true);
    const Fixture fixture(kit.device);
    CommandList list = kit.list(QueueType::Graphics, "test.indirect");
    fixture.begin(list);
    fixture.drawIndirect(list);
    fixture.drawIndexedIndirect(list);
    fixture.drawIndirect(list, 1);
    fixture.drawIndexedIndirect(list, 1);
    CHECK(countsOf(list).encodersOpened == 1);
    CHECK(
        prefixKinds(list) ==
        std::vector{ Kind::Pipeline, Kind::Viewport, Kind::Scissor, Kind::Execute, Kind::Execute, Kind::DrawIndirect, Kind::IndexedIndirect }
    );
    // A pass whose only commands are counted draws opens its render encoder at its end, which replays the executes.
    list.endRenderPass();
    CHECK(countsOf(list).encodersOpened == 2);
    CHECK(drawOf(list).prefix.empty());
    kit.run(std::array{ list });
}

//======================================================================================================================
TEST_CASE("a counted draw after a direct draw splits the pass", "[metal4]")
{
    test::GpuKit kit(2, true);
    const Fixture fixture(kit.device);
    CommandList list = kit.list(QueueType::Graphics, "test.indirect");
    // Not stored at the pass's end, so the split's Store is a call.
    fixture.begin(list, StoreOp::DontCare);
    list.draw(3);
    REQUIRE(encodersOf(list).kind() == metal4::EncoderKind::Render);
    const uint64_t before = callsOf(list);
    fixture.drawIndirect(list);
    // The split stores the attachment and ends the render encoder; then the kernel's encoder opens and the kernel runs.
    CHECK(callsOf(list) - before == 2 + batchOpenCalls(list) + 4);
    CHECK(encodersOf(list).kind() == metal4::EncoderKind::Compute);
    CHECK(drawOf(list).resuming);
    CHECK(countsOf(list).encodersOpened == 2);
    list.endRenderPass();
    CHECK(countsOf(list).encodersOpened == 3);
    kit.run(std::array{ list });
}

//======================================================================================================================
TEST_CASE("a split orders the ended encoder's stores before the resumed encoder's loads", "[metal4]")
{
    test::GpuKit kit(2, true);
    const Fixture fixture(kit.device);
    CommandList list = kit.list(QueueType::Graphics, "test.indirect");
    fixture.begin(list);
    fixture.drawIndirect(list);
    list.draw(3);
    // No split yet: the first render encoder waits for the kernel alone.
    CHECK((encodersOf(list).barriers().openSource & MTL::StageFragment) == 0);
    fixture.drawIndirect(list);
    REQUIRE(drawOf(list).resuming);
    const uint64_t barriers = countsOf(list).nativeBarriers;
    list.draw(3);
    // The resumed encoder's one opening barrier also waits for the fragment stage, where the ended encoder stored what this one
    // loads, and holds back its own fragment stage, where the load runs.
    CHECK(countsOf(list).nativeBarriers == barriers + 1);
    CHECK((encodersOf(list).barriers().openSource & (MTL::StageDispatch | MTL::StageFragment)) == (MTL::StageDispatch | MTL::StageFragment));
    CHECK((encodersOf(list).barriers().openDestination & (MTL::StageVertex | MTL::StageFragment)) == (MTL::StageVertex | MTL::StageFragment));
    list.endRenderPass();
    CHECK(encodersOf(list).barriers().pendingDestination == 0);
    kit.run(std::array{ list });
}

//======================================================================================================================
TEST_CASE("a kernel makes four calls on a new encoder, two once the encoder holds its pipeline, and its execute one", "[metal4]")
{
    test::GpuKit kit(2, true);
    const Fixture fixture(kit.device);
    CommandList list = kit.list(QueueType::Graphics, "test.indirect");
    fixture.begin(list);
    uint64_t before = callsOf(list);
    // The encoder's open, the pass's begin timestamp, then the pipeline, the kernel table, the parameters and the dispatch.
    fixture.drawIndirect(list);
    CHECK(callsOf(list) - before == batchOpenCalls(list) + kPassBegin + 4);
    // The encoder holds the pipeline and the kernel table: the parameters and the dispatch, as the native arm's two.
    before = callsOf(list);
    fixture.drawIndirect(list);
    CHECK(callsOf(list) - before == 2);
    // Another kernel sets its pipeline.
    before = callsOf(list);
    fixture.drawIndexedIndirect(list);
    CHECK(callsOf(list) - before == 3);
    before = callsOf(list);
    fixture.drawIndexedIndirect(list);
    CHECK(callsOf(list) - before == 2);
    // The kernels' encoder ends; the render encoder's open with its barrier, the state, one execute per counted draw,
    // the pass's end timestamp, endEncoding and its group.
    before = callsOf(list);
    list.endRenderPass();
    CHECK(callsOf(list) - before == 1 + openCalls(list) + 1 + kStateCalls + 4 + 2);
    kit.run(std::array{ list });
}

//======================================================================================================================
TEST_CASE("maxCount 1 runs the writer kernel and one native indirect draw", "[metal4]")
{
    test::GpuKit kit(2, true);
    const Fixture fixture(kit.device);
    CommandList list = kit.list(QueueType::Graphics, "test.indirect");
    fixture.begin(list);
    uint64_t before = callsOf(list);
    // The encoder's open, the pass's begin timestamp and the writer kernel: no indirect command buffer.
    fixture.drawIndirect(list, 1);
    CHECK(callsOf(list) - before == batchOpenCalls(list) + kPassBegin + 4);
    CHECK(encodersOf(list).kernelPipeline() == kernelsOf(kit.device).writeDraw);
    fixture.drawIndexedIndirect(list, 1);
    CHECK(encodersOf(list).kernelPipeline() == kernelsOf(kit.device).writeIndexed);
    CHECK(drawOf(list).icbs.slots(metal4::IcbKind::Draw) == 0);
    CHECK(drawOf(list).icbs.slots(metal4::IcbKind::Indexed) == 0);
    CHECK(prefixKinds(list) == std::vector{ Kind::Pipeline, Kind::Viewport, Kind::Scissor, Kind::DrawIndirect, Kind::IndexedIndirect });
    const metal4::PrefixIndirect& indexed = drawOf(list).prefix.back().indirect;
    CHECK(indexed.primitive == MTL::PrimitiveTypeTriangle);
    CHECK(indexed.indexType == MTL::IndexTypeUInt32);
    CHECK(indexed.indices == gpuAddressOf(kit.device.address(fixture.indices)));
    CHECK(indexed.indexLength == 3 * sizeof(uint32_t));
    // The kernels' encoder ends; the render encoder's open with its barrier, the state, two native indirect draws,
    // the pass's end timestamp, endEncoding and its group.
    before = callsOf(list);
    list.endRenderPass();
    CHECK(callsOf(list) - before == 1 + openCalls(list) + 1 + kStateCalls + 2 + 2);
    kit.run(std::array{ list });
}

//======================================================================================================================
TEST_CASE("an execute replays after the root, pipeline and state recorded before it", "[metal4]")
{
    test::GpuKit kit(2, true);
    const Fixture fixture(kit.device);
    CommandList list = kit.list(QueueType::Graphics, "test.indirect");
    const std::array<std::byte, 256> zeroes{};
    const Addr first = list.uploadFrameData(zeroes, 256);
    const Addr second = list.uploadFrameData(zeroes, 256);
    fixture.begin(list);
    list.setRoot(first);
    fixture.drawIndirect(list);
    list.setPipeline(fixture.other);
    list.setRoot(second);
    list.setViewport(Viewport{ .width = 8.0f, .height = 8.0f });
    fixture.drawIndirect(list, 1);
    // No entry merges across an execute: each sees the root and the pipeline recorded before it.
    CHECK(
        prefixKinds(list) ==
        std::vector{ Kind::Pipeline, Kind::Viewport, Kind::Scissor, Kind::Root, Kind::Execute, Kind::Pipeline, Kind::Root, Kind::Viewport, Kind::DrawIndirect }
    );
    CHECK(drawOf(list).prefix[3].root == gpuAddressOf(first));
    CHECK(drawOf(list).prefix[6].root == gpuAddressOf(second));
    list.draw(3);
    // The replay leaves the encoder holding what was recorded last.
    CHECK(drawOf(list).held.pipeline == drawOf(list).pipeline);
    CHECK(drawOf(list).held.viewport.width == 8.0);
    CHECK(drawOf(list).tableRoot == gpuAddressOf(second));
    list.endRenderPass();
    kit.run(std::array{ list });
}

//======================================================================================================================
TEST_CASE("a counted draw after a scope opened in the pass splits it and the resumed encoder pushes the group again", "[metal4]")
{
    test::GpuKit kit(2, true);
    const Fixture fixture(kit.device);
    CommandList list = kit.list(QueueType::Graphics, "test.indirect");
    fixture.begin(list);
    list.beginScope("test.scope");
    REQUIRE(encodersOf(list).kind() == metal4::EncoderKind::Render);
    REQUIRE(drawOf(list).passGroups == 1);
    uint64_t before = callsOf(list);
    fixture.drawIndirect(list);
    // The split pops the group and ends the encoder (the attachment is stored at no cost); then the kernel's encoder.
    CHECK(callsOf(list) - before == 1 + 1 + batchOpenCalls(list) + 4);
    CHECK(drawOf(list).passGroups == 1);
    before = callsOf(list);
    // The scope's end ends the kernels' encoder and opens the resumed encoder with its barrier, which sets the held state
    // again and pushes the group, then replays the execute, then writes the scope's end timestamp and pops the group. The
    // split wrote no timestamp.
    list.endScope();
    CHECK(encodersOf(list).kind() == metal4::EncoderKind::Render);
    CHECK(callsOf(list) - before == 1 + openCalls(list) + 1 + kStateCalls + 1 + 1 + 2);
    CHECK(drawOf(list).passGroups == 0);
    list.endRenderPass();
    CHECK(countsOf(list).encodersOpened == 3);
    kit.run(std::array{ list });
}

//======================================================================================================================
TEST_CASE("ICBs are made once per list and reused when its slot comes round; a larger maxCount replaces one", "[metal4]")
{
    Device device = *createDevice({ .scopeTimings = true });
    const uint32_t slots = core::coreOf(device).framesInFlight();
    const Fixture fixture(device);
    std::vector<const MTL::IndirectCommandBuffer*> first;
    for (uint32_t frame = 0; frame < 2 * slots; ++frame)
    {
        device.beginFrame();
        const Counters before = device.counters();
        CommandList list = device.acquireCommandList(QueueType::Graphics, "test.indirect");
        fixture.begin(list);
        fixture.drawIndirect(list);
        fixture.drawIndirect(list);
        fixture.drawIndexedIndirect(list);
        list.endRenderPass();
        // The pinned creations include the counter heap the pass's timestamps need.
        test::requireTimed(list);
        const Counters after = device.counters();
        // A new list makes its four objects, its first frame-arena page and three indirect command buffers, and the pass's
        // timestamps the frame slot's first counter heap; a reused list and slot make none.
        CHECK(after.nativeCreateCalls - before.nativeCreateCalls == (frame < slots ? 4 + 1 + 3 + 1 : 0));
        const metal4::IcbPool& icbs = drawOf(list).icbs;
        REQUIRE(icbs.slots(metal4::IcbKind::Draw) == 2);
        REQUIRE(icbs.slots(metal4::IcbKind::Indexed) == 1);
        CHECK(icbs.capacity(metal4::IcbKind::Draw, 0) == metal4::kIcbMinimumCommands);
        if (frame < slots)
        {
            first.push_back(icbs.buffer(metal4::IcbKind::Draw, 0));
        }
        else
        {
            CHECK(icbs.buffer(metal4::IcbKind::Draw, 0) == first[frame - slots]);
        }
        finish(device, list);
    }
    // A counted draw of more records than the slot holds replaces its buffer with one of the next power of two.
    std::vector<DrawIndirectArgs> many(100, DrawIndirectArgs{ .vertexCount = 3, .instanceCount = 1 });
    const Buffer manyRecords = *device.createBuffer(
        BufferDesc{ .size = sizeof(DrawIndirectArgs) * many.size(), .usage = BufferUsage::IndirectArgs, .label = "test.many" }, std::as_bytes(std::span(many))
    );
    device.beginFrame();
    const Counters before = device.counters();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.indirect");
    fixture.begin(list);
    fixture.drawIndirect(list);
    list.drawIndirect(device.address(manyRecords), static_cast<uint32_t>(many.size()));
    list.endRenderPass();
    const Counters after = device.counters();
    CHECK(after.nativeCreateCalls - before.nativeCreateCalls == 1);
    CHECK(drawOf(list).icbs.capacity(metal4::IcbKind::Draw, 0) == metal4::kIcbMinimumCommands);
    CHECK(drawOf(list).icbs.capacity(metal4::IcbKind::Draw, 1) == 128);
    finish(device, list);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a list's indirect command buffers join the device's residency set at submission, and a replaced one leaves it", "[metal4]")
{
    Device device = *createDevice({ .scopeTimings = true });
    const uint32_t slots = core::coreOf(device).framesInFlight();
    const Fixture fixture(device);
    metal4::Residency& residency = DeviceImpl::of(device).residency;
    std::vector<const MTL::IndirectCommandBuffer*> made;
    for (uint32_t frame = 0; frame < 2 * slots; ++frame)
    {
        device.beginFrame();
        CommandList list = device.acquireCommandList(QueueType::Graphics, "test.indirect");
        fixture.begin(list);
        fixture.drawIndirect(list);
        // A later counted draw of the recording sends its parameters and its dispatch, and no residency set.
        const uint64_t calls = callsOf(list);
        fixture.drawIndirect(list);
        CHECK(callsOf(list) - calls == 2);
        list.endRenderPass();
        const metal4::IcbPool& icbs = drawOf(list).icbs;
        REQUIRE(icbs.slots(metal4::IcbKind::Draw) == 2);
        const MTL::IndirectCommandBuffer* buffer = icbs.buffer(metal4::IcbKind::Draw, 0);
        // end commits nothing; the submission adds a new list's buffers to the device's set and commits it, once.
        const Counters before = device.counters();
        list.end();
        CHECK(device.counters().nativeHostCalls == before.nativeHostCalls);
        const Fence fence = *device.createFence(0, "test.done");
        const Counters submitted = device.counters();
        device.queue(QueueType::Graphics).submit(std::array{ list }, {}, std::array{ FenceValue{ fence, 1 } });
        CHECK(device.counters().nativeHostCalls - submitted.nativeHostCalls == (frame < slots ? 1 : 0));
        CHECK(residency.contains(buffer));
        CHECK(residency.contains(icbs.buffer(metal4::IcbKind::Draw, 1)));
        if (frame < slots)
        {
            made.push_back(buffer);
        }
        CHECK(device.wait({ fence, 1 }, UINT64_MAX));
        device.endFrame();
    }
    // A counted draw of more records than a slot holds replaces its buffer; the replaced one leaves the set at the list's
    // next submission, its last use having retired before the list was acquired again.
    std::vector<DrawIndirectArgs> many(100, DrawIndirectArgs{ .vertexCount = 3, .instanceCount = 1 });
    const Buffer manyRecords = *device.createBuffer(
        BufferDesc{ .size = sizeof(DrawIndirectArgs) * many.size(), .usage = BufferUsage::IndirectArgs, .label = "test.many" }, std::as_bytes(std::span(many))
    );
    // The frame after the loop reuses the slot, and so the list, of the loop's first frame. The case keeps a reference to
    // the buffer that will be replaced, which the hand-over releases.
    MTL::IndirectCommandBuffer* replaced = const_cast<MTL::IndirectCommandBuffer*>(made.front())->retain();
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.indirect");
    fixture.begin(list);
    list.drawIndirect(device.address(manyRecords), static_cast<uint32_t>(many.size()));
    list.endRenderPass();
    const MTL::IndirectCommandBuffer* grown = drawOf(list).icbs.buffer(metal4::IcbKind::Draw, 0);
    REQUIRE(grown != replaced);
    list.end();
    const Fence fence = *device.createFence(0, "test.done");
    device.queue(QueueType::Graphics).submit(std::array{ list }, {}, std::array{ FenceValue{ fence, 1 } });
    CHECK(residency.contains(grown));
    CHECK_FALSE(residency.contains(replaced));
    replaced->release();
    CHECK(device.wait({ fence, 1 }, UINT64_MAX));
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a counted draw that claims a new frame-arena page reads its kernel's parameters from that page", "[metal4]")
{
    test::GpuKit kit(2, true);
    const OracleTarget oracle(kit, Format::R32Float);
    // Each counted draw reserves 96 bytes of the list's 1 MiB page, so these claim a second page part way through. Each draws
    // its own drawId into its own pixel; one that read its parameters at the old page's address would draw another's.
    constexpr uint32_t kDraws = 12000;
    std::vector<DrawIndirectArgs> records;
    for (uint32_t i = 0; i < kDraws; ++i)
    {
        records.push_back(DrawIndirectArgs{ .drawId = i, .vertexCount = 3, .instanceCount = 1 });
    }
    const Buffer args = kit.buffer(sizeof(DrawIndirectArgs) * kDraws, Memory::Gpu, BufferUsage::IndirectArgs, std::as_bytes(std::span(records)));
    CommandList list = kit.list(QueueType::Graphics, "test.indirect");
    oracle.begin(list);
    std::set<uint64_t> pages;
    for (uint32_t i = 0; i < kDraws; ++i)
    {
        list.drawIndirect(kit.device.address(args, uint64_t{ i } * sizeof(DrawIndirectArgs)), 1);
        pages.insert(core::coreOf(list).page.buffer.bits);
    }
    REQUIRE(pages.size() >= 2);
    list.endRenderPass();
    kit.run(std::array{ list });
    std::vector<float> expected(size_t{ kOracleEdge } * kOracleEdge, 0.0f);
    for (uint32_t i = 0; i < kDraws; ++i)
    {
        expected[i] = static_cast<float>(i + 1);
    }
    const std::span<const std::byte> bytes = std::as_bytes(std::span(expected));
    CHECK(texelsDiffering(oracle.read(kit), { bytes.begin(), bytes.end() }, sizeof(float)) == 0);
}

//======================================================================================================================
TEST_CASE("counted indexed draws read the index buffer each names", "[metal4]")
{
    test::GpuKit kit(2, true);
    const OracleTarget oracle(kit, Format::RGBA16Float);
    // From index 0, the first buffer draws vertices 0 to 2; from index 3, the second draws vertices 3 to 5. Either read at the
    // other's address draws a degenerate triangle, or nothing, so a stale address loses its pixels.
    const std::array<uint16_t, 3> first{ 0, 1, 2 };
    const std::array<uint16_t, 6> second{ 9, 9, 9, 3, 4, 5 };
    const Buffer firstIndices = kit.buffer(sizeof(first), Memory::Gpu, BufferUsage::Index, std::as_bytes(std::span(first)));
    const Buffer secondIndices = kit.buffer(sizeof(second), Memory::Gpu, BufferUsage::Index, std::as_bytes(std::span(second)));
    // Records of drawIds from `firstDrawId`, reading three indices from `firstIndex`.
    const auto recordsOf = [&kit](uint32_t firstDrawId, uint32_t count, uint32_t firstIndex) {
        std::vector<DrawIndexedIndirectArgs> records;
        for (uint32_t i = 0; i < count; ++i)
        {
            records.push_back(DrawIndexedIndirectArgs{ .drawId = firstDrawId + i, .indexCount = 3, .instanceCount = 1, .firstIndex = firstIndex });
        }
        return kit.buffer(sizeof(DrawIndexedIndirectArgs) * count, Memory::Gpu, BufferUsage::IndirectArgs, std::as_bytes(std::span(records)));
    };
    const Buffer loneFirst = recordsOf(0, 1, 0);
    const Buffer loneSecond = recordsOf(1, 1, 3);
    const Buffer manySecond = recordsOf(6, 4, 3);
    const Buffer manyFirst = recordsOf(2, 4, 0);
    CommandList list = kit.list(QueueType::Graphics, "test.indirect");
    oracle.begin(list);
    // The writer kernel's path, then the indirect command buffer's: each changes the index buffer once and keeps it once.
    list.drawIndexedIndirect(firstIndices, IndexType::UInt16, kit.device.address(loneFirst), 1);
    list.drawIndexedIndirect(secondIndices, IndexType::UInt16, kit.device.address(loneSecond), 1);
    list.drawIndexedIndirect(secondIndices, IndexType::UInt16, kit.device.address(manySecond), 4);
    list.drawIndexedIndirect(firstIndices, IndexType::UInt16, kit.device.address(manyFirst), 4);
    list.endRenderPass();
    kit.run(std::array{ list });
    // The oracle writes drawId + 1, instance + 1 and the triangle's first vertex + 1.
    std::vector<uint16_t> expected(size_t{ kOracleEdge } * kOracleEdge * 4, 0);
    const auto set = [&expected](uint32_t drawId, uint32_t base) {
        const std::array<uint16_t, 4> texel{ halfOf(static_cast<float>(drawId + 1)), halfOf(1.0f), halfOf(static_cast<float>(base + 1)), halfOf(1.0f) };
        std::memcpy(&expected[size_t{ drawId } * 4], texel.data(), sizeof(texel));
    };
    set(0, 0);
    set(1, 3);
    for (uint32_t i = 0; i < 4; ++i)
    {
        set(2 + i, 0);
        set(6 + i, 3);
    }
    const std::span<const std::byte> bytes = std::as_bytes(std::span(expected));
    CHECK(texelsDiffering(oracle.read(kit), { bytes.begin(), bytes.end() }, 4 * sizeof(uint16_t)) == 0);
}
