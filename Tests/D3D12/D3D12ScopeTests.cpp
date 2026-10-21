//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12ScopeTests.cpp
/// @brief Tests D3D12 scopes: their events and timestamp queries, the list's query heaps, the resolve at end and the durations.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/CommandLists.h"
#include "Core/DeviceCore.h"
#include "Core/ScopeRecord.h"
#include "D3D12Common.h"
#include "D3D12Device.h"
#include "D3D12ListState.h"
#include "D3D12Timestamps.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace rojoRHI;

namespace {
    /// The scopes one query heap times: two queries each.
    constexpr uint32_t kHeapScopes = d3d12::ListTimestamps::kHeapQueries / 2;

    /// The scopes each list of the threaded case records.
    constexpr uint32_t kThreadScopes = 3;

    /// Guards `g_messages`, which the message callback may fill from any thread.
    std::mutex g_messagesMutex;

    /// The messages at `Warning` or above the library reported while a case collected them.
    std::vector<std::string> g_messages;

    //==================================================================================================================
    /// Keeps `message` when it is a warning or an error. It may run on any thread.
    void collectMessage(MessageSeverity severity, std::string_view message, void*)
    {
        if (severity == MessageSeverity::Info)
        {
            return;
        }
        const std::lock_guard lock(g_messagesMutex);
        g_messages.emplace_back(message);
    }

    /// Collects the library's warnings and errors, the debug layer's included, into `g_messages`, emptied first, while it
    /// lives, and removes the callback when it goes, however the case ends.
    class MessageCollection
    {
      public:
        MessageCollection();
        ~MessageCollection();

        //==============================================================================================================
        MessageCollection(const MessageCollection&) = delete;

        //==============================================================================================================
        MessageCollection& operator=(const MessageCollection&) = delete;

        /// Returns the messages collected so far, and lists them for the log.
        std::vector<std::string> all() const;
    };

    //==================================================================================================================
    MessageCollection::MessageCollection()
    {
        {
            const std::lock_guard lock(g_messagesMutex);
            g_messages.clear();
        }
        setMessageCallback(collectMessage, nullptr);
    }

    //==================================================================================================================
    MessageCollection::~MessageCollection()
    {
        setMessageCallback(nullptr, nullptr);
    }

    //==================================================================================================================
    std::vector<std::string> MessageCollection::all() const
    {
        const std::lock_guard lock(g_messagesMutex);
        for (const std::string& message : g_messages)
        {
            UNSCOPED_INFO("message: " << message);
        }
        return g_messages;
    }

    //==================================================================================================================
    /// Returns a new device that keeps `framesInFlight` frames in flight.
    Device makeDevice(uint32_t framesInFlight = 3)
    {
        DeviceDesc desc;
        desc.framesInFlight = framesInFlight;
        Result<Device> created = createDevice(desc);
        REQUIRE(created.has_value());
        return *created;
    }

    //==================================================================================================================
    /// Returns a new fence at zero on `device`, labelled `label`.
    Fence makeFence(Device device, std::string_view label)
    {
        Result<Fence> created = device.createFence(0, label);
        REQUIRE(created.has_value());
        return *created;
    }

    //==================================================================================================================
    /// Returns the native state of `list`. A case keeps the reference past the list's submission rather than asking the
    /// handle again.
    d3d12::ListState& stateOf(CommandList list)
    {
        return CommandListImpl::of(list).state;
    }

    //==================================================================================================================
    /// Returns the native calls `list` has recorded since its acquisition.
    uint64_t recordCalls(CommandList list)
    {
        return core::coreOf(list).counts.nativeRecordCalls;
    }

    //==================================================================================================================
    /// Returns the scope `list` recorded last.
    const core::ScopeRecord& lastScope(CommandList list)
    {
        const std::vector<core::ScopeRecord>& log = core::coreOf(list).scopeLog;
        REQUIRE(!log.empty());
        return log.back();
    }

    //==================================================================================================================
    /// Returns the `ScopeRecord::timestamp` of the pair at query `pair` of the list at `ordinal` in its pool.
    uint32_t stamp(uint32_t ordinal, uint32_t pair)
    {
        return (ordinal << 16) | pair;
    }

    //==================================================================================================================
    /// Records `count` scopes with the core's default label, one after another, on `list`; a default label takes no
    /// frame-arena page, so the scopes create nothing but query heaps.
    void recordScopes(CommandList list, uint32_t count)
    {
        for (uint32_t scope = 0; scope < count; ++scope)
        {
            list.beginScope("");
            list.endScope();
        }
    }

    //==================================================================================================================
    /// Ends every list of `lists` and submits it to the queue of its type, joined into the open frame: the Compute lists
    /// signal value `value` of `computeDone` and the Copy lists of `copyDone`, and one Graphics submission, with the
    /// Graphics lists, waits for what they signalled.
    void submitJoined(Device device, std::span<const CommandList> lists, Fence computeDone, Fence copyDone, uint64_t value)
    {
        std::vector<CommandList> graphics;
        std::vector<CommandList> compute;
        std::vector<CommandList> copy;
        for (CommandList list : lists)
        {
            list.end();
            switch (core::coreOf(list).type)
            {
            case QueueType::Graphics: graphics.push_back(list); break;
            case QueueType::Compute: compute.push_back(list); break;
            case QueueType::Copy: copy.push_back(list); break;
            }
        }
        std::vector<FenceValue> joins;
        if (!compute.empty())
        {
            const std::array<FenceValue, 1> signal{ FenceValue{ computeDone, value } };
            device.queue(QueueType::Compute).submit(compute, {}, signal);
            joins.push_back(signal[0]);
        }
        if (!copy.empty())
        {
            const std::array<FenceValue, 1> signal{ FenceValue{ copyDone, value } };
            device.queue(QueueType::Copy).submit(copy, {}, signal);
            joins.push_back(signal[0]);
        }
        device.queue(QueueType::Graphics).submit(graphics, joins, {});
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

    //==================================================================================================================
    /// Checks that `timing` reports a duration a GPU can measure: finite and not negative.
    void checkDuration(const ScopeTiming& timing)
    {
        INFO("scope " << std::string(timing.label));
        CHECK(std::isfinite(timing.gpuMilliseconds));
        CHECK(timing.gpuMilliseconds >= 0.0);
    }

    //==================================================================================================================
    /// Records a global barrier natively on `list`, a Graphics or Compute list: unordered-access writes of compute shaders
    /// before it complete ahead of those after it. It is work the GPU orders, recorded without a RojoRHI entry.
    void nativeBarrier(CommandList list)
    {
        D3D12_GLOBAL_BARRIER barrier = {};
        barrier.SyncBefore = D3D12_BARRIER_SYNC_COMPUTE_SHADING;
        barrier.SyncAfter = D3D12_BARRIER_SYNC_COMPUTE_SHADING;
        barrier.AccessBefore = D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
        barrier.AccessAfter = D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
        D3D12_BARRIER_GROUP group = {};
        group.Type = D3D12_BARRIER_TYPE_GLOBAL;
        group.NumBarriers = 1;
        group.pGlobalBarriers = &barrier;
        stateOf(list).list->Barrier(1, &group);
    }
} // namespace

//======================================================================================================================
TEST_CASE("the device reads the timestamp frequency of each queue that writes timestamps", "[d3d12]")
{
    Device device = makeDevice();
    DeviceImpl& impl = DeviceImpl::of(device);
    for (const QueueType type : { QueueType::Graphics, QueueType::Compute, QueueType::Copy })
    {
        INFO("queue type " << static_cast<int>(type));
        const double frequency = impl.timestamps.frequency(type);
        if (type == QueueType::Copy && !impl.features.copyQueueTimestamps)
        {
            CHECK(frequency == 0.0);
            WARN("the copy queue writes no timestamps on this device");
            continue;
        }
        UINT64 native = 0;
        REQUIRE(SUCCEEDED(impl.queues[static_cast<size_t>(type)].native->GetTimestampFrequency(&native)));
        CHECK(native > 0u);
        CHECK(frequency == static_cast<double>(native));
        WARN("queue type " << static_cast<int>(type) << " ticks at " << native << " Hz");
    }
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a scope records an event and a timestamp at each end", "[d3d12]")
{
    Device device = makeDevice();
    const MessageCollection messages;
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    const d3d12::ListState& state = stateOf(list);
    const uint64_t before = recordCalls(list);
    // BeginEvent, then EndQuery at the pair's first query.
    list.beginScope("test.scope");
    CHECK(recordCalls(list) - before == 2u);
    CHECK(lastScope(list).timestamp == stamp(state.ordinal, 0));
    CHECK(state.timestamps.next == 2u);
    CHECK(state.timestamps.open == std::vector<uint32_t>{ 0 });
    CHECK(state.timestamps.heaps.size() == 1u);
    // EndQuery at the pair's second query, then EndEvent.
    list.endScope();
    CHECK(recordCalls(list) - before == 4u);
    CHECK(state.timestamps.open.empty());
    list.end();
    device.queue(QueueType::Graphics).submit(std::span(&list, 1), {}, {});
    device.endFrame();
    CHECK(messages.all().empty());
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("end resolves each query heap the recording used, once", "[d3d12]")
{
    Device device = makeDevice();
    const MessageCollection messages;
    device.beginFrame();
    CommandList one = device.acquireCommandList(QueueType::Graphics, "test.one");
    CommandList many = device.acquireCommandList(QueueType::Graphics, "test.many");
    const d3d12::ListState& oneState = stateOf(one);
    const d3d12::ListState& manyState = stateOf(many);
    recordScopes(one, 1);
    // A heap holds 128 pairs, so the 129th scope needs a second heap and its readback buffer.
    recordScopes(many, kHeapScopes);
    CHECK(manyState.timestamps.heaps.size() == 1u);
    const Counters before = device.counters();
    many.beginScope("");
    const Counters after = device.counters();
    CHECK(after.nativeCreateCalls - before.nativeCreateCalls == 2u);
    CHECK(after.gpuAllocations - before.gpuAllocations == 1u);
    CHECK(after.nativeHostCalls - before.nativeHostCalls == 1u);
    CHECK(manyState.timestamps.heaps.size() == 2u);
    CHECK(lastScope(many).timestamp == stamp(manyState.ordinal, d3d12::ListTimestamps::kHeapQueries));
    many.endScope();
    // One ResolveQueryData per heap used, then Close.
    const uint64_t oneBefore = recordCalls(one);
    one.end();
    CHECK(recordCalls(one) - oneBefore == 2u);
    const uint64_t manyBefore = recordCalls(many);
    many.end();
    CHECK(recordCalls(many) - manyBefore == 3u);
    const std::array<CommandList, 2> lists = { one, many };
    device.queue(QueueType::Graphics).submit(lists, {}, {});
    retireOpenFrame(device);
    const std::span<const ScopeTiming> report = device.scopeTimings();
    REQUIRE(report.size() == 1u + kHeapScopes + 1u);
    for (const ScopeTiming& timing : report)
    {
        checkDuration(timing);
    }
    // Both pairs of the second heap's first scope were resolved into its readback buffer.
    CHECK(manyState.timestamps.heaps[1].mapped[0] != 0u);
    CHECK(manyState.timestamps.heaps[1].mapped[1] >= manyState.timestamps.heaps[1].mapped[0]);
    CHECK(oneState.timestamps.heaps.size() == 1u);
    device.endFrame();
    CHECK(messages.all().empty());
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a list that records no scope resolves nothing", "[d3d12]")
{
    Device device = makeDevice();
    const MessageCollection messages;
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    const uint64_t before = recordCalls(list);
    list.end();
    // Close alone.
    CHECK(recordCalls(list) - before == 1u);
    CHECK(stateOf(list).timestamps.heaps.empty());
    device.queue(QueueType::Graphics).submit(std::span(&list, 1), {}, {});
    device.endFrame();
    CHECK(messages.all().empty());
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("nested scopes stamp pairs in order and end each at its own odd index", "[d3d12]")
{
    Device device = makeDevice();
    const MessageCollection messages;
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    const d3d12::ListState& state = stateOf(list);
    list.beginScope("test.outer");
    list.beginScope("test.inner");
    const std::vector<core::ScopeRecord>& log = core::coreOf(list).scopeLog;
    REQUIRE(log.size() == 2u);
    CHECK(log[0].timestamp == stamp(state.ordinal, 0));
    CHECK(log[1].timestamp == stamp(state.ordinal, 2));
    CHECK(state.timestamps.open == std::vector<uint32_t>{ 0, 2 });
    list.endScope();
    CHECK(state.timestamps.open == std::vector<uint32_t>{ 0 });
    list.endScope();
    CHECK(state.timestamps.open.empty());
    CHECK(state.timestamps.next == 4u);
    list.end();
    device.queue(QueueType::Graphics).submit(std::span(&list, 1), {}, {});
    retireOpenFrame(device);
    // The readback buffer starts zeroed, so a tick in each of the four queries shows that each end reached its own query.
    const uint64_t* ticks = state.timestamps.heaps[0].mapped;
    for (uint32_t query = 0; query < 4; ++query)
    {
        INFO("query " << query);
        CHECK(ticks[query] != 0u);
    }
    // The inner scope began after the outer one and ended before it.
    CHECK(ticks[2] >= ticks[0]);
    CHECK(ticks[3] >= ticks[2]);
    CHECK(ticks[1] >= ticks[3]);
    const std::span<const ScopeTiming> report = device.scopeTimings();
    REQUIRE(report.size() == 2u);
    CHECK(report[0].label == "test.outer");
    CHECK(report[0].depth == 0u);
    CHECK(report[1].label == "test.inner");
    CHECK(report[1].depth == 1u);
    checkDuration(report[0]);
    checkDuration(report[1]);
    CHECK(report[0].gpuMilliseconds >= report[1].gpuMilliseconds);
    device.endFrame();
    CHECK(messages.all().empty());
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("scopes report finite non-negative durations with their labels, depths and queue", "[d3d12]")
{
    Device device = makeDevice();
    const MessageCollection messages;
    const Fence computeDone = makeFence(device, "test.computeDone");
    const Fence copyDone = makeFence(device, "test.copyDone");
    device.beginFrame();
    CommandList graphics = device.acquireCommandList(QueueType::Graphics, "test.graphics");
    CommandList compute = device.acquireCommandList(QueueType::Compute, "test.compute");
    graphics.beginScope("test.outer");
    graphics.beginScope("test.empty");
    graphics.endScope();
    graphics.beginScope("test.barrier");
    nativeBarrier(graphics);
    graphics.endScope();
    graphics.endScope();
    compute.beginScope("test.compute.empty");
    compute.endScope();
    compute.beginScope("test.compute.barrier");
    nativeBarrier(compute);
    compute.endScope();
    const std::array<CommandList, 2> lists = { graphics, compute };
    submitJoined(device, lists, computeDone, copyDone, 1);
    retireOpenFrame(device);
    const std::span<const ScopeTiming> report = device.scopeTimings();
    // Graphics scopes first, in the order they began, then Compute's.
    struct Expected
    {
        std::string_view label;
        uint32_t depth;
        QueueType queue;
    };
    const std::array<Expected, 5> expected = { Expected{ "test.outer", 0, QueueType::Graphics },
                                               Expected{ "test.empty", 1, QueueType::Graphics },
                                               Expected{ "test.barrier", 1, QueueType::Graphics },
                                               Expected{ "test.compute.empty", 0, QueueType::Compute },
                                               Expected{ "test.compute.barrier", 0, QueueType::Compute } };
    REQUIRE(report.size() == expected.size());
    for (size_t i = 0; i < expected.size(); ++i)
    {
        INFO("scope " << i);
        CHECK(report[i].label == expected[i].label);
        CHECK(report[i].depth == expected[i].depth);
        CHECK(report[i].queue == expected[i].queue);
        CHECK(report[i].frame == 1u);
        checkDuration(report[i]);
    }
    device.endFrame();
    CHECK(messages.all().empty());
    device.destroy(computeDone);
    device.destroy(copyDone);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a Copy list's scope is untimed when the device has no copy-queue timestamps, else timed", "[d3d12]")
{
    // The second pass clears the feature before the first Copy list exists, so the untimed path runs on a device that
    // reports copy-queue timestamps too.
    for (const bool disable : { false, true })
    {
        INFO("feature cleared " << disable);
        Device device = makeDevice();
        bool& feature = DeviceImpl::of(device).features.copyQueueTimestamps;
        if (disable)
        {
            feature = false;
        }
        const bool timed = feature;
        INFO("copy-queue timestamps " << timed);
        const MessageCollection messages;
        const Fence computeDone = makeFence(device, "test.computeDone");
        const Fence copyDone = makeFence(device, "test.copyDone");
        device.beginFrame();
        CommandList copy = device.acquireCommandList(QueueType::Copy, "test.copy");
        const d3d12::ListState& state = stateOf(copy);
        const uint64_t before = recordCalls(copy);
        copy.beginScope("test.copy.scope");
        // BeginEvent, and EndQuery when the queue writes timestamps.
        CHECK(recordCalls(copy) - before == (timed ? 2u : 1u));
        CHECK(lastScope(copy).timestamp == (timed ? stamp(state.ordinal, 0) : core::kNoTimestamp));
        CHECK(state.timestamps.heaps.size() == (timed ? 1u : 0u));
        copy.endScope();
        CHECK(recordCalls(copy) - before == (timed ? 4u : 2u));
        submitJoined(device, std::span(&copy, 1), computeDone, copyDone, 1);
        retireOpenFrame(device);
        const std::span<const ScopeTiming> report = device.scopeTimings();
        REQUIRE(report.size() == 1u);
        CHECK(report[0].queue == QueueType::Copy);
        CHECK(report[0].label == "test.copy.scope");
        checkDuration(report[0]);
        const std::vector<std::string> collected = messages.all();
        if (timed)
        {
            CHECK(collected.empty());
        }
        else
        {
            // The frame's one warning names the frame and the reason.
            CHECK(report[0].gpuMilliseconds == 0.0);
            REQUIRE(collected.size() == 1u);
            CHECK(collected[0].find("frame 1") != std::string::npos);
            CHECK(collected[0].find("copy") != std::string::npos);
        }
        device.endFrame();
        device.destroy(computeDone);
        device.destroy(copyDone);
        destroyDevice(device);
    }
}

//======================================================================================================================
TEST_CASE("a list reused in the next frame of its slot starts its pairs from zero", "[d3d12]")
{
    constexpr uint32_t kFramesInFlight = 2;
    Device device = makeDevice(kFramesInFlight);
    const MessageCollection messages;
    std::vector<const d3d12::ListState*> states;
    for (uint64_t frame = 1; frame <= 6; ++frame)
    {
        INFO("frame " << frame);
        device.beginFrame();
        if (frame > kFramesInFlight)
        {
            // The frame that reused the slot reported the two scopes of the frame it waited for.
            const std::span<const ScopeTiming> report = device.scopeTimings();
            REQUIRE(report.size() == 2u);
            for (const ScopeTiming& timing : report)
            {
                CHECK(timing.frame == frame - kFramesInFlight);
                checkDuration(timing);
            }
        }
        CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
        const d3d12::ListState& state = stateOf(list);
        states.push_back(&state);
        CHECK(state.timestamps.next == 0u);
        CHECK(state.timestamps.open.empty());
        const uint64_t before = device.counters().nativeCreateCalls;
        recordScopes(list, 2);
        // A list's first recording makes its first heap and its readback buffer; the list's later recordings reuse them.
        CHECK(device.counters().nativeCreateCalls - before == (frame <= kFramesInFlight ? 2u : 0u));
        const std::vector<core::ScopeRecord>& log = core::coreOf(list).scopeLog;
        REQUIRE(log.size() == 2u);
        CHECK(log[0].timestamp == stamp(state.ordinal, 0));
        CHECK(log[1].timestamp == stamp(state.ordinal, 2));
        list.end();
        device.queue(QueueType::Graphics).submit(std::span(&list, 1), {}, {});
        device.endFrame();
    }
    for (size_t i = kFramesInFlight; i < states.size(); ++i)
    {
        CHECK(states[i] == states[i - kFramesInFlight]);
    }
    CHECK(messages.all().empty());
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a non-ASCII label reaches the event intact and an empty label uses the core's default", "[d3d12]")
{
    // "test." followed by U+00E9, U+6F22 and U+1F600 in UTF-8.
    constexpr std::string_view kLabel = "test.\xC3\xA9\xE6\xBC\xA2\xF0\x9F\x98\x80";
    std::wstring expected = L"test.";
    expected += static_cast<wchar_t>(0x00E9);
    expected += static_cast<wchar_t>(0x6F22);
    expected += static_cast<wchar_t>(0xD83D);
    expected += static_cast<wchar_t>(0xDE00);
    Device device = makeDevice();
    const MessageCollection messages;
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    const d3d12::ListState& state = stateOf(list);
    list.beginScope(kLabel);
    CHECK(state.scratch == expected);
    list.endScope();
    list.beginScope("");
    CHECK(state.scratch == L"rojorhi.scope.unnamed");
    CHECK(lastScope(list).label == "rojorhi.scope.unnamed");
    list.endScope();
    list.end();
    device.queue(QueueType::Graphics).submit(std::span(&list, 1), {}, {});
    retireOpenFrame(device);
    const std::span<const ScopeTiming> report = device.scopeTimings();
    REQUIRE(report.size() == 2u);
    CHECK(report[0].label == kLabel);
    CHECK(report[1].label == "rojorhi.scope.unnamed");
    device.endFrame();
    CHECK(messages.all().empty());
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("lists on four threads time their scopes in query heaps of their own", "[d3d12]")
{
    Device device = makeDevice();
    const MessageCollection messages;
    device.beginFrame();
    std::array<CommandList, 4> lists = {};
    std::array<std::thread, 4> threads;
    for (size_t i = 0; i < threads.size(); ++i)
    {
        threads[i] = std::thread([&device, &lists, i] {
            CommandList list = device.acquireCommandList(QueueType::Graphics, "test.thread");
            for (uint32_t scope = 0; scope < kThreadScopes; ++scope)
            {
                list.beginScope("test.thread.scope");
                list.endScope();
            }
            lists[i] = list;
        });
    }
    for (std::thread& thread : threads)
    {
        thread.join();
    }
    std::set<ID3D12QueryHeap*> heaps;
    for (CommandList list : lists)
    {
        const d3d12::ListState& state = stateOf(list);
        REQUIRE(state.timestamps.heaps.size() == 1u);
        heaps.insert(state.timestamps.heaps[0].queries.get());
        const std::vector<core::ScopeRecord>& log = core::coreOf(list).scopeLog;
        REQUIRE(log.size() == kThreadScopes);
        for (uint32_t scope = 0; scope < kThreadScopes; ++scope)
        {
            CHECK(log[scope].timestamp == stamp(state.ordinal, 2 * scope));
        }
        list.end();
    }
    CHECK(heaps.size() == lists.size());
    device.queue(QueueType::Graphics).submit(lists, {}, {});
    retireOpenFrame(device);
    const std::span<const ScopeTiming> report = device.scopeTimings();
    REQUIRE(report.size() == lists.size() * kThreadScopes);
    for (const ScopeTiming& timing : report)
    {
        checkDuration(timing);
    }
    device.endFrame();
    CHECK(messages.all().empty());
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a list past 2^16 queries records its further scopes untimed and its frame warns once", "[d3d12]")
{
    constexpr uint32_t kCapacity = d3d12::ListTimestamps::kMaxQueries / 2;
    Device device = makeDevice(2);
    const MessageCollection messages;
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    const d3d12::ListState& state = stateOf(list);
    recordScopes(list, kCapacity);
    CHECK(state.timestamps.heaps.size() == d3d12::ListTimestamps::kMaxQueries / d3d12::ListTimestamps::kHeapQueries);
    CHECK(lastScope(list).timestamp == stamp(state.ordinal, d3d12::ListTimestamps::kMaxQueries - 2));
    const uint64_t before = recordCalls(list);
    // The event alone at each end.
    list.beginScope("test.untimed");
    CHECK(recordCalls(list) - before == 1u);
    CHECK(lastScope(list).timestamp == core::kNoTimestamp);
    list.endScope();
    CHECK(recordCalls(list) - before == 2u);
    const uint64_t ended = recordCalls(list);
    list.end();
    CHECK(recordCalls(list) - ended == 1u + state.timestamps.heaps.size());
    device.queue(QueueType::Graphics).submit(std::span(&list, 1), {}, {});
    retireOpenFrame(device);
    const std::span<const ScopeTiming> report = device.scopeTimings();
    REQUIRE(report.size() == kCapacity + 1u);
    CHECK(report.back().label == "test.untimed");
    CHECK(report.back().gpuMilliseconds == 0.0);
    checkDuration(report[kCapacity - 1]);
    std::vector<std::string> collected = messages.all();
    REQUIRE(collected.size() == 1u);
    CHECK(collected[0].find("frame 1") != std::string::npos);
    device.endFrame();
    // The next retired frame recorded no scope, so it warns of nothing.
    device.beginFrame();
    CHECK(messages.all().size() == 1u);
    device.endFrame();
    destroyDevice(device);
}
