//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4TimestampTests.cpp
/// @brief Tests the Metal 4 backend's counter heaps and the timestamps of scopes, render passes, scopes inside them and upscales.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/CommandLists.h"
#include "Core/DeviceCore.h"
#include "Core/ScopeRecord.h"
#include "Metal4Common.h"
#include "Metal4Device.h"
#include "Metal4Timestamps.h"
#include "Support/Backend.h"
#include "Support/Metal4/Metal4Timing.h"
#include "Support/UpscaleKit.h"

#include <catch2/catch_test_macros.hpp>

#include <Metal/Metal.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <format>
#include <latch>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace rojoRHI;

namespace {
    /// The warnings the message callback collected.
    std::vector<std::string> g_warnings;

    //==================================================================================================================
    /// Collects every warning into `g_warnings`.
    void collectWarning(MessageSeverity severity, std::string_view message, void*)
    {
        if (severity == MessageSeverity::Warning)
        {
            g_warnings.emplace_back(message);
        }
    }

    //==================================================================================================================
    /// The timestamp storage of `device`.
    const metal4::Timestamps& timestampsOf(Device device)
    {
        return DeviceImpl::of(device).timestamps;
    }

    //==================================================================================================================
    /// The frame slot `list` records into.
    uint32_t slotOf(Device device, CommandList list)
    {
        return static_cast<uint32_t>(core::coreOf(list).frame % core::coreOf(device).framesInFlight());
    }

    //==================================================================================================================
    /// Returns true when no frame slot of `device` holds a counter heap.
    bool holdsNoHeap(Device device)
    {
        for (uint32_t slot = 0; slot < core::coreOf(device).framesInFlight(); ++slot)
        {
            for (uint32_t number = 0; number < metal4::Timestamps::kSlotHeaps; ++number)
            {
                if (timestampsOf(device).heap(slot, number) != nullptr)
                {
                    return false;
                }
            }
        }
        return true;
    }

    //==================================================================================================================
    /// The label of `heap`, read inside a pool because the getter autoreleases.
    std::string heapLabel(MTL4::CounterHeap* heap)
    {
        const metal4::AutoreleasePool pool;
        return metal4::toStdString(heap->label());
    }

    //==================================================================================================================
    /// Ends every list of `lists` and submits each to the queue of its type; the compute lists signal a fence the frame's
    /// graphics queue waits on, which joins them into the frame. Waits on the host until all of them completed.
    void submitFrame(Device device, std::span<const CommandList> lists)
    {
        const Fence fence = *device.createFence(0, "test.done");
        uint64_t value = 0;
        for (CommandList list : lists)
        {
            list.end();
            const QueueType type = core::coreOf(list).type;
            if (type == QueueType::Graphics)
            {
                device.queue(type).submit(std::array{ list });
                continue;
            }
            const std::array<FenceValue, 1> signal{ FenceValue{ fence, ++value } };
            device.queue(type).submit(std::array{ list }, {}, signal);
        }
        const std::array<FenceValue, 1> wait{ FenceValue{ fence, value } };
        const std::array<FenceValue, 1> signal{ FenceValue{ fence, value + 1 } };
        device.queue(QueueType::Graphics).submit({}, value > 0 ? std::span<const FenceValue>(wait) : std::span<const FenceValue>(), signal);
        REQUIRE(device.wait({ fence, value + 1 }, UINT64_MAX));
    }

    //==================================================================================================================
    /// Gives `list` its frame-arena page, which a scope's label is copied into, so the page's creation is not counted with a
    /// scope's.
    void claimArenaPage(CommandList list)
    {
        const std::array<std::byte, 16> bytes{};
        list.uploadFrameData(bytes);
    }

    //==================================================================================================================
    /// Records `count` scopes, one after another, on `list`.
    void recordScopes(CommandList list, uint32_t count)
    {
        for (uint32_t scope = 0; scope < count; ++scope)
        {
            list.beginScope("test.scope");
            list.endScope();
        }
    }

    //==================================================================================================================
    /// Ends the open frame and runs empty frames until the next `beginFrame` waits for it, then begins that frame, so
    /// `Device::scopeTimings` reports the scopes of the frame that was open.
    void retireOpenFrame(Device device)
    {
        device.endFrame();
        for (uint32_t frame = 1; frame < core::coreOf(device).framesInFlight(); ++frame)
        {
            device.beginFrame();
            device.endFrame();
        }
        device.beginFrame();
    }

    /// The edge of the drawing cases' target, in texels.
    constexpr uint32_t kEdge = 64;

    /// What the drawing cases draw with: the standard library's vertex and fragment pipeline over an `RGBA8Unorm` target, and
    /// one draw record of three vertices with a count of one, for a counted draw.
    struct Drawing
    {
        ShaderLibrary library; ///< The standard library.
        Pipeline pipeline;     ///< The vertex and fragment pipeline over the target.
        Texture target;        ///< A `kEdge` square `RGBA8Unorm` render target.
        Buffer records;        ///< One draw record of three vertices.
        Buffer counts;         ///< A count of one.

        //==============================================================================================================
        /// Creates the objects on `device`.
        explicit Drawing(Device device)
        {
            library = *device.loadShaderLibrary(test::standardLibrary(), "test.library");
            const std::array colors{ ColorTarget{ .format = Format::RGBA8Unorm } };
            pipeline = *device.createPipeline(
                GraphicsPipelineDesc{ .vertex = { library, "vsMain" }, .fragment = { library, "fsMain" }, .colors = colors, .label = "test.pipeline" }
            );
            target = *device.createTexture(
                TextureDesc{ .format = Format::RGBA8Unorm, .extent = { kEdge, kEdge, 1 }, .usage = TextureUsage::RenderTarget, .label = "test.target" }
            );
            const std::array<DrawIndirectArgs, 1> draws{ DrawIndirectArgs{ .vertexCount = 3, .instanceCount = 1 } };
            records = *device.createBuffer(
                BufferDesc{ .size = sizeof(draws), .usage = BufferUsage::IndirectArgs, .label = "test.records" }, std::as_bytes(std::span(draws))
            );
            const std::array<uint32_t, 1> one{ 1 };
            counts = *device.createBuffer(
                BufferDesc{ .size = sizeof(one), .usage = BufferUsage::IndirectArgs, .label = "test.counts" }, std::as_bytes(std::span(one))
            );
        }

        //==============================================================================================================
        /// Begins a pass labelled `label` over the target, cleared, binds the pipeline and sets the viewport and scissor.
        void begin(CommandList list, std::string_view label) const
        {
            const std::array colors{ ColorAttachment{ .texture = target, .load = LoadOp::Clear } };
            list.beginRenderPass(RenderPassDesc{ .colors = colors, .area = { 0, 0, kEdge, kEdge }, .label = label });
            list.setPipeline(pipeline);
            list.setViewport(Viewport{ .width = static_cast<float>(kEdge), .height = static_cast<float>(kEdge) });
            list.setScissor(Rect2D{ 0, 0, kEdge, kEdge });
        }

        //==============================================================================================================
        /// Records a counted draw of the one record, whose kernel runs on a compute encoder ahead of the render encoder.
        void counted(Device device, CommandList list) const
        {
            list.drawIndirect(device.address(records), 1, device.address(counts));
        }
    };

    //==================================================================================================================
    /// Returns the growth of `nativeHostCalls` across `device.beginFrame()`.
    uint64_t hostCallsOfBeginFrame(Device device)
    {
        const uint64_t before = device.counters().nativeHostCalls;
        device.beginFrame();
        return device.counters().nativeHostCalls - before;
    }
} // namespace

//======================================================================================================================
TEST_CASE("a scope outside a pass writes two timestamps on the command buffer", "[metal4]")
{
    Device device = *createDevice();
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    const core::CommandListCore& state = core::coreOf(list);
    const uint64_t calls = state.counts.nativeRecordCalls;
    // No encoder is open, so the scope closes none: the debug group, then the begin timestamp.
    list.beginScope("test.scope");
    // The pinned counts include the timestamp writes, so every scope must hold a pair.
    test::requireTimed(list);
    CHECK(state.counts.nativeRecordCalls == calls + 2);
    REQUIRE(state.scopeLog.size() == 1);
    const uint32_t pair = state.scopeLog.back().timestamp;
    CHECK(pair != core::kNoTimestamp);
    CHECK(pair % 2 == 0);
    REQUIRE(CommandListImpl::of(list).state.scopeTimestamps.size() == 1);
    CHECK(CommandListImpl::of(list).state.scopeTimestamps.back() == pair);
    CHECK(timestampsOf(device).heapFor(slotOf(device, list), pair) != nullptr);
    // The end timestamp, then the pop.
    list.endScope();
    CHECK(state.counts.nativeRecordCalls == calls + 4);
    CHECK(state.counts.encodersOpened == 0);
    CHECK(CommandListImpl::of(list).state.scopeTimestamps.empty());
    submitFrame(device, std::array{ list });
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a device that records no scope creates no counter heap", "[metal4]")
{
    Device device = *createDevice({ .framesInFlight = 2 });
    const Buffer a = *device.createBuffer({ .size = 256, .label = "test.a" });
    const Buffer b = *device.createBuffer({ .size = 256, .label = "test.b" });
    for (int frame = 0; frame < 4; ++frame)
    {
        device.beginFrame();
        CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
        list.copyBuffer(a, 0, b, 0, 256);
        submitFrame(device, std::array{ list });
        device.endFrame();
    }
    CHECK(holdsNoHeap(device));
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("the first scope of a frame slot creates its heap once, a second does not", "[metal4]")
{
    Device device = *createDevice({ .framesInFlight = 2 });
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    const uint32_t slot = slotOf(device, list);
    claimArenaPage(list);
    const uint64_t before = device.counters().nativeCreateCalls;
    list.beginScope("test.first");
    // The pinned counts include the timestamp writes, so every scope must hold a pair.
    test::requireTimed(list);
    CHECK(device.counters().nativeCreateCalls == before + 1);
    MTL4::CounterHeap* heap = timestampsOf(device).heap(slot, 0);
    REQUIRE(heap != nullptr);
    CHECK(heapLabel(heap) == std::format("rojorhi.timestamps.{}.0", slot));
    CHECK(heap->count() == metal4::Timestamps::kHeapEntries);
    list.endScope();
    list.beginScope("test.second");
    list.endScope();
    CHECK(device.counters().nativeCreateCalls == before + 1);
    CHECK(timestampsOf(device).heap(slot, 0) == heap);
    CHECK(timestampsOf(device).heap(slot, 1) == nullptr);
    CHECK(timestampsOf(device).heap(1 - slot, 0) == nullptr);
    submitFrame(device, std::array{ list });
    device.endFrame();
    // The other slot has its own heap; the first slot, reused two frames later, keeps its heap and claims from index 0 again.
    for (int frame = 0; frame < 2; ++frame)
    {
        device.beginFrame();
        CommandList next = device.acquireCommandList(QueueType::Graphics, "test.next");
        claimArenaPage(next);
        const uint64_t created = device.counters().nativeCreateCalls;
        next.beginScope("test.scope");
        test::requireTimed(next);
        CHECK(device.counters().nativeCreateCalls == created + (slotOf(device, next) == slot ? 0 : 1));
        CHECK(core::coreOf(next).scopeLog.back().timestamp == 0);
        next.endScope();
        submitFrame(device, std::array{ next });
        device.endFrame();
    }
    CHECK(timestampsOf(device).heap(slot, 0) == heap);
    CHECK(timestampsOf(device).heap(1 - slot, 0) != nullptr);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("pairs are claimed without overlap by lists recording on several threads", "[metal4]")
{
    constexpr uint32_t kThreads = 8;
    constexpr uint32_t kScopes = 256;
    Device device = *createDevice({ .framesInFlight = 2 });
    device.beginFrame();
    std::array<CommandList, kThreads> lists{};
    for (CommandList& list : lists)
    {
        list = device.acquireCommandList(QueueType::Graphics, "test.thread");
        claimArenaPage(list);
    }
    const uint32_t slot = slotOf(device, lists[0]);
    const uint64_t before = device.counters().nativeCreateCalls;
    std::latch start(kThreads);
    std::vector<std::thread> threads;
    for (CommandList list : lists)
    {
        threads.emplace_back([&start, list] {
            start.arrive_and_wait();
            recordScopes(list, kScopes);
        });
    }
    for (std::thread& thread : threads)
    {
        thread.join();
    }
    for (CommandList list : lists)
    {
        test::requireTimed(list);
    }
    // The threads raced for the slot's first heap, and one heap was kept.
    CHECK(device.counters().nativeCreateCalls == before + 1);
    CHECK(timestampsOf(device).heap(slot, 0) != nullptr);
    CHECK(timestampsOf(device).heap(slot, 1) == nullptr);
    std::set<uint32_t> pairs;
    for (CommandList list : lists)
    {
        for (const core::ScopeRecord& record : core::coreOf(list).scopeLog)
        {
            CHECK(record.timestamp % 2 == 0);
            CHECK(record.timestamp < metal4::Timestamps::kHeapEntries);
            pairs.insert(record.timestamp);
        }
    }
    CHECK(pairs.size() == kThreads * kScopes);
    submitFrame(device, lists);
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a frame with more scopes than the heaps hold reports zero for the rest and warns once", "[metal4]")
{
    // Recorded in full rather than through a smaller capacity: the slot's eight heaps of 4,096 timestamps hold 16,384 pairs.
    constexpr uint32_t kCapacity = metal4::Timestamps::kSlotHeaps * metal4::Timestamps::kHeapEntries / 2;
    g_warnings.clear();
    setMessageCallback(collectWarning, nullptr);
    Device device = *createDevice({ .framesInFlight = 2 });
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    const uint32_t slot = slotOf(device, list);
    recordScopes(list, kCapacity + 1);
    const std::vector<core::ScopeRecord>& log = core::coreOf(list).scopeLog;
    REQUIRE(log.size() == kCapacity + 1);
    CHECK(log[kCapacity - 1].timestamp == 2 * (kCapacity - 1));
    CHECK(log[kCapacity].timestamp == core::kNoTimestamp);
    for (uint32_t number = 0; number < metal4::Timestamps::kSlotHeaps; ++number)
    {
        CHECK(timestampsOf(device).heap(slot, number) != nullptr);
    }
    submitFrame(device, std::array{ list });
    device.endFrame();
    device.beginFrame();
    device.endFrame();
    CHECK(g_warnings.empty());
    device.beginFrame();
    const std::span<const ScopeTiming> report = device.scopeTimings();
    REQUIRE(report.size() == kCapacity + 1);
    CHECK(report.back().gpuMilliseconds == 0.0);
    CHECK(std::isfinite(report[kCapacity - 1].gpuMilliseconds));
    CHECK(report[kCapacity - 1].gpuMilliseconds >= 0.0);
    REQUIRE(g_warnings.size() == 1);
    CHECK(g_warnings[0].find("frame 1") != std::string::npos);
    CHECK(g_warnings[0].find("1 scope") != std::string::npos);
    device.endFrame();
    // The next retired frame recorded no scope, so it warns of nothing.
    device.beginFrame();
    CHECK(g_warnings.size() == 1);
    device.endFrame();
    destroyDevice(device);
    setMessageCallback(nullptr, nullptr);
}

//======================================================================================================================
TEST_CASE("scopeTimings counts one host call per used heap resolve", "[metal4]")
{
    // A heap holds 2,048 pairs, so 2,049 scopes use two heaps. Each used heap is resolved once at the frame wait and its used
    // range invalidated once when the slot is reused: two host calls per heap beyond a frame without scopes.
    constexpr uint32_t kScopes = metal4::Timestamps::kHeapEntries / 2 + 1;
    Device device = *createDevice({ .framesInFlight = 2 });
    for (const uint32_t scopes : { 0u, kScopes })
    {
        device.beginFrame();
        CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
        recordScopes(list, scopes);
        test::requireTimed(list);
        submitFrame(device, std::array{ list });
        device.endFrame();
    }
    // Frame 3 retires the frame without scopes, frame 4 the one with them.
    const uint64_t plain = hostCallsOfBeginFrame(device);
    device.endFrame();
    const uint64_t timed = hostCallsOfBeginFrame(device);
    CHECK(timed == plain + 2 * 2);
    // Reading the report is no further native call.
    const uint64_t before = device.counters().nativeHostCalls;
    CHECK(device.scopeTimings().size() == kScopes);
    CHECK(device.counters().nativeHostCalls == before);
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("scopes on two queue types in one frame resolve their slot's heap once", "[metal4]")
{
    Device device = *createDevice({ .framesInFlight = 2 });
    for (const bool scoped : { false, true })
    {
        device.beginFrame();
        const std::array<CommandList, 2> lists{ device.acquireCommandList(QueueType::Graphics, "test.graphics"),
                                                device.acquireCommandList(QueueType::Compute, "test.compute") };
        if (scoped)
        {
            recordScopes(lists[0], 1);
            recordScopes(lists[1], 1);
            test::requireTimed(lists[0]);
            test::requireTimed(lists[1]);
        }
        submitFrame(device, lists);
        device.endFrame();
    }
    const uint64_t plain = hostCallsOfBeginFrame(device);
    device.endFrame();
    // The hook runs once for the graphics scopes and once for the compute ones; the heap is resolved at the first call only.
    const uint64_t timed = hostCallsOfBeginFrame(device);
    CHECK(timed == plain + 1 + 1);
    const std::span<const ScopeTiming> report = device.scopeTimings();
    REQUIRE(report.size() == 2);
    CHECK(report[0].queue == QueueType::Graphics);
    CHECK(report[1].queue == QueueType::Compute);
    for (const ScopeTiming& timing : report)
    {
        CHECK(std::isfinite(timing.gpuMilliseconds));
        CHECK(timing.gpuMilliseconds >= 0.0);
    }
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a scope on a Copy list claims a pair and reports a duration of at least zero", "[metal4]")
{
    Device device = *createDevice({ .framesInFlight = 2 });
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Copy, "test.copy");
    list.beginScope("test.scope");
    list.endScope();
    const uint32_t pair = core::coreOf(list).scopeLog.back().timestamp;
    CHECK(pair != core::kNoTimestamp);
    CHECK(timestampsOf(device).heapFor(slotOf(device, list), pair) != nullptr);
    submitFrame(device, std::array{ list });
    device.endFrame();
    device.beginFrame();
    device.endFrame();
    device.beginFrame();
    const std::span<const ScopeTiming> report = device.scopeTimings();
    REQUIRE(report.size() == 1);
    CHECK(report[0].queue == QueueType::Copy);
    CHECK(std::isfinite(report[0].gpuMilliseconds));
    CHECK(report[0].gpuMilliseconds >= 0.0);
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("two live devices each time a scope in heaps of their own", "[metal4]")
{
    std::array<Device, 2> devices{ *createDevice({ .framesInFlight = 2 }), *createDevice({ .framesInFlight = 2 }) };
    std::array<CommandList, 2> lists{};
    for (size_t i = 0; i < devices.size(); ++i)
    {
        devices[i].beginFrame();
        lists[i] = devices[i].acquireCommandList(QueueType::Graphics, "test.list");
        claimArenaPage(lists[i]);
        const uint64_t before = devices[i].counters().nativeCreateCalls;
        lists[i].beginScope("test.scope");
        test::requireTimed(lists[i]);
        lists[i].endScope();
        CHECK(devices[i].counters().nativeCreateCalls == before + 1);
        CHECK(core::coreOf(lists[i]).scopeLog.back().timestamp == 0);
    }
    const uint32_t slot = slotOf(devices[0], lists[0]);
    MTL4::CounterHeap* first = timestampsOf(devices[0]).heap(slot, 0);
    MTL4::CounterHeap* second = timestampsOf(devices[1]).heap(slotOf(devices[1], lists[1]), 0);
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);
    CHECK(first != second);
    for (size_t i = 0; i < devices.size(); ++i)
    {
        submitFrame(devices[i], std::array{ lists[i] });
        devices[i].endFrame();
        devices[i].beginFrame();
        devices[i].endFrame();
        devices[i].beginFrame();
        const std::span<const ScopeTiming> report = devices[i].scopeTimings();
        REQUIRE(report.size() == 1);
        CHECK(std::isfinite(report[0].gpuMilliseconds));
        CHECK(report[0].gpuMilliseconds >= 0.0);
        devices[i].endFrame();
    }
    for (Device device : devices)
    {
        destroyDevice(device);
    }
}

//======================================================================================================================
TEST_CASE("a pair no timestamp was written into reports zero", "[metal4]")
{
    // Metal resolves an unwritten entry of a new heap to zero, and so does an invalidated one; neither is a time.
    // Creating the system device autoreleases objects, so the case holds a pool for its whole body.
    const metal4::AutoreleasePool outer;
    MTL::Device* native = MTL::CreateSystemDefaultDevice();
    REQUIRE(native != nullptr);
    core::DeviceCounts counts;
    metal4::Timestamps timestamps;
    timestamps.create(native);
    const uint32_t pair = timestamps.claim(0, counts);
    REQUIRE(pair == 0);
    const std::array<core::ScopeRecord, 1> records{ core::ScopeRecord{ .label = "test.unwritten", .timestamp = pair } };
    std::array<ScopeTiming, 1> report{ ScopeTiming{ .label = "test.unwritten", .gpuMilliseconds = -1.0 } };
    timestamps.resolve(0, records, report, counts);
    CHECK(report[0].gpuMilliseconds == 0.0);
    timestamps.reuse(0, counts);
    CHECK(timestamps.claim(0, counts) == pair);
    report[0].gpuMilliseconds = -1.0;
    timestamps.resolve(0, records, report, counts);
    CHECK(report[0].gpuMilliseconds == 0.0);
    timestamps.reuse(0, counts);
    // One heap, then a resolve and an invalidation in each of the two frames.
    CHECK(counts.nativeCreateCalls.load() == 1);
    CHECK(counts.nativeHostCalls.load() == 2 + 2);
    {
        const metal4::AutoreleasePool pool;
        timestamps.release();
        native->release();
    }
}

//======================================================================================================================
TEST_CASE("a heap Metal refuses is asked for again after its slot's reuse, and created once there is room", "[metal4]")
{
    // Metal limits the counter heaps of a process or of the system: storages fill slot 0 until Metal refuses a heap.
    constexpr uint32_t kSlotPairs = metal4::Timestamps::kSlotHeaps * metal4::Timestamps::kHeapEntries / 2;
    g_warnings.clear();
    setMessageCallback(collectWarning, nullptr);
    // Creating the system device autoreleases objects, so the case holds a pool for its whole body.
    const metal4::AutoreleasePool outer;
    MTL::Device* native = MTL::CreateSystemDefaultDevice();
    REQUIRE(native != nullptr);
    core::DeviceCounts counts;
    std::deque<metal4::Timestamps> full;
    bool refused = false;
    while (!refused && full.size() < 8)
    {
        metal4::Timestamps& timestamps = full.emplace_back();
        timestamps.create(native);
        for (uint32_t pair = 0; pair < kSlotPairs && !refused; ++pair)
        {
            refused = timestamps.claim(0, counts) == core::kNoTimestamp;
        }
    }
    REQUIRE(refused);
    metal4::Timestamps late;
    late.create(native);
    CHECK(late.claim(0, counts) == core::kNoTimestamp);
    CHECK(late.heap(0, 0) == nullptr);
    // Room for eight heaps again. The refusal holds for the rest of the frame, so the slot claims nothing yet.
    {
        const metal4::AutoreleasePool pool;
        full.front().release();
    }
    CHECK(late.claim(0, counts) == core::kNoTimestamp);
    CHECK(late.heap(0, 0) == nullptr);
    // The slot's reuse clears the refusal: its next claim asks Metal again and gets the heap.
    late.reuse(0, counts);
    const uint64_t created = counts.nativeCreateCalls.load();
    CHECK(late.claim(0, counts) == 0);
    CHECK(late.heap(0, 0) != nullptr);
    CHECK(counts.nativeCreateCalls.load() == created + 1);
    // Each device logs the first refusal only.
    CHECK(g_warnings.size() == 2);
    {
        const metal4::AutoreleasePool pool;
        late.release();
        for (metal4::Timestamps& timestamps : full)
        {
            timestamps.release();
        }
        native->release();
    }
    setMessageCallback(nullptr, nullptr);
}

//======================================================================================================================
TEST_CASE("a render pass with a counted draw writes its begin timestamp on the kernels' encoder and its end on its render encoder", "[metal4]")
{
    Device device = *createDevice({ .framesInFlight = 2 });
    const Drawing drawing(device);
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    const uint64_t label = test::encoderLabel(list);
    claimArenaPage(list);
    const core::CommandListCore& state = core::coreOf(list);
    const metal4::ListState& native = CommandListImpl::of(list).state;
    const metal4::DrawState& draw = CommandListImpl::of(list).draw;
    const uint64_t calls = state.counts.nativeRecordCalls;
    const uint64_t encoders = state.counts.encodersOpened;
    const std::array colors{ ColorAttachment{ .texture = drawing.target, .load = LoadOp::Clear } };
    list.beginRenderPass(RenderPassDesc{ .colors = colors, .area = { 0, 0, kEdge, kEdge }, .label = "test.pass" });
    // The pinned counts include the timestamp writes, so every scope must hold a pair.
    test::requireTimed(list);
    // The pass's debug group alone: the pass claims its pair, and no encoder of the pass is open to write it on.
    CHECK(state.counts.nativeRecordCalls == calls + 1);
    CHECK(state.counts.encodersOpened == encoders);
    REQUIRE(state.scopeLog.size() == 1);
    const uint32_t pair = state.scopeLog.back().timestamp;
    CHECK(pair != core::kNoTimestamp);
    CHECK(pair % 2 == 0);
    // The begin entry waits for the pass's first encoder.
    CHECK(draw.passBegin == pair);
    REQUIRE(native.scopeTimestamps.size() == 1);
    CHECK(native.scopeTimestamps.back() == pair);
    CHECK(timestampsOf(device).heapFor(slotOf(device, list), pair) != nullptr);
    list.setPipeline(drawing.pipeline);
    list.setViewport(Viewport{ .width = static_cast<float>(kEdge), .height = static_cast<float>(kEdge) });
    list.setScissor(Rect2D{ 0, 0, kEdge, kEdge });
    // A counted draw first: its kernel's compute encoder is the pass's first encoder, and the begin timestamp goes on it,
    // between the encoder's factory and label and the writer kernel's pipeline, kernel table, parameters and dispatch.
    uint64_t before = state.counts.nativeRecordCalls;
    drawing.counted(device, list);
    CHECK(state.counts.encodersOpened == encoders + 1);
    CHECK(CommandListImpl::of(list).encoders.kind() == metal4::EncoderKind::Compute);
    CHECK(state.counts.nativeRecordCalls == before + 1 + label + 1 + 4);
    CHECK(draw.passBegin == core::kNoTimestamp);
    // A second counted draw shares the kernels' encoder and writes no timestamp: its parameters and its dispatch.
    before = state.counts.nativeRecordCalls;
    drawing.counted(device, list);
    CHECK(state.counts.encodersOpened == encoders + 1);
    CHECK(state.counts.nativeRecordCalls == before + 2);
    const uint64_t beforeEnd = state.counts.nativeRecordCalls;
    list.endRenderPass();
    // The kernels' encoder ends; the render encoder opens (factory, label, table) with the barrier that orders the kernels
    // before the vertex stage, sets the pipeline, its winding, the viewport and the scissor and draws the two records; then
    // the end timestamp on it and the pop on the command buffer. The render encoder stays open until the list ends.
    CHECK(state.counts.encodersOpened == encoders + 2);
    CHECK(state.counts.nativeRecordCalls == beforeEnd + 1 + 2 + label + 1 + 4 + 2 + 1 + 1);
    CHECK(CommandListImpl::of(list).encoders.lingering());
    CHECK(native.scopeTimestamps.empty());
    CHECK(native.scopeLabels.empty());
    submitFrame(device, std::array{ list });
    retireOpenFrame(device);
    const std::span<const ScopeTiming> report = device.scopeTimings();
    REQUIRE(report.size() == 1);
    CHECK(report[0].label == "test.pass");
    CHECK(std::isfinite(report[0].gpuMilliseconds));
    CHECK(report[0].gpuMilliseconds > 0.0);
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a pass whose encoder opens at endRenderPass writes both its timestamps on that encoder", "[metal4]")
{
    Device device = *createDevice({ .framesInFlight = 2 });
    const Drawing drawing(device);
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    const uint64_t label = test::encoderLabel(list);
    claimArenaPage(list);
    const core::CommandListCore& state = core::coreOf(list);
    const uint64_t calls = state.counts.nativeRecordCalls;
    const std::array colors{ ColorAttachment{ .texture = drawing.target, .load = LoadOp::Clear } };
    list.beginRenderPass(RenderPassDesc{ .colors = colors, .area = { 0, 0, kEdge, kEdge }, .label = "test.empty" });
    // The pinned counts include the timestamp writes, so every scope must hold a pair.
    test::requireTimed(list);
    CHECK(state.counts.nativeRecordCalls == calls + 1);
    // No encoder opened, so the begin entry still waits.
    CHECK(CommandListImpl::of(list).draw.passBegin == state.scopeLog.back().timestamp);
    const uint64_t encoders = state.counts.encodersOpened;
    list.endRenderPass();
    // renderCommandEncoder, setLabel and setArgumentTable, the begin and end timestamps on the encoder, then popDebugGroup on
    // the command buffer; the encoder, which runs the pass's clear and store, stays open until the list ends.
    CHECK(state.counts.nativeRecordCalls == calls + 1 + 2 + label + 2 + 1);
    CHECK(state.counts.encodersOpened == encoders + 1);
    CHECK(state.scopeLog.back().timestamp != core::kNoTimestamp);
    submitFrame(device, std::array{ list });
    retireOpenFrame(device);
    const std::span<const ScopeTiming> report = device.scopeTimings();
    REQUIRE(report.size() == 1);
    CHECK(report[0].label == "test.empty");
    CHECK(std::isfinite(report[0].gpuMilliseconds));
    CHECK(report[0].gpuMilliseconds >= 0.0);
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a pass that splits writes two timestamps, the begin on its first render encoder and the end on its last", "[metal4]")
{
    Device device = *createDevice({ .framesInFlight = 2 });
    const Drawing drawing(device);
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    const uint64_t label = test::encoderLabel(list);
    claimArenaPage(list);
    const core::CommandListCore& state = core::coreOf(list);
    const uint64_t encoders = state.counts.encodersOpened;
    const metal4::DrawState& draw = CommandListImpl::of(list).draw;
    drawing.begin(list, "test.pass");
    // The pinned counts include the timestamp writes, so every scope must hold a pair.
    test::requireTimed(list);
    CHECK(draw.passBegin == state.scopeLog.back().timestamp);
    // The first draw opens the pass's render encoder: the factory, the label and the table, the begin timestamp, the
    // pipeline with its winding, the viewport and the scissor, then the draw.
    uint64_t calls = state.counts.nativeRecordCalls;
    list.draw(3);
    CHECK(state.counts.encodersOpened == encoders + 1);
    CHECK(state.counts.nativeRecordCalls == calls + 2 + label + 1 + 4 + 1);
    CHECK(draw.passBegin == core::kNoTimestamp);
    // A counted draw splits the pass with no timestamp: the render encoder ends (its Store attachment costs no call), and the
    // kernels' encoder opens (factory, label) and runs the writer kernel (pipeline, kernel table, parameters, dispatch).
    calls = state.counts.nativeRecordCalls;
    drawing.counted(device, list);
    CHECK(state.counts.encodersOpened == encoders + 2);
    CHECK(state.counts.nativeRecordCalls == calls + 1 + 1 + label + 4);
    // The resumed render encoder writes none either.
    list.draw(3);
    CHECK(state.counts.encodersOpened == encoders + 3);
    CHECK(draw.passBegin == core::kNoTimestamp);
    calls = state.counts.nativeRecordCalls;
    list.endRenderPass();
    // The end timestamp on the resumed encoder and the pop on the command buffer; the encoder stays open.
    CHECK(state.counts.nativeRecordCalls == calls + 1 + 1);
    submitFrame(device, std::array{ list });
    retireOpenFrame(device);
    const std::span<const ScopeTiming> report = device.scopeTimings();
    REQUIRE(report.size() == 1);
    CHECK(report[0].label == "test.pass");
    CHECK(std::isfinite(report[0].gpuMilliseconds));
    CHECK(report[0].gpuMilliseconds > 0.0);
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a scope that opens a pass's render encoder writes after the pass's begin timestamp, on the same encoder", "[metal4]")
{
    Device device = *createDevice({ .framesInFlight = 2 });
    const Drawing drawing(device);
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    const uint64_t label = test::encoderLabel(list);
    claimArenaPage(list);
    const core::CommandListCore& state = core::coreOf(list);
    drawing.begin(list, "test.pass");
    const uint32_t passPair = state.scopeLog.back().timestamp;
    uint64_t calls = state.counts.nativeRecordCalls;
    list.beginScope("test.inner");
    // The pinned counts include the timestamp writes, so every scope must hold a pair.
    test::requireTimed(list);
    // The scope opened the render encoder (factory, label, table), which wrote the pass's begin timestamp and replayed the
    // pipeline with its winding, the viewport and the scissor before the scope's group and begin timestamp.
    CHECK(state.scopeLog.back().timestamp == passPair + 2);
    CHECK(state.counts.nativeRecordCalls == calls + 2 + label + 1 + 4 + 1 + 1);
    CHECK(CommandListImpl::of(list).draw.passBegin == core::kNoTimestamp);
    list.draw(3);
    calls = state.counts.nativeRecordCalls;
    list.endScope();
    // The scope's end timestamp and its pop, on the encoder.
    CHECK(state.counts.nativeRecordCalls == calls + 2);
    calls = state.counts.nativeRecordCalls;
    list.endRenderPass();
    // The pass's end timestamp and the pop on the command buffer; the encoder stays open.
    CHECK(state.counts.nativeRecordCalls == calls + 2);
    submitFrame(device, std::array{ list });
    retireOpenFrame(device);
    const std::span<const ScopeTiming> report = device.scopeTimings();
    REQUIRE(report.size() == 2);
    CHECK(report[0].label == "test.pass");
    CHECK(report[1].label == "test.inner");
    for (const ScopeTiming& timing : report)
    {
        CHECK(std::isfinite(timing.gpuMilliseconds));
        CHECK(timing.gpuMilliseconds >= 0.0);
    }
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a scope inside a pass writes its timestamps on the render encoder; a split writes none", "[metal4]")
{
    Device device = *createDevice({ .framesInFlight = 2 });
    const Drawing drawing(device);
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    claimArenaPage(list);
    const core::CommandListCore& state = core::coreOf(list);
    const metal4::ListState& native = CommandListImpl::of(list).state;
    drawing.begin(list, "test.pass");
    const uint32_t passPair = state.scopeLog.back().timestamp;
    REQUIRE(passPair != core::kNoTimestamp);
    // The first draw opens the render encoder, so the scope's calls are its own.
    list.draw(3);
    const uint64_t encoders = state.counts.encodersOpened;
    uint64_t calls = state.counts.nativeRecordCalls;
    list.beginScope("test.inner");
    // The pinned counts include the timestamp writes, so every scope must hold a pair.
    test::requireTimed(list);
    // pushDebugGroup and the begin timestamp, both on the open render encoder, which stays open.
    CHECK(state.counts.nativeRecordCalls == calls + 2);
    CHECK(state.counts.encodersOpened == encoders);
    const uint32_t pair = state.scopeLog.back().timestamp;
    CHECK(pair == passPair + 2);
    REQUIRE(native.scopeTimestamps.size() == 2);
    CHECK(native.scopeTimestamps.back() == pair);
    // A counted draw splits the pass: the split pops the scope's group and the resumed encoder pushes it again, two calls more
    // than the same split with no scope open, and no timestamp.
    calls = state.counts.nativeRecordCalls;
    drawing.counted(device, list);
    list.draw(3);
    const uint64_t scopedSplit = state.counts.nativeRecordCalls - calls;
    CHECK(state.counts.encodersOpened == encoders + 2);
    calls = state.counts.nativeRecordCalls;
    list.endScope();
    // The end timestamp on the resumed render encoder, then popDebugGroup.
    CHECK(state.counts.nativeRecordCalls == calls + 2);
    CHECK(native.scopeTimestamps.size() == 1);
    calls = state.counts.nativeRecordCalls;
    drawing.counted(device, list);
    list.draw(3);
    const uint64_t plainSplit = state.counts.nativeRecordCalls - calls;
    CHECK(scopedSplit == plainSplit + 2);
    // The split claimed no pair: the next scope's pair follows the inner scope's.
    list.beginScope("test.after");
    CHECK(state.scopeLog.back().timestamp == pair + 2);
    list.endScope();
    list.endRenderPass();
    CHECK(native.scopeTimestamps.empty());
    submitFrame(device, std::array{ list });
    retireOpenFrame(device);
    const std::span<const ScopeTiming> report = device.scopeTimings();
    REQUIRE(report.size() == 3);
    const std::array<std::string_view, 3> labels{ "test.pass", "test.inner", "test.after" };
    const std::array<uint32_t, 3> depths{ 0, 1, 1 };
    for (size_t i = 0; i < report.size(); ++i)
    {
        CAPTURE(i);
        CHECK(report[i].label == labels[i]);
        CHECK(report[i].depth == depths[i]);
        CHECK(std::isfinite(report[i].gpuMilliseconds));
        CHECK(report[i].gpuMilliseconds >= 0.0);
    }
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("an upscale writes its two timestamps around the carrier and the MetalFX encode", "[metal4]")
{
    test::UpscaleKit kit;
    Device device = kit.kit.device;
    CommandList list = kit.kit.list;
    const uint64_t label = test::encoderLabel(list);
    claimArenaPage(list);
    const core::CommandListCore& state = core::coreOf(list);
    const uint64_t calls = state.counts.nativeRecordCalls;
    kit.params.label = "test.upscale";
    kit.upscale();
    // The pinned counts include the timestamp writes, so every scope must hold a pair.
    test::requireTimed(list);
    // pushDebugGroup, the begin timestamp, the carrier's five calls, the scaler's fourteen setters, encodeToCommandBuffer,
    // the end timestamp and popDebugGroup.
    CHECK(state.counts.nativeRecordCalls == calls + 1 + 1 + 4 + label + 14 + 1 + 1 + 1);
    const uint32_t pair = state.scopeLog.back().timestamp;
    CHECK(pair != core::kNoTimestamp);
    CHECK(pair % 2 == 0);
    CHECK(CommandListImpl::of(list).state.scopeTimestamps.empty());
    submitFrame(device, std::array{ list });
    retireOpenFrame(device);
    const std::span<const ScopeTiming> report = device.scopeTimings();
    REQUIRE(report.size() == 1);
    CHECK(report[0].label == "test.upscale");
    CHECK(std::isfinite(report[0].gpuMilliseconds));
    CHECK(report[0].gpuMilliseconds > 0.0);
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a frame of passes, scopes inside them and upscales leaves no scope untimed", "[metal4]")
{
    g_warnings.clear();
    setMessageCallback(collectWarning, nullptr);
    {
        test::UpscaleKit kit;
        Device device = kit.kit.device;
        CommandList list = kit.kit.list;
        const Drawing drawing(device);
        drawing.begin(list, "test.pass");
        list.beginScope("test.inner");
        list.draw(3);
        list.endScope();
        list.endRenderPass();
        kit.upscale();
        submitFrame(device, std::array{ list });
        retireOpenFrame(device);
        CHECK(device.scopeTimings().size() == 3);
        device.endFrame();
        destroyDevice(device);
    }
    setMessageCallback(nullptr, nullptr);
    CHECK(g_warnings.empty());
}

//======================================================================================================================
TEST_CASE("nested scopes inside a pass end on the encoder a split resumed, inner then outer", "[metal4]")
{
    Device device = *createDevice({ .framesInFlight = 2 });
    const Drawing drawing(device);
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    claimArenaPage(list);
    const core::CommandListCore& state = core::coreOf(list);
    const metal4::ListState& native = CommandListImpl::of(list).state;
    drawing.begin(list, "test.pass");
    list.draw(3);
    list.beginScope("test.outer");
    const uint32_t outer = state.scopeLog.back().timestamp;
    list.beginScope("test.inner");
    // The pinned counts include the timestamp writes, so every scope must hold a pair.
    test::requireTimed(list);
    const uint32_t inner = state.scopeLog.back().timestamp;
    REQUIRE(outer != core::kNoTimestamp);
    REQUIRE(inner == outer + 2);
    // The split ends the encoder that holds both begin timestamps; the draw resumes the pass on a new one.
    const uint64_t encoders = state.counts.encodersOpened;
    drawing.counted(device, list);
    list.draw(3);
    CHECK(state.counts.encodersOpened == encoders + 2);
    REQUIRE(native.scopeTimestamps.size() == 3);
    // Each end timestamp and pop goes on the resumed encoder, which stays open: the inner scope's first, then the outer's.
    uint64_t calls = state.counts.nativeRecordCalls;
    list.endScope();
    CHECK(state.counts.nativeRecordCalls == calls + 2);
    CHECK(native.scopeTimestamps.back() == outer);
    calls = state.counts.nativeRecordCalls;
    list.endScope();
    CHECK(state.counts.nativeRecordCalls == calls + 2);
    CHECK(state.counts.encodersOpened == encoders + 2);
    CHECK(CommandListImpl::of(list).encoders.kind() == metal4::EncoderKind::Render);
    REQUIRE(native.scopeTimestamps.size() == 1);
    list.endRenderPass();
    submitFrame(device, std::array{ list });
    retireOpenFrame(device);
    const std::span<const ScopeTiming> report = device.scopeTimings();
    REQUIRE(report.size() == 3);
    const std::array<std::string_view, 3> labels{ "test.pass", "test.outer", "test.inner" };
    const std::array<uint32_t, 3> depths{ 0, 1, 2 };
    for (size_t i = 0; i < report.size(); ++i)
    {
        CAPTURE(i);
        CHECK(report[i].label == labels[i]);
        CHECK(report[i].depth == depths[i]);
        CHECK(std::isfinite(report[i].gpuMilliseconds));
        CHECK(report[i].gpuMilliseconds >= 0.0);
    }
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a pass, a scope inside it and an upscale past the slot's capacity write no timestamp, report zero and warn once", "[metal4]")
{
    constexpr uint32_t kCapacity = metal4::Timestamps::kSlotHeaps * metal4::Timestamps::kHeapEntries / 2;
    g_warnings.clear();
    setMessageCallback(collectWarning, nullptr);
    {
        test::UpscaleKit kit;
        Device device = kit.kit.device;
        CommandList list = kit.kit.list;
        claimArenaPage(list);
        const Drawing drawing(device);
        const Buffer a = *device.createBuffer({ .size = 256, .label = "test.a" });
        const Buffer b = *device.createBuffer({ .size = 256, .label = "test.b" });
        const core::CommandListCore& state = core::coreOf(list);
        // The native calls of each site: the upscale, the pass's begin through its first draw, whose encoder writes the
        // begin timestamp, the in-pass scope's begin and end, and the pass's end.
        struct Sites
        {
            uint64_t upscale = 0;
            uint64_t begin = 0;
            uint64_t scopeBegin = 0;
            uint64_t scopeEnd = 0;
            uint64_t end = 0;
        };
        const auto record = [&](const char* suffix, Upscaler upscaler) {
            Sites sites;
            uint64_t calls = state.counts.nativeRecordCalls;
            const auto delta = [&](uint64_t& site) {
                site = state.counts.nativeRecordCalls - calls;
                calls = state.counts.nativeRecordCalls;
            };
            kit.params.label = "test.upscale";
            list.upscale(upscaler, kit.params);
            delta(sites.upscale);
            // The copy takes the compute wait the upscale owes and the pass's encoder its render wait, in both rounds, so the
            // sites of the two rounds owe the same fences.
            list.copyBuffer(a, 0, b, 0, 256);
            calls = state.counts.nativeRecordCalls;
            const std::array colors{ ColorAttachment{ .texture = drawing.target, .load = LoadOp::Clear } };
            list.beginRenderPass(RenderPassDesc{ .colors = colors, .area = { 0, 0, kEdge, kEdge }, .label = std::string("test.pass") + suffix });
            list.setPipeline(drawing.pipeline);
            list.setViewport(Viewport{ .width = static_cast<float>(kEdge), .height = static_cast<float>(kEdge) });
            list.setScissor(Rect2D{ 0, 0, kEdge, kEdge });
            list.draw(3);
            delta(sites.begin);
            list.beginScope("test.inner");
            delta(sites.scopeBegin);
            list.endScope();
            delta(sites.scopeEnd);
            list.endRenderPass();
            delta(sites.end);
            return sites;
        };
        const Sites timed = record(".timed", kit.upscaler);
        // The three sites claimed three pairs; the rest of the slot's capacity goes to scopes outside a pass.
        recordScopes(list, kCapacity - 3);
        // An upscaler upscales once a frame, so the second round uses another.
        const Sites untimed = record(".untimed", *device.createUpscaler(kit.desc));
        CHECK(untimed.begin == timed.begin - 1);
        CHECK(untimed.scopeBegin == timed.scopeBegin - 1);
        CHECK(untimed.scopeEnd == timed.scopeEnd - 1);
        CHECK(untimed.end == timed.end - 1);
        CHECK(untimed.upscale == timed.upscale - 2);
        const std::vector<core::ScopeRecord>& log = state.scopeLog;
        REQUIRE(log.size() == kCapacity + 3);
        for (size_t i = log.size() - 3; i < log.size(); ++i)
        {
            CHECK(log[i].timestamp == core::kNoTimestamp);
        }
        submitFrame(device, std::array{ list });
        retireOpenFrame(device);
        const std::span<const ScopeTiming> report = device.scopeTimings();
        REQUIRE(report.size() == kCapacity + 3);
        CHECK(report[0].label == "test.upscale");
        CHECK(report[0].gpuMilliseconds > 0.0);
        CHECK(report[1].label == "test.pass.timed");
        CHECK(report[1].gpuMilliseconds > 0.0);
        const std::array<std::string_view, 3> labels{ "test.upscale", "test.pass.untimed", "test.inner" };
        for (size_t i = 0; i < labels.size(); ++i)
        {
            const ScopeTiming& timing = report[kCapacity + i];
            CAPTURE(i);
            CHECK(timing.label == labels[i]);
            CHECK(timing.gpuMilliseconds == 0.0);
        }
        REQUIRE(g_warnings.size() == 1);
        CHECK(g_warnings[0].find("3 scope") != std::string::npos);
        device.endFrame();
        destroyDevice(device);
    }
    setMessageCallback(nullptr, nullptr);
}

//======================================================================================================================
TEST_CASE("requireTimed fails a case whose list recorded a scope without a timestamp pair", "[metal4][!shouldfail]")
{
    // The guard of the count pins must fail, not pass, when a pair is missing: past the slot's capacity none is claimed.
    constexpr uint32_t kCapacity = metal4::Timestamps::kSlotHeaps * metal4::Timestamps::kHeapEntries / 2;
    Device device = *createDevice({ .framesInFlight = 2 });
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    recordScopes(list, kCapacity + 1);
    // A failing guard throws here: the list is submitted, the frame closed and the device destroyed before the
    // failure propagates, so its counter heaps are not leaked.
    try
    {
        test::requireTimed(list);
    }
    catch (...)
    {
        submitFrame(device, std::array{ list });
        device.endFrame();
        destroyDevice(device);
        throw;
    }
    submitFrame(device, std::array{ list });
    device.endFrame();
    destroyDevice(device);
}
