//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12QueueTests.cpp
/// @brief Tests D3D12 queues, fences and frame pacing against the native queues and fences they create and signal.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/DeviceCore.h"
#include "D3D12Common.h"
#include "D3D12Device.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <thread>

using namespace rojoRHI;

namespace {
    /// The longest a case waits on a native fence before it fails: thirty seconds, in nanoseconds.
    constexpr uint64_t kNativeWaitNs = 30'000'000'000;

    //==================================================================================================================
    /// Returns the name `object` carries, without a terminating null.
    std::wstring objectName(ID3D12Object* object)
    {
        wchar_t name[128] = {};
        UINT size = static_cast<UINT>(sizeof(name));
        if (FAILED(object->GetPrivateData(WKPDID_D3DDebugObjectNameW, &size, name)))
        {
            return {};
        }
        // The size counts bytes; a terminating null, when stored, is dropped.
        std::wstring read(name, size / sizeof(wchar_t));
        while (!read.empty() && read.back() == L'\0')
        {
            read.pop_back();
        }
        return read;
    }

    /// The warnings and errors `countWarning` has seen.
    std::atomic<uint64_t> g_warnings = 0;

    //==================================================================================================================
    /// Counts `severity` when it is a warning or an error, the debug layer's among them. It may run on any thread.
    void countWarning(MessageSeverity severity, std::string_view, void*)
    {
        if (severity != MessageSeverity::Info)
        {
            ++g_warnings;
        }
    }

    //==================================================================================================================
    /// Returns a native fence at zero named `test.gate`, which only the CPU signals, made on `device`'s native device.
    d3d12::Com<ID3D12Fence> makeGate(Device device)
    {
        d3d12::Com<ID3D12Fence> gate;
        REQUIRE(SUCCEEDED(DeviceImpl::of(device).device()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(gate.put()))));
        d3d12::setName(gate.get(), "test.gate");
        return gate;
    }

    //==================================================================================================================
    /// Queues a native wait for `gate` to reach 1 on `device`'s queue of `type`, which holds that queue's later work until the
    /// CPU opens the gate.
    void holdQueue(Device device, QueueType type, ID3D12Fence* gate)
    {
        REQUIRE(SUCCEEDED(DeviceImpl::of(device).queues[static_cast<size_t>(type)].native->Wait(gate, 1)));
    }

    //==================================================================================================================
    /// Returns the native fence of `fence`, created on `device`.
    ID3D12Fence* nativeFence(Device device, Fence fence)
    {
        return static_cast<ID3D12Fence*>(DeviceImpl::of(device).core.pool<Fence>().native(fence));
    }
} // namespace

//======================================================================================================================
TEST_CASE("each queue type has its own native queue of the matching list type", "[d3d12]")
{
    Result<Device> created = createDevice();
    REQUIRE(created.has_value());
    constexpr std::array<D3D12_COMMAND_LIST_TYPE, 3> kTypes = { D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_TYPE_COMPUTE, D3D12_COMMAND_LIST_TYPE_COPY };
    constexpr std::array<std::wstring_view, 3> kNames = { L"graphics", L"compute", L"copy" };
    for (size_t i = 0; i < kTypes.size(); ++i)
    {
        QueueImpl& queue = QueueImpl::of(created->queue(static_cast<QueueType>(i)));
        REQUIRE(queue.native);
        REQUIRE(queue.progress.fence);
        CHECK(queue.native->GetDesc().Type == kTypes[i]);
        const std::wstring label = std::wstring(L"rojorhi.queue.") + std::wstring(kNames[i]);
        CHECK(objectName(queue.native.get()) == label);
        CHECK(objectName(queue.progress.fence.get()) == label + L".progress");
    }
    destroyDevice(*created);
}

//======================================================================================================================
TEST_CASE("a fence is an ID3D12Fence at its initial value", "[d3d12]")
{
    Result<Device> created = createDevice();
    REQUIRE(created.has_value());
    Result<Fence> fence = created->createFence(5, "test.fence");
    REQUIRE(fence.has_value());
    ID3D12Fence* native = nativeFence(*created, *fence);
    REQUIRE(native != nullptr);
    CHECK(native->GetCompletedValue() == 5);
    CHECK(objectName(native) == L"test.fence");
    destroyDevice(*created);
}

//======================================================================================================================
TEST_CASE("a signal submitted with no lists completes on the GPU", "[d3d12]")
{
    Result<Device> created = createDevice();
    REQUIRE(created.has_value());
    Result<Fence> fence = created->createFence(0, "test.fence");
    REQUIRE(fence.has_value());
    for (uint64_t n = 1; n <= 3; ++n)
    {
        const std::array<FenceValue, 1> signal{ FenceValue{ *fence, n } };
        created->queue(static_cast<QueueType>(n - 1)).submit({}, {}, signal);
        CHECK(created->wait({ *fence, n }, UINT64_MAX));
        CHECK(created->completedValue(*fence) >= n);
    }
    destroyDevice(*created);
}

//======================================================================================================================
TEST_CASE("a wait on another queue's fence holds the waiting queue's signal", "[d3d12]")
{
    Result<Device> created = createDevice();
    REQUIRE(created.has_value());
    Result<Fence> a = created->createFence(0, "test.a");
    REQUIRE(a.has_value());
    Result<Fence> b = created->createFence(0, "test.b");
    REQUIRE(b.has_value());
    // The graphics queue's signal of a waits on a gate only the CPU opens, so the compute queue's wait on a holds until then.
    d3d12::Com<ID3D12Fence> gate = makeGate(*created);
    holdQueue(*created, QueueType::Graphics, gate.get());
    const std::array<FenceValue, 1> signalA{ FenceValue{ *a, 1 } };
    created->queue(QueueType::Graphics).submit({}, {}, signalA);
    const std::array<FenceValue, 1> waitA{ FenceValue{ *a, 1 } };
    const std::array<FenceValue, 1> signalB{ FenceValue{ *b, 1 } };
    created->queue(QueueType::Compute).submit({}, waitA, signalB);
    CHECK(created->completedValue(*a) == 0);
    CHECK(created->completedValue(*b) == 0);
    CHECK_FALSE(created->wait({ *b, 1 }, 1'000'000));
    REQUIRE(SUCCEEDED(gate->Signal(1)));
    CHECK(created->wait({ *b, 1 }, UINT64_MAX));
    CHECK(created->completedValue(*a) == 1);
    // The gate holds a reference to the device, which must be the device's last one when it is destroyed.
    gate.reset();
    destroyDevice(*created);
}

//======================================================================================================================
TEST_CASE("endFrame signals the frame fence with the frame number", "[d3d12]")
{
    Result<Device> created = createDevice();
    REQUIRE(created.has_value());
    for (int frame = 0; frame < 3; ++frame)
    {
        created->beginFrame();
        created->endFrame();
    }
    ID3D12Fence* frames = DeviceImpl::of(*created).frames.fence();
    REQUIRE(frames != nullptr);
    CHECK(objectName(frames) == L"rojorhi.frame");
    CHECK(d3d12::waitForFence(frames, 3, kNativeWaitNs, "the frame fence").reached);
    CHECK(frames->GetCompletedValue() >= 3);
    destroyDevice(*created);
}

//======================================================================================================================
TEST_CASE("beginFrame waits for the frame framesInFlight earlier", "[d3d12]")
{
    DeviceDesc desc;
    desc.framesInFlight = 2;
    Result<Device> created = createDevice(desc);
    REQUIRE(created.has_value());
    ID3D12Fence* frames = DeviceImpl::of(*created).frames.fence();
    REQUIRE(frames != nullptr);
    // The graphics queue holds frame 1's end behind a gate, so frame 3 begins only once a wait has seen the gate open.
    d3d12::Com<ID3D12Fence> gate = makeGate(*created);
    created->beginFrame();
    holdQueue(*created, QueueType::Graphics, gate.get());
    created->endFrame();
    created->beginFrame();
    created->endFrame();
    CHECK(frames->GetCompletedValue() == 0);
    std::atomic<HRESULT> opened = E_FAIL;
    std::thread opener([&gate, &opened] {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        opened = gate->Signal(1);
    });
    created->beginFrame();
    CHECK(frames->GetCompletedValue() >= 1);
    created->endFrame();
    created->beginFrame();
    CHECK(frames->GetCompletedValue() >= 2);
    created->endFrame();
    opener.join();
    CHECK(SUCCEEDED(opened.load()));
    // The gate holds a reference to the device, which must be the device's last one when it is destroyed.
    gate.reset();
    destroyDevice(*created);
}

//======================================================================================================================
TEST_CASE("a destroyed fence is released once every queue has passed it", "[d3d12]")
{
    Result<Device> created = createDevice();
    REQUIRE(created.has_value());
    DeviceImpl& impl = DeviceImpl::of(*created);
    Result<Fence> fence = created->createFence(0, "test.fence");
    REQUIRE(fence.has_value());
    // A fence-only submission on the copy queue joins no frame, so the copy queue signals its progress after it; a gate
    // holds both signals until the CPU opens it.
    d3d12::Com<ID3D12Fence> gate = makeGate(*created);
    holdQueue(*created, QueueType::Copy, gate.get());
    const std::array<FenceValue, 1> signal{ FenceValue{ *fence, 1 } };
    created->queue(QueueType::Copy).submit({}, {}, signal);
    created->beginFrame();
    created->destroy(*fence);
    created->endFrame();
    // The last of these frames retires the fence; the copy queue has not passed its progress, so the fence is kept.
    for (uint32_t frame = 0; frame < impl.core.framesInFlight(); ++frame)
    {
        created->beginFrame();
        created->endFrame();
    }
    CHECK(impl.frames.keptCount() == 1);
    REQUIRE(SUCCEEDED(gate->Signal(1)));
    QueueImpl& copy = impl.queues[static_cast<size_t>(QueueType::Copy)];
    const uint64_t progress = copy.progress.value.load();
    CHECK(progress >= 1);
    REQUIRE(d3d12::waitForFence(copy.progress.fence.get(), progress, kNativeWaitNs, "the copy queue's progress fence").reached);
    created->beginFrame();
    CHECK(impl.frames.keptCount() == 0);
    created->endFrame();
    gate.reset();
    destroyDevice(*created);
}

//======================================================================================================================
TEST_CASE("a submission counts one native queue call per wait and signal", "[d3d12]")
{
    Result<Device> created = createDevice();
    REQUIRE(created.has_value());
    Result<Fence> a = created->createFence(0, "test.a");
    Result<Fence> b = created->createFence(0, "test.b");
    Result<Fence> c = created->createFence(0, "test.c");
    REQUIRE((a.has_value() && b.has_value() && c.has_value()));
    Queue graphics = created->queue(QueueType::Graphics);
    const std::array<FenceValue, 1> first{ FenceValue{ *a, 1 } };
    graphics.submit({}, {}, first);
    // The graphics queue never signals a progress fence, so the submission's native calls are its wait and two signals.
    const uint64_t before = created->counters().nativeSubmitCalls;
    const std::array<FenceValue, 1> wait{ FenceValue{ *a, 1 } };
    const std::array<FenceValue, 2> signal{ FenceValue{ *b, 1 }, FenceValue{ *c, 1 } };
    graphics.submit({}, wait, signal);
    CHECK(created->counters().nativeSubmitCalls - before == 3);
    CHECK(created->wait({ *c, 1 }, UINT64_MAX));
    destroyDevice(*created);
}

//======================================================================================================================
TEST_CASE("Device::wait with a zero timeout polls", "[d3d12]")
{
    Result<Device> created = createDevice();
    REQUIRE(created.has_value());
    Result<Fence> fence = created->createFence(0, "test.fence");
    REQUIRE(fence.has_value());
    // A poll reads the fence once and arms no event.
    const uint64_t before = created->counters().nativeHostCalls;
    CHECK_FALSE(created->wait({ *fence, 1 }, 0));
    CHECK(created->counters().nativeHostCalls - before == 1);
    destroyDevice(*created);
}

//======================================================================================================================
TEST_CASE("a submission that waits for value zero raises no debug-layer message", "[d3d12]")
{
    Result<Device> created = createDevice();
    REQUIRE(created.has_value());
    Result<Fence> zero = created->createFence(0, "test.zero");
    Result<Fence> done = created->createFence(0, "test.done");
    REQUIRE((zero.has_value() && done.has_value()));
    g_warnings = 0;
    setMessageCallback(countWarning, nullptr);
    const std::array<FenceValue, 1> wait{ FenceValue{ *zero, 0 } };
    uint64_t value = 0;
    for (const QueueType type : { QueueType::Graphics, QueueType::Compute, QueueType::Copy })
    {
        const std::array<FenceValue, 1> signal{ FenceValue{ *done, ++value } };
        created->queue(type).submit({}, wait, signal);
        CHECK(created->wait({ *done, value }, UINT64_MAX));
    }
    setMessageCallback(nullptr, nullptr);
    CHECK(g_warnings.load() == 0);
    destroyDevice(*created);
}
