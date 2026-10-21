//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12UpscalerTests.cpp
/// @brief Tests the D3D12 FSR upscaler: the FidelityFX runtime's loading, the capability, creation, refusals and retirement.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/DeviceCore.h"
#include "Core/HandlePool.h"
#include "D3D12Device.h"
#include "D3D12Upscaler.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <future>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>

using namespace rojoRHI;

namespace {
    /// How long a case that calls into FidelityFX may run before its watchdog ends the process.
    constexpr std::chrono::seconds kFidelityFxLimit{ 300 };

    //==================================================================================================================
    /// Returns the milliseconds since `start` on the steady clock.
    long long millisecondsSince(std::chrono::steady_clock::time_point start)
    {
        return static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
    }

    //==================================================================================================================
    /// Runs `body` on the calling thread while a watchdog thread waits at most `limit` for it, and prints how long it took.
    /// When the limit passes first, the watchdog prints `what` and the time elapsed and ends the process with exit code 3, so
    /// a FidelityFX call that never returns fails the run at once instead of holding the step until its timeout.
    template<typename Body> void within(std::chrono::seconds limit, const char* what, Body&& body)
    {
        const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        std::promise<void> finished;
        std::future<void> done = finished.get_future();
        std::thread watchdog([&done, limit, what, start] {
            if (done.wait_for(limit) != std::future_status::ready)
            {
                std::fprintf(
                    stderr,
                    "%s did not finish within %lld s (%lld ms elapsed); the watchdog ends the process\n",
                    what,
                    static_cast<long long>(limit.count()),
                    millisecondsSince(start)
                );
                std::fflush(stderr);
                TerminateProcess(GetCurrentProcess(), 3);
            }
        });
        try
        {
            body();
        }
        catch (...)
        {
            finished.set_value();
            watchdog.join();
            throw;
        }
        finished.set_value();
        watchdog.join();
        std::printf("%s finished in %lld ms\n", what, millisecondsSince(start));
        std::fflush(stdout);
    }

    //==================================================================================================================
    /// Returns a new empty directory under the temporary directory, named for this process.
    std::filesystem::path makeEmptyDirectory()
    {
        const std::filesystem::path directory = std::filesystem::temp_directory_path() / std::format("rojorhi-fsr-empty-{}", GetCurrentProcessId());
        std::filesystem::remove_all(directory);
        std::filesystem::create_directories(directory);
        return directory;
    }

    //==================================================================================================================
    /// Returns the references `device` holds, read with an `AddRef` and `Release` pair.
    ULONG references(ID3D12Device12* device)
    {
        device->AddRef();
        return device->Release();
    }

    //==================================================================================================================
    /// Returns the description of a 64x64 to 128x128 upscaler of `kind` with the default formats.
    UpscalerDesc upscalerOf(UpscalerKind kind)
    {
        return UpscalerDesc{ .kind = kind, .input = { 64, 64 }, .output = { 128, 128 }, .label = "test.upscaler" };
    }
} // namespace

//======================================================================================================================
TEST_CASE("loadFidelityFx finds nothing in an empty directory", "[d3d12]")
{
    const std::filesystem::path empty = makeEmptyDirectory();
    CHECK_FALSE(d3d12::loadFidelityFx(empty.wstring()).has_value());
    CHECK_FALSE(d3d12::loadFidelityFx(std::wstring_view{}).has_value());
    std::filesystem::remove_all(empty);
}

//======================================================================================================================
TEST_CASE("upscalers lists FSR when the runtime sits beside the executable", "[d3d12]")
{
    const std::filesystem::path beside = std::filesystem::path(d3d12::executableDirectory()) / "amd_fidelityfx_loader_dx12.dll";
    REQUIRE(std::filesystem::exists(beside));
    Result<Device> created = createDevice();
    REQUIRE(created.has_value());
    DeviceImpl& impl = DeviceImpl::of(*created);
    REQUIRE(impl.fsr.fidelityFx.has_value());
    // The version query keeps no reference to the device it is given.
    const ULONG before = references(impl.device());
    CHECK(d3d12::fsrAvailable(*impl.fsr.fidelityFx, *impl.device()));
    CHECK(references(impl.device()) == before);
    const std::span<const UpscalerKind> upscalers = created->capabilities().upscalers;
    REQUIRE(upscalers.size() == 1u);
    CHECK(upscalers.front() == UpscalerKind::FSR);
    destroyDevice(*created);
}

//======================================================================================================================
TEST_CASE("createUpscaler with Auto creates an FSR context and destroy retires it", "[d3d12]")
{
    // The device's creation queries the runtime and its destruction frees it, so both run under the watchdog too.
    within(kFidelityFxLimit, "createUpscaler with Auto creates an FSR context and destroy retires it", [] {
        Result<Device> created = createDevice(DeviceDesc{ .framesInFlight = 2 });
        REQUIRE(created.has_value());
        Device device = *created;
        DeviceImpl& impl = DeviceImpl::of(device);
        REQUIRE(device.capabilities().upscalers.size() == 1u);
        ID3D12Device12* native = impl.device();
        const ULONG baseline = references(native);
        device.beginFrame();
        const Counters before = device.counters();
        const Result<Upscaler> upscaler = device.createUpscaler(upscalerOf(UpscalerKind::Auto));
        REQUIRE(upscaler.has_value());
        // The pool counts the record, which stands for the context.
        CHECK(device.counters().nativeCreateCalls == before.nativeCreateCalls + 1);
        const core::LookupResult<core::UpscalerRecord> found = impl.core.pool<Upscaler>().lookup(*upscaler);
        REQUIRE(found.record != nullptr);
        CHECK(found.record->kind == UpscalerKind::FSR);
        CHECK(found.record->native != nullptr);
        // The context's objects are children of the device, each holding a reference to it.
        CHECK(references(native) > baseline);
        device.destroy(*upscaler);
        const uint64_t retired = device.counters().deferredDestructionsRetired;
        for (uint32_t frame = 1; frame < impl.core.framesInFlight(); ++frame)
        {
            device.endFrame();
            device.beginFrame();
            CHECK(device.counters().deferredDestructionsRetired == retired);
        }
        device.endFrame();
        const Counters held = device.counters();
        // This frame reuses the slot of the frame the upscaler was destroyed in, so its context is destroyed.
        device.beginFrame();
        const Counters after = device.counters();
        CHECK(after.deferredDestructionsRetired == held.deferredDestructionsRetired + 1);
        CHECK(after.nativeCreateCalls == held.nativeCreateCalls + 1);
        CHECK(impl.core.pool<Upscaler>().liveCount() == 0u);
        CHECK(references(native) == baseline);
        device.endFrame();
        destroyDevice(device);
    });
}

//======================================================================================================================
TEST_CASE("createUpscaler refuses DLSS, XeSS and MetalFX with Unsupported", "[d3d12]")
{
    Result<Device> created = createDevice();
    REQUIRE(created.has_value());
    for (const auto& [kind, name] :
         { std::pair{ UpscalerKind::DLSS, "DLSS" }, std::pair{ UpscalerKind::XeSS, "XeSS" }, std::pair{ UpscalerKind::MetalFX, "MetalFX" } })
    {
        const Result<Upscaler> upscaler = created->createUpscaler(upscalerOf(kind));
        REQUIRE_FALSE(upscaler.has_value());
        CHECK(upscaler.error().category == ErrorCategory::Unsupported);
        CHECK(upscaler.error().message.find(name) != std::string::npos);
    }
    CHECK(DeviceImpl::of(*created).core.pool<Upscaler>().liveCount() == 0u);
    destroyDevice(*created);
}

//======================================================================================================================
TEST_CASE("a device whose runtime is absent refuses Auto with Unsupported", "[d3d12]")
{
    Result<Device> created = createDevice();
    REQUIRE(created.has_value());
    DeviceImpl& impl = DeviceImpl::of(*created);
    // The device is made into one built where no runtime sits beside the executable: the seam loads nothing from an empty
    // directory, and the capability report lists what that leaves.
    const std::filesystem::path empty = makeEmptyDirectory();
    std::optional<d3d12::FidelityFx> loaded = std::exchange(impl.fsr.fidelityFx, d3d12::loadFidelityFx(empty.wstring()));
    std::filesystem::remove_all(empty);
    REQUIRE_FALSE(impl.fsr.fidelityFx.has_value());
    Capabilities& capabilities = impl.core.capabilities();
    const std::span<const UpscalerKind> listed = std::exchange(capabilities.upscalers, d3d12::listedUpscalers(impl.fsr.fidelityFx, *impl.device()));
    CHECK(created->capabilities().upscalers.empty());
    for (const UpscalerKind kind : { UpscalerKind::Auto, UpscalerKind::FSR })
    {
        const Result<Upscaler> upscaler = created->createUpscaler(upscalerOf(kind));
        REQUIRE_FALSE(upscaler.has_value());
        CHECK(upscaler.error().category == ErrorCategory::Unsupported);
        CHECK(upscaler.error().message.find("amd_fidelityfx_loader_dx12.dll") != std::string::npos);
    }
    CHECK(impl.core.pool<Upscaler>().liveCount() == 0u);
    impl.fsr.fidelityFx = std::move(loaded);
    capabilities.upscalers = listed;
    destroyDevice(*created);
}
