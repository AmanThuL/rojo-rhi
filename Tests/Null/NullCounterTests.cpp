//----------------------------------------------------------------------------------------------------------------------
/// @file NullCounterTests.cpp
/// @brief Tests the exact counter deltas the Null backend produces for a scripted sequence of one native call per entry.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Support/Backend.h"
#include "Support/PassKit.h"
#include "Support/UpscaleKit.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

using namespace rojoRHI;

namespace {
    /// Stand-ins for native surfaces: the Null backend never dereferences them.
    std::array<int, 1> g_surface = {};

    //==================================================================================================================
    /// Returns the field-by-field difference `after - before`.
    Counters delta(const Counters& before, const Counters& after)
    {
        Counters d;
        d.nativeCreateCalls = after.nativeCreateCalls - before.nativeCreateCalls;
        d.nativeRecordCalls = after.nativeRecordCalls - before.nativeRecordCalls;
        d.nativeSubmitCalls = after.nativeSubmitCalls - before.nativeSubmitCalls;
        d.nativeHostCalls = after.nativeHostCalls - before.nativeHostCalls;
        d.cpuAllocations = after.cpuAllocations - before.cpuAllocations;
        d.gpuAllocations = after.gpuAllocations - before.gpuAllocations;
        d.frameDataUploads = after.frameDataUploads - before.frameDataUploads;
        d.frameArenaBytes = after.frameArenaBytes - before.frameArenaBytes;
        d.frameArenaGrowths = after.frameArenaGrowths - before.frameArenaGrowths;
        d.descriptorWrites = after.descriptorWrites - before.descriptorWrites;
        d.globalBarriers = after.globalBarriers - before.globalBarriers;
        d.bufferBarriers = after.bufferBarriers - before.bufferBarriers;
        d.textureBarriers = after.textureBarriers - before.textureBarriers;
        d.nativeBarriers = after.nativeBarriers - before.nativeBarriers;
        d.pipelinesCreated = after.pipelinesCreated - before.pipelinesCreated;
        d.pipelineCacheHits = after.pipelineCacheHits - before.pipelineCacheHits;
        d.pipelineCacheMisses = after.pipelineCacheMisses - before.pipelineCacheMisses;
        d.pipelineCompileNanoseconds = after.pipelineCompileNanoseconds - before.pipelineCompileNanoseconds;
        d.encodersOpened = after.encodersOpened - before.encodersOpened;
        d.deferredDestructionsRetired = after.deferredDestructionsRetired - before.deferredDestructionsRetired;
        return d;
    }
} // namespace

//======================================================================================================================
TEST_CASE("the scripted sequence changes every counter by its exact Null delta", "[null]")
{
    Device device = *createDevice(DeviceDesc{ .scopeTimings = true, .framesInFlight = 2 });
    const Counters start = device.counters();

    // Creation: a library, a buffer, two textures, a graphics pipeline and a fence.
    const std::vector<std::byte> blob = test::standardLibrary();
    const ShaderLibrary library = *device.loadShaderLibrary(blob, "counter.library");
    const Buffer buffer = *device.createBuffer(BufferDesc{ .size = 256, .memory = Memory::Gpu, .usage = BufferUsage::ShaderWrite, .label = "counter.buffer" });
    const Texture sampled = *device.createTexture(
        TextureDesc{ .format = Format::RGBA8Unorm, .extent = { 64, 64, 1 }, .mips = 3, .usage = TextureUsage::Sampled, .label = "counter.sampled" }
    );
    const Texture target = *device.createTexture(
        TextureDesc{ .format = Format::RGBA8Unorm, .extent = { 64, 64, 1 }, .usage = TextureUsage::RenderTarget, .label = "counter.target" }
    );
    const std::array<ColorTarget, 1> colors{ ColorTarget{ .format = Format::RGBA8Unorm } };
    const Pipeline pipeline = *device.createPipeline(GraphicsPipelineDesc{ .vertex = { library, "vsMain" }, .colors = colors, .label = "counter.pipeline" });
    const Fence fence = *device.createFence(0, "counter.fence");
    const Counters created = device.counters();
    {
        const Counters d = delta(start, created);
        CHECK(d.nativeCreateCalls == 6);
        CHECK(d.gpuAllocations == 3);
        CHECK(d.cpuAllocations == 0);
        CHECK(d.pipelinesCreated == 1);
        CHECK(d.pipelineCacheMisses == 1);
        CHECK(d.pipelineCacheHits == 0);
        CHECK(d.nativeRecordCalls == 0);
    }

    // Heap-slot writes: a view and a sampler created, and one of each written into a reserved range.
    const ViewIndex view = *device.createView(sampled, TextureViewDesc{});
    const SamplerIndex sampler = *device.createSampler(SamplerDesc{});
    const IndexRange views = *device.reserveViews(1);
    device.writeView(views, 0, sampled, TextureViewDesc{});
    const IndexRange samplers = *device.reserveSamplers(1);
    device.writeSampler(samplers, 0, SamplerDesc{});
    const Counters written = device.counters();
    CHECK(delta(created, written).descriptorWrites == 4);
    CHECK(delta(created, written).nativeCreateCalls == 2);

    // Recording: a pass of two draws, two barrier calls, a barrier of no records and an upload of 100 bytes.
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "counter.list");
    const std::array<ColorAttachment, 1> attachments{ ColorAttachment{ .texture = target } };
    list.beginRenderPass(RenderPassDesc{ .colors = attachments, .area = { 0, 0, 64, 64 }, .label = "counter.pass" });
    list.setPipeline(pipeline);
    list.setViewport({ .width = 64.0f, .height = 64.0f });
    list.setScissor({ 0, 0, 64, 64 });
    list.draw(3, 1, 0, 0);
    list.draw(3, 1, 0, 0);
    list.endRenderPass();
    const std::array<BufferBarrier, 1> buffers{ BufferBarrier{ .buffer = buffer, .srcStage = Stage::ComputeShader, .dstStage = Stage::All } };
    const std::array<TextureBarrier, 2> textures{
        TextureBarrier{ .texture = sampled, .range = { .baseMip = 0, .mipCount = 1 }, .dstStage = Stage::All, .newLayout = Layout::ShaderRead },
        TextureBarrier{ .texture = sampled, .range = { .baseMip = 1, .mipCount = 1 }, .dstStage = Stage::All, .newLayout = Layout::ShaderRead }
    };
    list.barrier({}, buffers, textures);
    const std::array<GlobalBarrier, 1> globals{ GlobalBarrier{ Stage::Copy, Access::CopyWrite, Stage::All, Access::ShaderRead } };
    list.barrier(globals);
    list.barrier({});
    const std::array<std::byte, 100> bytes{};
    (void)list.uploadFrameData(bytes);
    list.end();
    const Counters recorded = device.counters();
    {
        const Counters d = delta(written, recorded);
        // A new list and the arena page the pass's scope label claimed: one allocation of each kind, one native page.
        CHECK(d.cpuAllocations == 2);
        CHECK(d.gpuAllocations == 1);
        CHECK(d.nativeCreateCalls == 1);
        CHECK(d.frameArenaGrowths == 1);
        CHECK(d.nativeRecordCalls == 0);
        CHECK(d.frameDataUploads == 0);
    }

    // Submission folds the list's counts into the device: 7 pass commands and 2 native barrier commands.
    const std::array<CommandList, 1> lists{ list };
    const std::array<FenceValue, 1> signal{ FenceValue{ fence, 1 } };
    device.queue(QueueType::Graphics).submit(lists, {}, signal);
    CHECK(device.completedValue(fence) == 1);
    const Counters submitted = device.counters();
    {
        const Counters d = delta(recorded, submitted);
        CHECK(d.nativeRecordCalls == 9);
        CHECK(d.encodersOpened == 1);
        CHECK(d.frameDataUploads == 1);
        CHECK(d.frameArenaBytes == 100);
        CHECK(d.globalBarriers == 1);
        CHECK(d.bufferBarriers == 1);
        CHECK(d.textureBarriers == 2);
        CHECK(d.nativeBarriers == 2);
        CHECK(d.nativeSubmitCalls == 2);
        CHECK(d.nativeHostCalls == 1);
    }

    // Destruction is deferred: the four objects, the view and the sampler are released when the frame slot is reused, two frames later.
    device.destroy(view);
    device.destroy(sampler);
    device.release(views);
    device.release(samplers);
    device.destroy(buffer);
    device.destroy(sampled);
    device.destroy(target);
    device.destroy(pipeline);
    device.endFrame();
    CHECK(delta(submitted, device.counters()).deferredDestructionsRetired == 0);
    device.beginFrame();
    device.endFrame();
    device.beginFrame();
    const Counters retired = device.counters();
    {
        const Counters d = delta(submitted, retired);
        CHECK(d.deferredDestructionsRetired == 6);
        CHECK(d.nativeCreateCalls == 6);
        CHECK(d.nativeHostCalls == 0);
    }

    // The frame that retired held a scope, so reading its timings is one native timestamp read.
    CHECK(device.scopeTimings().size() == 1);
    CHECK(delta(retired, device.counters()).nativeHostCalls == 1);
    device.endFrame();
    device.destroy(fence);
    device.destroy(library);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a Null device without scope timings counts no timestamp read after a frame with a scope", "[null]")
{
    Device device = *createDevice(DeviceDesc{ .framesInFlight = 2 });
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "null.list");
    list.beginScope("null.scope");
    list.endScope();
    list.end();
    device.queue(QueueType::Graphics).submit(std::array{ list });
    device.endFrame();
    device.beginFrame();
    device.endFrame();
    device.beginFrame();
    const Counters retired = device.counters();
    CHECK(device.scopeTimings().empty());
    CHECK(delta(retired, device.counters()).nativeHostCalls == 0);
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a Null swapchain counts its creation, each image acquire, each present and its deferred release", "[null]")
{
    Device device = *createDevice(DeviceDesc{ .framesInFlight = 2 });
    const Counters start = device.counters();
    Swapchain swapchain = *device.createSwapchain(SwapchainDesc{ .nativeSurface = &g_surface[0] });
    const Counters created = device.counters();
    CHECK(delta(start, created).nativeCreateCalls == 1);
    CHECK(delta(start, created).cpuAllocations == 1);
    device.beginFrame();
    (void)swapchain.acquire();
    const Counters acquired = device.counters();
    CHECK(delta(created, acquired).gpuAllocations == 1);
    CHECK(delta(created, acquired).nativeCreateCalls == 0);
    device.queue(QueueType::Graphics).present(swapchain);
    CHECK(delta(acquired, device.counters()).nativeSubmitCalls == 1);
    device.destroy(swapchain);
    device.endFrame();
    const Counters destroyed = device.counters();
    device.beginFrame();
    device.endFrame();
    device.beginFrame();
    device.endFrame();
    // The released image and the swapchain are two deferred destructions; only the swapchain is a native release.
    CHECK(delta(destroyed, device.counters()).deferredDestructionsRetired == 2);
    CHECK(delta(destroyed, device.counters()).nativeCreateCalls == 1);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a Null tile mapping counts one native queue call", "[null]")
{
    Device device = *createDevice();
    const Counters before = device.counters();
    device.queue(QueueType::Graphics).mapTiles({});
    CHECK(delta(before, device.counters()).nativeSubmitCalls == 1);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a Null buffer with host memory counts its allocation", "[null]")
{
    Device device = *createDevice();
    const Counters before = device.counters();
    const Buffer buffer = *device.createBuffer(BufferDesc{ .size = 64, .memory = Memory::Upload, .label = "counter.upload" });
    CHECK(delta(before, device.counters()).cpuAllocations == 1);
    CHECK(delta(before, device.counters()).gpuAllocations == 1);
    device.destroy(buffer);
    destroyDevice(device);
}

namespace {
    /// The objects the per-entry cases record against, created before any measured command.
    struct Fixture
    {
        Buffer args;     ///< An `IndirectArgs` buffer.
        Buffer index;    ///< An `Index` buffer.
        Buffer first;    ///< A plain buffer.
        Buffer second;   ///< Another plain buffer.
        Buffer writable; ///< A `ShaderWrite` buffer.
        Texture sampled; ///< A sampled texture.
        Texture other;   ///< Another sampled texture.
        Texture color;   ///< A render target.
    };

    /// One recording entry measured on its own.
    struct EntryCase
    {
        const char* name;                                    ///< What the case records.
        QueueType type;                                      ///< The queue type of the list.
        bool inPass;                                         ///< `setup` leaves a render pass open, which the case ends.
        uint64_t calls;                                      ///< The `nativeRecordCalls` the entry adds.
        std::function<void(test::PassKit&, Fixture&)> setup; ///< Records what the entry needs, in both runs.
        std::function<void(test::PassKit&, Fixture&)> entry; ///< Records the entry, in the measured run only.
    };

    //==================================================================================================================
    Fixture makeFixture(test::PassKit& kit)
    {
        Fixture fx;
        fx.args = kit.buffer(BufferUsage::IndirectArgs, 1024);
        fx.index = kit.buffer(BufferUsage::Index, 64);
        fx.first = kit.buffer(BufferUsage::None, 1 << 20);
        fx.second = kit.buffer(BufferUsage::None, 1 << 20);
        fx.writable = kit.buffer(BufferUsage::ShaderWrite, 256);
        fx.sampled = kit.texture();
        fx.other = kit.texture();
        fx.color = kit.target();
        return fx;
    }

    //==================================================================================================================
    /// Records the case's setup, and its entry when `withEntry`, submits the list and returns the device's counters.
    Counters runCase(const EntryCase& entryCase, bool withEntry)
    {
        test::PassKit kit(entryCase.type);
        Fixture fx = makeFixture(kit);
        entryCase.setup(kit, fx);
        if (withEntry)
        {
            entryCase.entry(kit, fx);
        }
        if (entryCase.inPass)
        {
            kit.list.endRenderPass();
        }
        kit.list.end();
        const std::array<CommandList, 1> lists{ kit.list };
        kit.device.queue(kit.type).submit(lists);
        kit.device.endFrame();
        const Counters counters = kit.device.counters();
        destroyDevice(kit.device);
        return counters;
    }

    //==================================================================================================================
    void noSetup(test::PassKit&, Fixture&)
    {
    }

    //==================================================================================================================
    std::vector<EntryCase> entryCases()
    {
        constexpr std::array<Format, 1> kRgba8 = { Format::RGBA8Unorm };
        const auto draw = [](test::PassKit& kit, Fixture&) { kit.readyToDraw(); };
        const auto mesh = [](test::PassKit& kit, Fixture&) { kit.readyToDraw(true); };
        const auto compute = [](test::PassKit& kit, Fixture&) { kit.list.setPipeline(kit.compute()); };
        const BufferTextureLayout layout{ .rowPitch = 256 };
        const TextureRegion row{ .extent = { 4, 1, 1 } };
        const TextureRegion square{ .extent = { 8, 8, 1 } };
        std::vector<EntryCase> cases;
        cases.push_back({ "drawIndexed", QueueType::Graphics, true, 1, draw, [](test::PassKit& kit, Fixture& fx) {
                             kit.list.drawIndexed(fx.index, IndexType::UInt16, 8);
                         } });
        cases.push_back({ "drawMeshTasks", QueueType::Graphics, true, 1, mesh, [](test::PassKit& kit, Fixture&) { kit.list.drawMeshTasks(1, 1, 1); } });
        cases.push_back({ "dispatch", QueueType::Graphics, false, 1, compute, [](test::PassKit& kit, Fixture&) { kit.list.dispatch(1, 1, 1); } });
        cases.push_back({ "drawIndirect", QueueType::Graphics, true, 1, draw, [](test::PassKit& kit, Fixture& fx) {
                             kit.list.drawIndirect(kit.device.address(fx.args), 4);
                         } });
        cases.push_back({ "drawIndexedIndirect", QueueType::Graphics, true, 1, draw, [](test::PassKit& kit, Fixture& fx) {
                             kit.list.drawIndexedIndirect(fx.index, IndexType::UInt32, kit.device.address(fx.args), 4);
                         } });
        cases.push_back({ "drawMeshTasksIndirect", QueueType::Graphics, true, 1, mesh, [](test::PassKit& kit, Fixture& fx) {
                             kit.list.drawMeshTasksIndirect(kit.device.address(fx.args), 4);
                         } });
        cases.push_back({ "dispatchIndirect", QueueType::Graphics, false, 1, compute, [](test::PassKit& kit, Fixture& fx) {
                             kit.list.dispatchIndirect(kit.device.address(fx.args));
                         } });
        cases.push_back({ "copyBuffer", QueueType::Copy, false, 1, noSetup, [](test::PassKit& kit, Fixture& fx) {
                             kit.list.copyBuffer(fx.first, 0, fx.second, 0, 64);
                         } });
        cases.push_back({ "copyBufferToTexture", QueueType::Copy, false, 1, noSetup, [=](test::PassKit& kit, Fixture& fx) {
                             kit.list.copyBufferToTexture(fx.first, layout, fx.sampled, row);
                         } });
        cases.push_back({ "copyTextureToBuffer", QueueType::Copy, false, 1, noSetup, [=](test::PassKit& kit, Fixture& fx) {
                             kit.list.copyTextureToBuffer(fx.sampled, row, fx.first, layout);
                         } });
        cases.push_back({ "copyTexture", QueueType::Copy, false, 1, noSetup, [=](test::PassKit& kit, Fixture& fx) {
                             kit.list.copyTexture(fx.sampled, square, fx.other, square);
                         } });
        cases.push_back({ "fillBuffer", QueueType::Graphics, false, 1, noSetup, [](test::PassKit& kit, Fixture& fx) {
                             kit.list.fillBuffer(fx.writable, 0, 256, 0xDEADBEEFu);
                         } });
        cases.push_back({ "clearTexture", QueueType::Graphics, false, 1, noSetup, [](test::PassKit& kit, Fixture& fx) {
                             kit.list.clearTexture(fx.color, {}, { .color = { 1, 0, 0, 1 } });
                         } });
        cases.push_back(
            { "setBlendConstants",
              QueueType::Graphics,
              true,
              1,
              [](test::PassKit& kit, Fixture&) { kit.beginSimplePass(); },
              [](test::PassKit& kit, Fixture&) { kit.list.setBlendConstants(0.5f, 0.25f, 0.0f, 1.0f); } }
        );
        cases.push_back(
            { "setStencilReference",
              QueueType::Graphics,
              true,
              1,
              [](test::PassKit& kit, Fixture&) { kit.beginSimplePass(); },
              [](test::PassKit& kit, Fixture&) { kit.list.setStencilReference(255); } }
        );
        cases.push_back(
            { "setDepthBias",
              QueueType::Graphics,
              true,
              1,
              [=](test::PassKit& kit, Fixture&) {
                  kit.beginSimplePass();
                  kit.list.setPipeline(kit.graphics(kRgba8, Format::Undefined, 1, PrimitiveClass::Triangle, true));
              },
              [](test::PassKit& kit, Fixture&) { kit.list.setDepthBias(1.0f, 2.0f, 0.0f); } }
        );
        cases.push_back({ "setRoot", QueueType::Graphics, false, 1, noSetup, [](test::PassKit& kit, Fixture& fx) {
                             kit.list.setRoot(kit.device.address(fx.first));
                         } });
        cases.push_back(
            { "setPrimitiveTopology",
              QueueType::Graphics,
              true,
              0,
              [](test::PassKit& kit, Fixture&) { kit.readyToDraw(); },
              [](test::PassKit& kit, Fixture&) { kit.list.setPrimitiveTopology(PrimitiveTopology::TriangleStrip); } }
        );
        cases.push_back({ "beginScope and endScope", QueueType::Graphics, false, 2, noSetup, [](test::PassKit& kit, Fixture&) {
                             kit.list.beginScope("entry.scope");
                             kit.list.endScope();
                         } });
        return cases;
    }
} // namespace

//======================================================================================================================
TEST_CASE("each recording entry adds its own nativeRecordCalls and no other recorded count", "[null]")
{
    for (const EntryCase& entryCase : entryCases())
    {
        CAPTURE(entryCase.name);
        const Counters base = runCase(entryCase, false);
        const Counters with = runCase(entryCase, true);
        CHECK(with.nativeRecordCalls - base.nativeRecordCalls == entryCase.calls);
        CHECK(with.frameDataUploads == base.frameDataUploads);
        CHECK(with.frameArenaBytes == base.frameArenaBytes);
        CHECK(with.nativeBarriers == base.nativeBarriers);
        CHECK(with.encodersOpened == base.encodersOpened);
    }
}

//======================================================================================================================
TEST_CASE("uploadFrameData adds an upload and no native call, and pushRoot adds the root bind as well", "[null]")
{
    const std::array<std::byte, 8> bytes{};
    const EntryCase upload{
        "uploadFrameData", QueueType::Graphics, false, 0, noSetup, [&](test::PassKit& kit, Fixture&) { (void)kit.list.uploadFrameData(bytes); }
    };
    const Counters uploaded = runCase(upload, true);
    const Counters base = runCase(upload, false);
    CHECK(uploaded.nativeRecordCalls == base.nativeRecordCalls);
    CHECK(uploaded.frameDataUploads == base.frameDataUploads + 1);
    CHECK(uploaded.frameArenaBytes == base.frameArenaBytes + 8);
    const EntryCase push{ "pushRoot", QueueType::Graphics,
                          false,      1,
                          noSetup,    [&](test::PassKit& kit, Fixture&) { (void)kit.list.pushRoot(std::array<uint32_t, 4>{}); } };
    const Counters pushed = runCase(push, true);
    CHECK(pushed.nativeRecordCalls == base.nativeRecordCalls + 1);
    CHECK(pushed.frameDataUploads == base.frameDataUploads + 1);
    CHECK(pushed.frameArenaBytes == base.frameArenaBytes + 16);
}

//======================================================================================================================
TEST_CASE("an upscale adds one nativeRecordCalls", "[null]")
{
    Counters counters[2];
    for (const bool withEntry : { false, true })
    {
        test::UpscaleKit kit;
        if (withEntry)
        {
            kit.upscale();
        }
        kit.kit.list.end();
        const std::array<CommandList, 1> lists{ kit.kit.list };
        kit.kit.device.queue(QueueType::Graphics).submit(lists);
        kit.kit.device.endFrame();
        counters[withEntry ? 1 : 0] = kit.kit.device.counters();
        destroyDevice(kit.kit.device);
    }
    CHECK(counters[1].nativeRecordCalls - counters[0].nativeRecordCalls == 1);
}

//======================================================================================================================
TEST_CASE("a submission counts one native call and one for each wait it carries", "[null]")
{
    Device device = *createDevice();
    const Fence a = *device.createFence(5, "wait.a");
    const Fence b = *device.createFence(5, "wait.b");
    const std::array<FenceValue, 2> waits{ FenceValue{ a, 1 }, FenceValue{ b, 5 } };
    const Counters before = device.counters();
    device.queue(QueueType::Graphics).submit({}, waits, {});
    CHECK(delta(before, device.counters()).nativeSubmitCalls == 3);
    const std::array<FenceValue, 1> signal{ FenceValue{ a, 6 } };
    const Counters between = device.counters();
    device.queue(QueueType::Graphics).submit({}, waits, signal);
    CHECK(delta(between, device.counters()).nativeSubmitCalls == 4);
    device.destroy(a);
    device.destroy(b);
    destroyDevice(device);
}
