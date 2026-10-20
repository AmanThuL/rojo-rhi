//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12DebugTests.cpp
/// @brief Tests that debug-layer messages reach the message callback with their severity, and that a removed device is reported.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "D3D12Debug.h"
#include "D3D12Device.h"
#include "Support/Death.h"

#include <catch2/catch_test_macros.hpp>

#include <d3d12sdklayers.h>
#include <dxgi1_6.h>

#include <iterator>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

using namespace rojoRHI;

namespace {
    /// True when the build selects the debug layer: `layer` and `gbv` always, `mode` in Debug builds only.
#if defined(ROJORHI_D3D12_DEBUG_LAYER) || defined(ROJORHI_D3D12_DEBUG_GBV) || (defined(ROJORHI_D3D12_DEBUG_MODE) && !defined(NDEBUG))
    constexpr bool kDebugLayer = true;
#else
    constexpr bool kDebugLayer = false;
#endif

    /// One message the callback saw.
    struct Seen
    {
        MessageSeverity severity;
        std::string text;
    };

    /// Records every message while it is installed as the message callback, and restores the default sink when it goes out of
    /// scope.
    class Recorder
    {
      public:
        Recorder()
        {
            setMessageCallback(&Recorder::record, this);
        }

        Recorder(const Recorder&) = delete;
        Recorder& operator=(const Recorder&) = delete;

        ~Recorder()
        {
            setMessageCallback(nullptr, nullptr);
        }

        /// Returns how many messages of `severity` contain `needle`.
        size_t count(MessageSeverity severity, std::string_view needle)
        {
            const std::lock_guard lock(m_mutex);
            size_t found = 0;
            for (const Seen& message : m_seen)
            {
                if (message.severity == severity && message.text.find(needle) != std::string::npos)
                {
                    ++found;
                }
            }
            return found;
        }

        /// Returns every message seen, one per line, for a failure message.
        std::string dump()
        {
            const std::lock_guard lock(m_mutex);
            std::string out;
            for (const Seen& message : m_seen)
            {
                out += message.text + "\n";
            }
            return out;
        }

      private:
        static void record(MessageSeverity severity, std::string_view message, void* user)
        {
            auto& self = *static_cast<Recorder*>(user);
            const std::lock_guard lock(self.m_mutex);
            self.m_seen.push_back({ severity, std::string(message) });
        }

        std::mutex m_mutex;
        std::vector<Seen> m_seen;
    };

    //==================================================================================================================
    /// Returns the message queue of `impl`'s device, which only a device with the debug layer on has.
    d3d12::Com<ID3D12InfoQueue> infoQueue(DeviceImpl& impl)
    {
        d3d12::Com<ID3D12InfoQueue> queue;
        REQUIRE(SUCCEEDED(impl.device()->QueryInterface(IID_PPV_ARGS(queue.put()))));
        return queue;
    }
} // namespace

//======================================================================================================================
TEST_CASE("a debug-layer warning reaches the message callback as Warning", "[d3d12]")
{
    if (!kDebugLayer)
    {
        SKIP("the build selects no debug layer");
    }
    Result<Device> created = createDevice();
    REQUIRE(created.has_value());
    {
        Recorder recorder;
        const d3d12::Com<ID3D12InfoQueue> queue = infoQueue(DeviceImpl::of(*created));
        REQUIRE(SUCCEEDED(queue->AddApplicationMessage(D3D12_MESSAGE_SEVERITY_WARNING, "test.d3d12.warning")));
        INFO(recorder.dump());
        CHECK(recorder.count(MessageSeverity::Warning, "test.d3d12.warning") == 1);
    }
    destroyDevice(*created);
}

//======================================================================================================================
TEST_CASE("a debug-layer info message reaches the message callback as Info", "[d3d12]")
{
    if (!kDebugLayer)
    {
        SKIP("the build selects no debug layer");
    }
    Result<Device> created = createDevice();
    REQUIRE(created.has_value());
    {
        Recorder recorder;
        const d3d12::Com<ID3D12InfoQueue> queue = infoQueue(DeviceImpl::of(*created));
        REQUIRE(SUCCEEDED(queue->AddApplicationMessage(D3D12_MESSAGE_SEVERITY_INFO, "test.d3d12.info")));
        INFO(recorder.dump());
        CHECK(recorder.count(MessageSeverity::Info, "test.d3d12.info") == 1);
    }
    destroyDevice(*created);
}

//======================================================================================================================
TEST_CASE("a debug-layer error aborts after reaching the callback", "[d3d12][death]")
{
    if (!kDebugLayer)
    {
        SKIP("the build selects no debug layer");
    }
    ROJORHI_EXPECT_ABORT("test.d3d12.error", [] {
        Result<Device> created = createDevice();
        REQUIRE(created.has_value());
        const d3d12::Com<ID3D12InfoQueue> queue = infoQueue(DeviceImpl::of(*created));
        queue->AddApplicationMessage(D3D12_MESSAGE_SEVERITY_ERROR, "test.d3d12.error");
    });
}

//======================================================================================================================
TEST_CASE("a removed device is reported with its reason before the entry fails", "[d3d12]")
{
    if (!kDebugLayer)
    {
        SKIP("the build selects no debug layer");
    }
    Result<Device> created = createDevice();
    REQUIRE(created.has_value());
    DeviceImpl& impl = DeviceImpl::of(*created);
    {
        Recorder recorder;
        // The runtime reports a removal it was asked for as an error of its own; the case causes it on purpose, so the
        // messages of the three removal identifiers are dropped for the length of the call.
        d3d12::Com<ID3D12InfoQueue> queue = infoQueue(impl);
        D3D12_MESSAGE_ID dropped[] = { D3D12_MESSAGE_ID_DEVICE_REMOVAL_PROCESS_AT_FAULT,
                                       D3D12_MESSAGE_ID_DEVICE_REMOVAL_PROCESS_POSSIBLY_AT_FAULT,
                                       D3D12_MESSAGE_ID_DEVICE_REMOVAL_PROCESS_NOT_AT_FAULT };
        D3D12_INFO_QUEUE_FILTER filter = {};
        filter.DenyList.NumIDs = static_cast<UINT>(std::size(dropped));
        filter.DenyList.pIDList = dropped;
        REQUIRE(SUCCEEDED(queue->PushCopyOfStorageFilter()));
        REQUIRE(SUCCEEDED(queue->AddStorageFilterEntries(&filter)));

        impl.device()->RemoveDevice();
        const Error error = d3d12::failure(impl, DXGI_ERROR_DEVICE_REMOVED, "test");

        queue->PopStorageFilter();
        INFO(recorder.dump());
        CHECK(error.category == ErrorCategory::DriverFailure);
        CHECK(recorder.count(MessageSeverity::Error, "test") >= 1);
        CHECK(recorder.count(MessageSeverity::Error, "removed") >= 1);
    }
    destroyDevice(*created);
}

//======================================================================================================================
TEST_CASE("detaching the router twice is harmless", "[d3d12]")
{
    Result<Device> created = createDevice();
    REQUIRE(created.has_value());
    DeviceImpl& impl = DeviceImpl::of(*created);
    impl.messages.detach();
    impl.messages.detach();
    destroyDevice(*created);
}
