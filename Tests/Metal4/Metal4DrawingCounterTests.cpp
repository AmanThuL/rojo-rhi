//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4DrawingCounterTests.cpp
/// @brief Pins the native calls per drawing operation of the Metal 4 backend, measured as the overhead note measures them.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Support/Backend.h"
#include "Support/Metal4/Metal4Timing.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <functional>
#include <span>
#include <vector>

using namespace rojoRHI;

namespace {
    /// The repetition counts of the pairs: the low and high count of the K differences.
    constexpr uint32_t kLow = 100;
    constexpr uint32_t kHigh = 1000;

    /// The edge of the targets, in texels.
    constexpr uint32_t kEdge = 256;

    /// The records a counted draw of these cases reads at most.
    constexpr uint32_t kRecords = 256;

    /// A 64-byte root block.
    struct Root
    {
        std::array<uint32_t, 16> words{};
    };

    /// What the native calls of one operation add up to, per repetition.
    struct PerOperation
    {
        uint64_t calls = 0;    ///< `nativeRecordCalls`.
        uint64_t barriers = 0; ///< `nativeBarriers`.
        uint64_t encoders = 0; ///< `encodersOpened`.
    };

    /// The commands of a list that a measure records around its repetitions.
    using Record = std::function<void(CommandList)>;

    /// What every case draws with: the standard library's pipelines, a target, a depth texture and the buffers of the
    /// operations, all on one device.
    struct Fixture
    {
        Device device = *createDevice();
        uint32_t measures = 0; ///< The measures run so far, which name each measure's list.
        ShaderLibrary library;
        Pipeline pipeline;     ///< A vertex and fragment pipeline over the target.
        Pipeline culled;       ///< The same with back-face culling and a depth test, for C3.
        Pipeline depthPlain;   ///< The same as `pipeline` over the target and the depth format, for C3.
        Pipeline computeEntry; ///< The standard compute pipeline.
        Texture target;        ///< An `RGBA8Unorm` render target.
        Texture depth;         ///< A `D32Float` depth texture.
        Buffer indices;        ///< Three `UInt16` indices.
        Buffer records;        ///< `kRecords` draw records of three vertices.
        Buffer counts;         ///< A count of `kRecords`.
        Buffer onePerCount;    ///< A count of one.

        //==============================================================================================================
        Fixture()
        {
            const std::vector<std::byte> blob = test::standardLibrary();
            library = *device.loadShaderLibrary(blob, "test.library");
            const std::array colors{ ColorTarget{ .format = Format::RGBA8Unorm } };
            const auto make = [&](RasterState raster, DepthStencilState depthStencil, Format depthFormat, const char* label) {
                return *device.createPipeline(
                    GraphicsPipelineDesc{ .vertex = { library, "vsMain" },
                                          .fragment = { library, "fsMain" },
                                          .raster = raster,
                                          .depthStencil = depthStencil,
                                          .colors = colors,
                                          .depthFormat = depthFormat,
                                          .label = label }
                );
            };
            pipeline = make({}, {}, Format::Undefined, "test.pipeline");
            depthPlain = make({}, {}, Format::D32Float, "test.plain");
            culled = make({ .cull = CullMode::Back }, { .depthTest = true, .depthWrite = true }, Format::D32Float, "test.culled");
            computeEntry = *device.createPipeline(ComputePipelineDesc{ .compute = { library, "csMain" }, .label = "test.compute" });
            target = *device.createTexture(
                TextureDesc{ .format = Format::RGBA8Unorm, .extent = { kEdge, kEdge, 1 }, .usage = TextureUsage::RenderTarget, .label = "test.target" }
            );
            depth = *device.createTexture(
                TextureDesc{ .format = Format::D32Float, .extent = { kEdge, kEdge, 1 }, .usage = TextureUsage::DepthStencil, .label = "test.depth" }
            );
            const std::array<uint16_t, 3> values{ 0, 1, 2 };
            indices = *device.createBuffer(
                BufferDesc{ .size = sizeof(values), .usage = BufferUsage::Index, .label = "test.indices" }, std::as_bytes(std::span(values))
            );
            std::vector<DrawIndirectArgs> draws;
            for (uint32_t i = 0; i < kRecords; ++i)
            {
                draws.push_back(DrawIndirectArgs{ .drawId = i, .vertexCount = 3, .instanceCount = 1 });
            }
            records = *device.createBuffer(
                BufferDesc{ .size = sizeof(DrawIndirectArgs) * kRecords, .usage = BufferUsage::IndirectArgs, .label = "test.records" },
                std::as_bytes(std::span(draws))
            );
            const std::array<uint32_t, 1> many{ kRecords };
            counts = *device.createBuffer(
                BufferDesc{ .size = sizeof(many), .usage = BufferUsage::IndirectArgs, .label = "test.counts" }, std::as_bytes(std::span(many))
            );
        }

        //==============================================================================================================
        ~Fixture()
        {
            destroyDevice(device);
        }

        //==============================================================================================================
        Fixture(const Fixture&) = delete;

        //==============================================================================================================
        Fixture& operator=(const Fixture&) = delete;

        //==============================================================================================================
        /// Begins a pass over the target (and the depth texture when `withDepth`), cleared, and sets the viewport and scissor.
        void beginPass(CommandList list, bool withDepth = false) const
        {
            const std::array colors{ ColorAttachment{ .texture = target, .load = LoadOp::Clear } };
            RenderPassDesc desc{ .colors = colors, .area = { 0, 0, kEdge, kEdge }, .label = "test.pass" };
            if (withDepth)
            {
                desc.depth = DepthAttachment{ .texture = depth, .load = LoadOp::Clear, .clearDepth = 0.0f };
            }
            list.beginRenderPass(desc);
            list.setViewport(Viewport{ .width = static_cast<float>(kEdge), .height = static_cast<float>(kEdge) });
            list.setScissor(Rect2D{ 0, 0, kEdge, kEdge });
        }

        //==============================================================================================================
        /// Runs a frame in which one list of the Graphics queue runs `open`, then `count` repetitions of `each`, then `close`,
        /// submits it and waits for it. Returns the counters' growth from before the list is acquired to after the wait.
        Counters measure(uint32_t count, const Record& open, const std::function<void(CommandList, uint32_t)>& each, const Record& close)
        {
            device.beginFrame();
            const Counters before = device.counters();
            // A label new to every measure makes every acquisition set it, so the fixed calls of a low and a high count match.
            CommandList list = device.acquireCommandList(QueueType::Graphics, std::format("test.list.{}", measures++));
            open(list);
            for (uint32_t index = 0; index < count; ++index)
            {
                each(list, index);
            }
            close(list);
            // The pinned counts include the timestamp writes of each pass and scope.
            test::requireTimed(list);
            list.end();
            const Fence fence = *device.createFence(0, "test.done");
            const std::array<FenceValue, 1> signal{ FenceValue{ fence, 1 } };
            device.queue(QueueType::Graphics).submit(std::array{ list }, {}, signal);
            REQUIRE(device.wait({ fence, 1 }, UINT64_MAX));
            const Counters after = device.counters();
            device.endFrame();
            return Counters{ .nativeRecordCalls = after.nativeRecordCalls - before.nativeRecordCalls,
                             .nativeBarriers = after.nativeBarriers - before.nativeBarriers,
                             .encodersOpened = after.encodersOpened - before.encodersOpened };
        }

        //==============================================================================================================
        /// The per-repetition growth between `kLow` and `kHigh` repetitions of `each` run between `open` and `close`.
        PerOperation perOperation(const Record& open, const std::function<void(CommandList, uint32_t)>& each, const Record& close)
        {
            const Counters low = measure(kLow, open, each, close);
            const Counters high = measure(kHigh, open, each, close);
            const auto per = [](uint64_t lowValue, uint64_t highValue) {
                const uint64_t grown = highValue - lowValue;
                REQUIRE(grown % (kHigh - kLow) == 0);
                return grown / (kHigh - kLow);
            };
            return PerOperation{ .calls = per(low.nativeRecordCalls, high.nativeRecordCalls),
                                 .barriers = per(low.nativeBarriers, high.nativeBarriers),
                                 .encoders = per(low.encodersOpened, high.encodersOpened) };
        }
    };

    //==================================================================================================================
    /// The barrier that orders one pass's writes to the target before the next pass's.
    TextureBarrier betweenPasses(Texture target)
    {
        return TextureBarrier{ .texture = target,
                               .srcStage = Stage::ColorAttachment,
                               .srcAccess = Access::ColorAttachmentWrite,
                               .dstStage = Stage::ColorAttachment,
                               .dstAccess = Access::ColorAttachmentRead | Access::ColorAttachmentWrite,
                               .oldLayout = Layout::ColorAttachment,
                               .newLayout = Layout::ColorAttachment };
    }

    //==================================================================================================================
    /// The commands of a list that do nothing.
    void nothing(CommandList)
    {
    }
} // namespace

//======================================================================================================================
TEST_CASE("C1: pushRoot of 64 bytes then draw of 3 vertices is two native calls", "[metal4]")
{
    Fixture fixture;
    const Root root;
    const PerOperation per = fixture.perOperation(
        [&](CommandList list) {
            fixture.beginPass(list);
            list.setPipeline(fixture.pipeline);
        },
        [&](CommandList list, uint32_t) {
            list.pushRoot(root);
            list.draw(3);
        },
        [](CommandList list) { list.endRenderPass(); }
    );
    // N: setAddress and drawPrimitives; the budget is N's two plus the three calls the operation makes.
    CHECK(per.calls == 2);
    CHECK(per.calls <= 2 + 3);
    CHECK(per.barriers == 0);
    CHECK(per.encoders == 0);
}

//======================================================================================================================
TEST_CASE("C2: pushRoot then drawIndexed of 3 UInt16 indices is two native calls", "[metal4]")
{
    Fixture fixture;
    const Root root;
    const PerOperation per = fixture.perOperation(
        [&](CommandList list) {
            fixture.beginPass(list);
            list.setPipeline(fixture.pipeline);
        },
        [&](CommandList list, uint32_t) {
            list.pushRoot(root);
            list.drawIndexed(fixture.indices, IndexType::UInt16, 3);
        },
        [](CommandList list) { list.endRenderPass(); }
    );
    CHECK(per.calls == 2);
    CHECK(per.calls <= 2 + 3);
    CHECK(per.barriers == 0);
    CHECK(per.encoders == 0);
}

//======================================================================================================================
TEST_CASE("C3: setPipeline alternating between two pipelines then draw is four native calls", "[metal4]")
{
    Fixture fixture;
    const PerOperation per = fixture.perOperation(
        [&](CommandList list) { fixture.beginPass(list, true); },
        [&](CommandList list, uint32_t index) {
            list.setPipeline(index % 2 == 0 ? fixture.depthPlain : fixture.culled);
            list.draw(3);
        },
        [](CommandList list) { list.endRenderPass(); }
    );
    // N: setRenderPipelineState, setCullMode, setDepthStencilState and drawPrimitives; the budget adds the two calls.
    CHECK(per.calls == 4);
    CHECK(per.calls <= 4 + 2);
    CHECK(per.barriers == 0);
    CHECK(per.encoders == 0);
}

//======================================================================================================================
TEST_CASE("C4: pushRoot then dispatch of one group is two native calls", "[metal4]")
{
    Fixture fixture;
    const Root root;
    const PerOperation per = fixture.perOperation(
        [&](CommandList list) { list.setPipeline(fixture.computeEntry); },
        [&](CommandList list, uint32_t) {
            list.pushRoot(root);
            list.dispatch(1, 1, 1);
        },
        nothing
    );
    CHECK(per.calls == 2);
    CHECK(per.calls <= 2 + 3);
    CHECK(per.barriers == 0);
    CHECK(per.encoders == 0);
}

//======================================================================================================================
TEST_CASE("C8: a pass from barrier to endRenderPass costs the registered native calls", "[metal4]")
{
    Fixture fixture;
    const uint64_t label = test::encoderLabel(fixture.device);
    const PerOperation per = fixture.perOperation(
        nothing,
        [&](CommandList list, uint32_t index) {
            if (index > 0)
            {
                list.barrier({}, {}, std::array{ betweenPasses(fixture.target) });
            }
            fixture.beginPass(list);
            list.setPipeline(fixture.pipeline);
            list.draw(3);
            list.endRenderPass();
        },
        nothing
    );
    // The registered 13: the pass's debug group pushed and popped on the command buffer and its two timestamps on the
    // encoder, the encoder's open, label, table and consumer barrier, the pipeline, viewport, scissor, the draw and
    // endEncoding. The pipeline's winding adds one call per encoder, so the pin is 14. N's 12 leaves out the argument table,
    // so the budget is N's 12 plus c (7), 19, a margin of 5.
    CHECK(per.calls == 13 + label);
    CHECK(per.calls <= 12 + 7);
    CHECK(per.encoders == 1);
    CHECK(per.barriers == 1);
}

//======================================================================================================================
TEST_CASE("C10: a counted drawIndirect of 256 records is three native calls per draw and eighteen per pass", "[metal4]")
{
    Fixture fixture;
    const uint64_t label = test::encoderLabel(fixture.device);
    const Addr records = fixture.device.address(fixture.records);
    const Addr counts = fixture.device.address(fixture.counts);
    const Record open = [&](CommandList list) {
        fixture.beginPass(list);
        list.setPipeline(fixture.pipeline);
    };
    const auto each = [&](CommandList list, uint32_t) { list.drawIndirect(records, kRecords, counts); };
    const Record close = [](CommandList list) { list.endRenderPass(); };
    const PerOperation slope = fixture.perOperation(open, each, close);
    // The setAddress of the kernel's record, its dispatch and the execute: arm E's three. The kernel table, the pipeline and
    // the residency set come once per encoder.
    CHECK(slope.calls == 3);
    CHECK(slope.barriers == 0);
    CHECK(slope.encoders == 0);
    // The per-pass constant: one counted draw in each of `K` passes costs the slope plus the constant.
    const PerOperation pass = fixture.perOperation(
        nothing,
        [&](CommandList list, uint32_t index) {
            if (index > 0)
            {
                list.barrier({}, {}, std::array{ betweenPasses(fixture.target) });
            }
            open(list);
            each(list, 0);
            close(list);
        },
        nothing
    );
    INFO("the per-pass total with one draw: calls " << pass.calls << ", barriers " << pass.barriers << ", encoders " << pass.encoders);
    // Eighteen beyond the draw's three: both encoders' opens and ends, the pass's debug group and its two timestamps, the
    // kernels' pipeline, table and residency set, the barrier that orders the kernels before the vertex stage and the render
    // encoder's state.
    CHECK(pass.calls == slope.calls + 16 + 2 * label);
    CHECK(pass.barriers == 1);
    CHECK(pass.encoders == 2);
}

//======================================================================================================================
TEST_CASE("C10 at maxCount 1 with a null count is the writer path", "[metal4]")
{
    Fixture fixture;
    const uint64_t label = test::encoderLabel(fixture.device);
    const Addr records = fixture.device.address(fixture.records);
    const PerOperation slope = fixture.perOperation(
        [&](CommandList list) {
            fixture.beginPass(list);
            list.setPipeline(fixture.pipeline);
        },
        [&](CommandList list, uint32_t) { list.drawIndirect(records, 1); },
        [](CommandList list) { list.endRenderPass(); }
    );
    CHECK(slope.calls == 3);
    CHECK(slope.barriers == 0);
    CHECK(slope.encoders == 0);
    const PerOperation pass = fixture.perOperation(
        nothing,
        [&](CommandList list, uint32_t index) {
            if (index > 0)
            {
                list.barrier({}, {}, std::array{ betweenPasses(fixture.target) });
            }
            fixture.beginPass(list);
            list.setPipeline(fixture.pipeline);
            list.drawIndirect(records, 1);
            list.endRenderPass();
        },
        nothing
    );
    INFO("the per-pass total with one draw: calls " << pass.calls << ", barriers " << pass.barriers << ", encoders " << pass.encoders);
    CHECK(pass.calls == slope.calls + 16 + 2 * label);
    CHECK(pass.barriers == 1);
    CHECK(pass.encoders == 2);
}
