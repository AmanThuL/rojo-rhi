//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4MeshIndirectTests.cpp
/// @brief Tests how Metal 4 records indirect mesh draws: the lone native draw, the writer kernel and mesh indirect command buffers.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/CommandLists.h"
#include "Core/Counters.h"
#include "Core/DeviceCore.h"
#include "Core/HandlePool.h"
#include "Metal4Barriers.h"
#include "Metal4Device.h"
#include "Metal4DrawState.h"
#include "Metal4Encoders.h"
#include "Metal4Icb.h"
#include "Metal4Kernels.h"
#include "Metal4PipelineState.h"
#include "Support/Backend.h"
#include "Support/GpuKit.h"
#include "Support/Metal4/Metal4Timing.h"

#include <catch2/catch_test_macros.hpp>

#include <Metal/Metal.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

using namespace rojoRHI;

namespace {
    constexpr Viewport kViewport{ .width = 16.0f, .height = 16.0f };
    constexpr Rect2D kScissor{ 0, 0, 16, 16 };

    // A render encoder that opens with no barrier owed: the factory and the label when the list's encoders carry one.
    uint64_t openCalls(CommandList list)
    {
        return 1 + test::encoderLabel(list);
    }

    // The list's table at the vertex and fragment stages, which a render encoder binds before its first draw that is not
    // an execute.
    constexpr uint64_t kTable = 1;

    // The compute encoder of a pass's counted draws, opening with no barrier owed: the factory and the label when the
    // list's encoders carry one.
    uint64_t batchOpenCalls(CommandList list)
    {
        return 1 + test::encoderLabel(list);
    }

    // The mesh oracle's pipeline state: the list's table at the object and mesh stages, the pipeline with its
    // counter-clockwise winding, the viewport and the scissor.
    constexpr uint64_t kStateCalls = 5;

    // A pass's first render encoder writes the begin timestamp right after it opens; the kernels' encoder of counted draws
    // ahead of it writes none.
    constexpr uint64_t kPassBegin = 1;

    constexpr uint32_t kRecords = 16;

    using Kind = metal4::PrefixKind;

    const metal4::Encoders& encodersOf(CommandList list)
    {
        return CommandListImpl::of(list).encoders;
    }

    const metal4::DrawState& drawOf(CommandList list)
    {
        return CommandListImpl::of(list).draw;
    }

    // Fails the case when a scope the list recorded holds no timestamp pair, since the pinned counts include both
    // timestamp writes.
    const core::RecordCounts& countsOf(CommandList list)
    {
        return test::timedCounts(list);
    }

    uint64_t callsOf(CommandList list)
    {
        return countsOf(list).nativeRecordCalls;
    }

    std::vector<Kind> prefixKinds(CommandList list)
    {
        std::vector<Kind> kinds;
        for (const metal4::PrefixCommand& command : drawOf(list).prefix)
        {
            kinds.push_back(command.kind);
        }
        return kinds;
    }

    uint64_t gpuAddressOf(Addr address)
    {
        return uint64_t{ address.lo } | uint64_t{ address.hi } << 32;
    }

    struct Fixture
    {
        explicit Fixture(Device created) : device(created)
        {
            // Every case records drawMeshTasksIndirect, which the core refuses on a device without it.
            REQUIRE(device.capabilities().meshTasksIndirect);
            library = *device.loadShaderLibrary(test::shaderLibraryFile("MeshOracle"), "test.meshOracle");
            standard = *device.loadShaderLibrary(test::standardLibrary(), "test.standard");
            const std::array colors{ ColorTarget{ .format = Format::RGBA8Unorm } };
            pipeline = *device.createPipeline(
                GraphicsPipelineDesc{ .task = { library, "oracleTaskMain" },
                                      .mesh = { library, "oracleMeshMain" },
                                      .fragment = { library, "meshFragmentMain" },
                                      .colors = colors,
                                      .label = "test.mesh" }
            );
            meshOnly = *device.createPipeline(
                GraphicsPipelineDesc{ .mesh = { standard, "msMain" }, .fragment = { standard, "fsMain" }, .colors = colors, .label = "test.meshOnly" }
            );
            target = *device.createTexture(
                TextureDesc{ .type = TextureType::Tex2D,
                             .format = Format::RGBA8Unorm,
                             .extent = { 16, 16, 1 },
                             .usage = TextureUsage::RenderTarget,
                             .label = "test.target" }
            );
            std::vector<DrawMeshTasksIndirectArgs> values;
            for (uint32_t i = 0; i < kRecords; ++i)
            {
                values.push_back(DrawMeshTasksIndirectArgs{ .drawId = i, .x = 1, .y = 1, .z = 1 });
            }
            records = *device.createBuffer(
                BufferDesc{ .size = sizeof(DrawMeshTasksIndirectArgs) * kRecords, .usage = BufferUsage::IndirectArgs, .label = "test.records" },
                std::as_bytes(std::span(values))
            );
            const uint32_t value = kRecords;
            count = *device.createBuffer(
                BufferDesc{ .size = sizeof(value), .usage = BufferUsage::IndirectArgs, .label = "test.count" }, std::as_bytes(std::span(&value, 1))
            );
        }

        void begin(CommandList list, Pipeline bound) const
        {
            const std::array colors{ ColorAttachment{ .texture = target, .load = LoadOp::Clear } };
            list.beginRenderPass(RenderPassDesc{ .colors = colors, .area = { 0, 0, 16, 16 }, .label = "test.pass" });
            list.setPipeline(bound);
            list.setViewport(kViewport);
            list.setScissor(kScissor);
        }

        void begin(CommandList list) const
        {
            begin(list, pipeline);
        }

        Addr record(uint32_t index = 0) const
        {
            return Device(device).address(records, uint64_t{ index } * sizeof(DrawMeshTasksIndirectArgs));
        }

        Addr counted() const
        {
            return Device(device).address(count);
        }

        Device device;
        ShaderLibrary library;
        ShaderLibrary standard;
        Pipeline pipeline;
        Pipeline meshOnly;
        Texture target;
        // `kRecords` mesh records of one group each; record `i` has drawId `i`.
        Buffer records;
        Buffer count;
    };

    metal4::MeshParams paramsOf(CommandList list, const metal4::PrefixCommand& command)
    {
        const core::ArenaPage& page = core::coreOf(list).page;
        const uint64_t offset = command.execute.range - metal4::kMeshRangeOffset - page.address;
        metal4::MeshParams params{};
        std::memcpy(&params, page.host + offset, sizeof(params));
        return params;
    }

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

TEST_CASE("a lone mesh record with a null count is one native indirect draw that binds the record and opens no compute encoder", "[metal4]")
{
    test::GpuKit kit(2, true);
    const Fixture fixture(kit.device);
    CommandList list = kit.list(QueueType::Graphics, "test.meshIndirect");
    fixture.begin(list);
    uint64_t before = callsOf(list);
    // The render encoder opens, writes the pass's begin timestamp and replays the state; then the table is bound, the record
    // is bound at buffer(3) and drawn natively.
    list.drawMeshTasksIndirect(fixture.record(), 1);
    CHECK(callsOf(list) - before == openCalls(list) + kPassBegin + kStateCalls + kTable + 2);
    CHECK(encodersOf(list).kind() == metal4::EncoderKind::Render);
    CHECK(countsOf(list).encodersOpened == 1);
    CHECK(drawOf(list).prefix.empty());
    CHECK(drawOf(list).tableDrawRecord == gpuAddressOf(fixture.record()));
    // The same record again is the draw alone; another record binds it first.
    before = callsOf(list);
    list.drawMeshTasksIndirect(fixture.record(), 1);
    CHECK(callsOf(list) - before == 1);
    before = callsOf(list);
    list.drawMeshTasksIndirect(fixture.record(3), 1);
    CHECK(callsOf(list) - before == 2);
    CHECK(drawOf(list).tableDrawRecord == gpuAddressOf(fixture.record(3)));
    // A direct mesh draw binds the zero record again.
    list.drawMeshTasks(1, 1, 1);
    CHECK(drawOf(list).tableDrawRecord == drawOf(list).zeroRecord);
    // A pipeline without a task entry draws the same way.
    list.setPipeline(fixture.meshOnly);
    list.drawMeshTasksIndirect(fixture.record(), 1);
    list.endRenderPass();
    CHECK(countsOf(list).encodersOpened == 1);
    CHECK(drawOf(list).icbs.slots(metal4::IcbKind::Mesh) == 0);
    kit.run(std::array{ list });
}

TEST_CASE("a mesh draw with a count runs the writer kernel and one native indirect draw", "[metal4]")
{
    test::GpuKit kit(2, true, false, true);
    const Fixture fixture(kit.device);
    CommandList list = kit.list(QueueType::Graphics, "test.meshIndirect");
    fixture.begin(list);
    uint64_t before = callsOf(list);
    // The encoder's open, with no timestamp; the writer kernel waits for the run it starts to end. No indirect command buffer.
    list.drawMeshTasksIndirect(fixture.record(5), 1, fixture.counted());
    CHECK(callsOf(list) - before == batchOpenCalls(list));
    CHECK(encodersOf(list).kind() == metal4::EncoderKind::Compute);
    CHECK(encodersOf(list).kernelPipeline() == nullptr);
    CHECK(drawOf(list).icbs.slots(metal4::IcbKind::Mesh) == 0);
    CHECK(prefixKinds(list) == std::vector{ Kind::Pipeline, Kind::Viewport, Kind::Scissor, Kind::MeshIndirect });
    const metal4::PrefixMesh& mesh = drawOf(list).prefix.back().mesh;
    CHECK(mesh.record == gpuAddressOf(fixture.record(5)));
    CHECK(mesh.pipeline == drawOf(list).pipeline);
    // The render encoder owes a barrier from the kernel's dispatch stage to the object and mesh stages, where the draw reads.
    const MTL::Stages meshStages = MTL::StageObject | MTL::StageMesh;
    CHECK((encodersOf(list).barriers().pendingSource & MTL::StageDispatch) != 0);
    CHECK((encodersOf(list).barriers().pendingDestination & meshStages) == meshStages);
    // The run's pipeline, kernel table, parameters and dispatch; the kernels' encoder ends; the render encoder's open with its
    // barrier, the pass's begin timestamp, the state, the table, the record's binding, the native indirect draw, the pass's
    // end timestamp and its group.
    before = callsOf(list);
    list.endRenderPass();
    CHECK(callsOf(list) - before == 4 + 1 + openCalls(list) + 1 + kPassBegin + kStateCalls + kTable + 2 + 2);
    CHECK(drawOf(list).tableDrawRecord == gpuAddressOf(fixture.record(5)));
    // The draw ran in the stages its barrier covered, so nothing is owed after the pass.
    CHECK(encodersOf(list).barriers().pendingDestination == 0);
    kit.run(std::array{ list });
}

TEST_CASE("a counted mesh draw that splits a pass leaves the resumed encoder to bind the mesh stages again", "[metal4]")
{
    test::GpuKit kit(2, true);
    const Fixture fixture(kit.device);
    CommandList list = kit.list(QueueType::Graphics, "test.meshIndirect");
    fixture.begin(list);
    // A direct mesh draw opens the render encoder, whose mesh pipeline binds the table at the object and mesh stages.
    list.drawMeshTasks(1, 1, 1);
    REQUIRE(encodersOf(list).kind() == metal4::EncoderKind::Render);
    CHECK(drawOf(list).held.meshStages);
    // The counted draw's writer kernel needs a compute encoder, so the render encoder ends and the draw waits in the prefix.
    list.drawMeshTasksIndirect(fixture.record(5), 1, fixture.counted());
    CHECK(encodersOf(list).kind() == metal4::EncoderKind::Compute);
    CHECK(prefixKinds(list) == std::vector{ Kind::MeshIndirect });
    uint64_t before = callsOf(list);
    // The writer kernel's run dispatches (pipeline, kernel table, parameters, dispatch) and the kernels' encoder ends; the
    // resumed render encoder opens with its barrier and restores the held state, the table at the object and mesh stages
    // first, as kStateCalls counts; then the table, the counted draw's record and draw, and the direct draw's zero record
    // and draw.
    list.drawMeshTasks(1, 1, 1);
    CHECK(callsOf(list) - before == 4 + 1 + openCalls(list) + 1 + kStateCalls + kTable + 2 + 2);
    CHECK(drawOf(list).held.meshStages);
    // A mesh pipeline set again on the resumed encoder sends only the pipelines.
    before = callsOf(list);
    list.setPipeline(fixture.meshOnly);
    list.setPipeline(fixture.pipeline);
    CHECK(callsOf(list) - before == 2);
    list.endRenderPass();
    kit.run(std::array{ list });
}

TEST_CASE("a mesh batch encodes an ICB whose commands bind the reserved buffers on each stage", "[metal4]")
{
    test::GpuKit kit(2, true, false, true);
    const Fixture fixture(kit.device);
    const DeviceImpl& impl = DeviceImpl::of(kit.device);
    CommandList list = kit.list(QueueType::Graphics, "test.meshIndirect");
    const std::array<std::byte, 256> zeroes{};
    const Addr root = list.uploadFrameData(zeroes, 256);
    fixture.begin(list);
    list.setRoot(root);
    uint64_t before = callsOf(list);
    // The encoder's open, then the pipeline, the kernel table, the parameters and the dispatch; no timestamp.
    list.drawMeshTasksIndirect(fixture.record(), kRecords, fixture.counted());
    CHECK(callsOf(list) - before == batchOpenCalls(list) + 4);
    CHECK(encodersOf(list).kernelPipeline() == impl.kernels.encodeMesh);
    const metal4::IcbPool& icbs = drawOf(list).icbs;
    REQUIRE(icbs.slots(metal4::IcbKind::Mesh) == 1);
    CHECK(icbs.capacity(metal4::IcbKind::Mesh, 0) == metal4::kIcbMinimumCommands);
    CHECK(icbs.buffer(metal4::IcbKind::Mesh, 0)->size() == metal4::kIcbMinimumCommands);
    CHECK(prefixKinds(list) == std::vector{ Kind::Pipeline, Kind::Viewport, Kind::Scissor, Kind::MeshExecute });
    const metal4::PrefixCommand& execute = drawOf(list).prefix.back();
    CHECK(execute.execute.buffer == icbs.buffer(metal4::IcbKind::Mesh, 0));
    // Each command binds the root recorded at the call, the view and sampler tables and its record on every stage.
    const metal4::MeshParams params = paramsOf(list, execute);
    CHECK(params.records == gpuAddressOf(fixture.record()));
    CHECK(params.count == gpuAddressOf(fixture.counted()));
    CHECK(params.maxCount == kRecords);
    CHECK(params.root == gpuAddressOf(root));
    CHECK(params.views == impl.viewTable->gpuAddress());
    CHECK(params.samplers == impl.samplerTable->gpuAddress());
    CHECK(params.objectThreads[0] == 1);
    CHECK(params.meshThreads[0] == 1);
    // A second batch shares the encoder and its kernel: the parameters and the dispatch.
    before = callsOf(list);
    list.drawMeshTasksIndirect(fixture.record(), kRecords);
    CHECK(callsOf(list) - before == 2);
    CHECK(icbs.slots(metal4::IcbKind::Mesh) == 2);
    // A pipeline without a task entry: no object threads, and the pipeline joins the prefix.
    list.setPipeline(fixture.meshOnly);
    list.drawMeshTasksIndirect(fixture.record(), 4);
    CHECK(paramsOf(list, drawOf(list).prefix.back()).objectThreads[0] == 0);
    // The kernels' encoder ends; the render encoder's open with its barrier, the pass's begin timestamp, the state,
    // each execute, the second pipeline, the pass's end timestamp and its group; the executes bind no table.
    before = callsOf(list);
    list.endRenderPass();
    CHECK(callsOf(list) - before == 1 + openCalls(list) + 1 + kPassBegin + kStateCalls + 3 + 1 + 2);
    CHECK(encodersOf(list).barriers().pendingDestination == 0);
    kit.run(std::array{ list });
}

TEST_CASE("mesh ICBs are reused across frames like the others", "[metal4]")
{
    Device device = *createDevice({ .scopeTimings = true });
    const uint32_t slots = core::coreOf(device).framesInFlight();
    const Fixture fixture(device);
    std::vector<const MTL::IndirectCommandBuffer*> first;
    for (uint32_t frame = 0; frame < 2 * slots; ++frame)
    {
        device.beginFrame();
        const Counters before = device.counters();
        CommandList list = device.acquireCommandList(QueueType::Graphics, "test.meshIndirect");
        fixture.begin(list);
        list.drawMeshTasksIndirect(fixture.record(), kRecords);
        list.drawMeshTasksIndirect(fixture.record(), kRecords, fixture.counted());
        list.endRenderPass();
        // The pinned creations include the counter heap the pass's timestamps need.
        test::requireTimed(list);
        const Counters after = device.counters();
        // A new list makes its four objects, its first frame-arena page and two indirect command buffers, and the pass's
        // timestamps the frame slot's first counter heap; a reused list and slot make none.
        CHECK(after.nativeCreateCalls - before.nativeCreateCalls == (frame < slots ? 4 + 1 + 2 + 1 : 0));
        const metal4::IcbPool& icbs = drawOf(list).icbs;
        REQUIRE(icbs.slots(metal4::IcbKind::Mesh) == 2);
        CHECK(icbs.slots(metal4::IcbKind::Draw) == 0);
        if (frame < slots)
        {
            first.push_back(icbs.buffer(metal4::IcbKind::Mesh, 0));
        }
        else
        {
            CHECK(icbs.buffer(metal4::IcbKind::Mesh, 0) == first[frame - slots]);
        }
        finish(device, list);
    }
    destroyDevice(device);
}
