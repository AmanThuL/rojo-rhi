//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4CaptureTests.cpp
/// @brief Tests programmatic capture on Metal 4: the capability, the refusal without MTL_CAPTURE_ENABLED and the capture with it.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Metal4Common.h"

#include <catch2/catch_test_macros.hpp>

#include <Metal/Metal.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

using namespace rojoRHI;

namespace {
    //==================================================================================================================
    /// Whether the process was launched with Metal capture enabled.
    bool captureEnabled()
    {
        const char* value = std::getenv("MTL_CAPTURE_ENABLED");
        return value != nullptr && std::string(value) == "1";
    }

    //==================================================================================================================
    /// A `.gputrace` path in the temporary directory that names nothing yet.
    std::filesystem::path freshTracePath(const char* name)
    {
        std::filesystem::path path = std::filesystem::temp_directory_path() / name;
        std::filesystem::remove_all(path);
        return path;
    }

    //==================================================================================================================
    /// Ends the capture `device` began. A `HeldCapture` runs it when it is reset or destroyed, so a failed check cannot leave
    /// Metal's process-global capture running for the cases after it.
    void endHeldCapture(Device* device)
    {
        device->endCapture();
    }

    /// Holds the capture a case began on a device until it is reset or destroyed; empty when none is held.
    using HeldCapture = std::unique_ptr<Device, void (*)(Device*)>;

    //==================================================================================================================
    /// An empty `HeldCapture`. A case declares it after its device and resets it before `destroyDevice`.
    HeldCapture holdCapture()
    {
        return HeldCapture(nullptr, endHeldCapture);
    }

    //==================================================================================================================
    /// Begins a capture of `device` to `path` and, when Metal starts it, makes `held` own its end.
    Result<void> beginHeldCapture(Device& device, HeldCapture& held, const std::filesystem::path& path)
    {
        Result<void> started = device.beginCapture(path.string());
        if (started.has_value())
        {
            held.reset(&device);
        }
        return started;
    }
} // namespace

//======================================================================================================================
TEST_CASE("capture reports whether the process can write a GPU trace document", "[metal4][capture]")
{
    // The manager answers false until the process has a Metal device, so the device comes first.
    Device device = *createDevice();
    bool writable = false;
    {
        const metal4::AutoreleasePool pool;
        writable = MTL::CaptureManager::sharedCaptureManager()->supportsDestination(MTL::CaptureDestinationGPUTraceDocument);
    }
    const bool capture = device.capabilities().capture;
    WARN("capture: " << capture);
    CHECK(capture == writable);
    CHECK(capture == captureEnabled());
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("beginCapture without MTL_CAPTURE_ENABLED is unsupported and leaves no capture in progress", "[metal4][capture]")
{
    if (captureEnabled())
    {
        SKIP("MTL_CAPTURE_ENABLED is set");
    }
    Device device = *createDevice();
    const Result<void> first = device.beginCapture("rojorhi-capture-disabled.gputrace");
    REQUIRE_FALSE(first.has_value());
    CHECK(first.error().category == ErrorCategory::Unsupported);
    CHECK(first.error().message.find("MTL_CAPTURE_ENABLED=1") != std::string::npos);

    // A refused start leaves the device not capturing, so a second call reaches the same refusal and not VU-rojo-diag-0008.
    const Result<void> second = device.beginCapture("rojorhi-capture-disabled.gputrace");
    REQUIRE_FALSE(second.has_value());
    CHECK(second.error().category == ErrorCategory::Unsupported);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a capture writes a gputrace document around a submission and a frame", "[metal4][capture]")
{
    if (!captureEnabled())
    {
        SKIP("MTL_CAPTURE_ENABLED is not set");
    }
    const std::filesystem::path path = freshTracePath("rojorhi-capture-test.gputrace");
    Device device = *createDevice();
    HeldCapture held = holdCapture();
    const Counters before = device.counters();
    const Result<void> started = beginHeldCapture(device, held, path);
    INFO((started.has_value() ? std::string() : started.error().message));
    REQUIRE(started.has_value());
    CHECK(device.counters().nativeHostCalls == before.nativeHostCalls + 1);

    device.queue(QueueType::Graphics).submit({});
    device.beginFrame();
    device.endFrame();
    const Counters during = device.counters();
    held.reset();
    CHECK(device.counters().nativeHostCalls == during.nativeHostCalls + 1);

    REQUIRE(std::filesystem::exists(path));
    CHECK_FALSE(std::filesystem::is_empty(path));
    destroyDevice(device);
    std::filesystem::remove_all(path);
}

//======================================================================================================================
TEST_CASE("a second device cannot begin a capture while one is in progress", "[metal4][capture]")
{
    if (!captureEnabled())
    {
        SKIP("MTL_CAPTURE_ENABLED is not set");
    }
    const std::filesystem::path path = freshTracePath("rojorhi-capture-first.gputrace");
    const std::filesystem::path other = freshTracePath("rojorhi-capture-second.gputrace");
    Device first = *createDevice();
    Device second = *createDevice();
    HeldCapture held = holdCapture();
    REQUIRE(beginHeldCapture(first, held, path).has_value());

    const Result<void> refused = second.beginCapture(other.string());
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error().category == ErrorCategory::Unsupported);

    held.reset();
    destroyDevice(second);
    destroyDevice(first);
    std::filesystem::remove_all(path);
    std::filesystem::remove_all(other);
}

//======================================================================================================================
TEST_CASE("a capture path below a regular file is refused and leaves no capture running", "[metal4][capture]")
{
    if (!captureEnabled())
    {
        SKIP("MTL_CAPTURE_ENABLED is not set");
    }
    // Metal creates missing directories, so the parent that cannot be one is a regular file.
    const std::filesystem::path file = freshTracePath("rojorhi-not-a-directory");
    std::ofstream(file) << "x";
    const std::filesystem::path missing = file / "x.gputrace";
    Device device = *createDevice();
    HeldCapture held = holdCapture();
    const Result<void> refused = beginHeldCapture(device, held, missing);
    REQUIRE_FALSE(refused.has_value());
    WARN("below a regular file: " << refused.error().message);

    // Nothing was left running, so a valid capture starts on the same device afterwards.
    const std::filesystem::path path = freshTracePath("rojorhi-capture-after.gputrace");
    CHECK(beginHeldCapture(device, held, path).has_value());
    held.reset();
    destroyDevice(device);
    std::filesystem::remove_all(path);
    std::filesystem::remove_all(file);
}
