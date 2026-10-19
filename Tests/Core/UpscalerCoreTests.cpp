//----------------------------------------------------------------------------------------------------------------------
/// @file UpscalerCoreTests.cpp
/// @brief Tests the record the core keeps for an upscaler and the kind resolution of its creation.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/DeviceCore.h"
#include "Core/Upscalers.h"
#include "Support/UpscaleKit.h"

#include <catch2/catch_test_macros.hpp>

#include <array>

using namespace rojoRHI;
using rojoRHI::test::UpscaleKit;

namespace {
    /// The kinds a device with a short list offers.
    constexpr std::array<UpscalerKind, 2> kShortList = { UpscalerKind::FSR, UpscalerKind::XeSS };

    //==================================================================================================================
    const core::UpscalerRecord& recordOf(Device device, Upscaler upscaler)
    {
        return *core::coreOf(device).pool<Upscaler>().lookup(upscaler).record;
    }
} // namespace

//======================================================================================================================
TEST_CASE("an upscaler's record keeps its description", "[core]")
{
    Device device = *createDevice();
    const Upscaler upscaler = *device.createUpscaler(
        UpscalerDesc{ .kind = UpscalerKind::FSR,
                      .input = { 40, 30 },
                      .output = { 80, 60 },
                      .color = Format::RGBA8Unorm,
                      .depth = Format::D32FloatS8,
                      .motion = Format::RG16Float,
                      .outputFormat = Format::BGRA8Unorm,
                      .flags = UpscalerFlags::HDR | UpscalerFlags::AutoExposure,
                      .label = "core.upscaler" }
    );
    const core::UpscalerRecord& record = recordOf(device, upscaler);
    CHECK(record.label == "core.upscaler");
    CHECK(record.kind == UpscalerKind::FSR);
    CHECK(record.input.width == 40);
    CHECK(record.input.height == 30);
    CHECK(record.output.width == 80);
    CHECK(record.output.height == 60);
    CHECK(record.color == Format::RGBA8Unorm);
    CHECK(record.depth == Format::D32FloatS8);
    CHECK(record.motion == Format::RG16Float);
    CHECK(record.outputFormat == Format::BGRA8Unorm);
    CHECK(record.flags == (UpscalerFlags::HDR | UpscalerFlags::AutoExposure));
    CHECK(record.lastFrame.frame == 0);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("an unlabelled upscaler takes the default label", "[core]")
{
    Device device = *createDevice();
    const Upscaler upscaler = *device.createUpscaler(UpscalerDesc{ .input = { 8, 8 }, .output = { 8, 8 } });
    CHECK(recordOf(device, upscaler).label == "rojorhi.upscaler.unnamed");
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("Auto resolves to the first kind the device lists", "[core]")
{
    Device device = *createDevice();
    core::coreOf(device).capabilities().upscalers = kShortList;
    const Upscaler upscaler = *device.createUpscaler(UpscalerDesc{ .kind = UpscalerKind::Auto, .input = { 8, 8 }, .output = { 8, 8 } });
    CHECK(recordOf(device, upscaler).kind == UpscalerKind::FSR);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a kind missing from a shorter list is an error and a listed one still works", "[core]")
{
    Device device = *createDevice();
    core::coreOf(device).capabilities().upscalers = kShortList;
    const Result<Upscaler> missing = device.createUpscaler(UpscalerDesc{ .kind = UpscalerKind::MetalFX, .input = { 8, 8 }, .output = { 8, 8 } });
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().category == ErrorCategory::Unsupported);
    CHECK(core::coreOf(device).pool<Upscaler>().liveCount() == 0);
    CHECK(device.createUpscaler(UpscalerDesc{ .kind = UpscalerKind::XeSS, .input = { 8, 8 }, .output = { 8, 8 } }).has_value());
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("Auto on a device that lists no kind is an error", "[core]")
{
    Device device = *createDevice();
    core::coreOf(device).capabilities().upscalers = {};
    const Result<Upscaler> upscaler = device.createUpscaler(UpscalerDesc{ .input = { 8, 8 }, .output = { 8, 8 } });
    REQUIRE_FALSE(upscaler.has_value());
    CHECK(upscaler.error().category == ErrorCategory::Unsupported);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("prepareUpscaler resolves Auto and rejects an unlisted kind without allocating", "[core]")
{
    Device device = *createDevice();
    core::DeviceCore& core = core::coreOf(device);
    core.capabilities().upscalers = kShortList;
    const Result<core::UpscalerRecord> resolved =
        core::prepareUpscaler(core, UpscalerDesc{ .kind = UpscalerKind::Auto, .input = { 8, 8 }, .output = { 16, 16 }, .label = "core.prepared" });
    REQUIRE(resolved.has_value());
    CHECK(resolved->kind == UpscalerKind::FSR);
    CHECK(resolved->label == "core.prepared");
    CHECK(resolved->output.width == 16);
    CHECK(resolved->native == nullptr);
    CHECK(core.pool<Upscaler>().liveCount() == 0);
    const Result<core::UpscalerRecord> missing =
        core::prepareUpscaler(core, UpscalerDesc{ .kind = UpscalerKind::MetalFX, .input = { 8, 8 }, .output = { 8, 8 } });
    REQUIRE_FALSE(missing.has_value());
    CHECK(missing.error().category == ErrorCategory::Unsupported);
    CHECK(core.pool<Upscaler>().liveCount() == 0);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("an upscale stores the frame it was recorded in", "[core]")
{
#ifdef NDEBUG
    SKIP("the frame stamp is a Debug-tier check (ADR 0020)");
#endif
    UpscaleKit kit;
    kit.upscale();
    CHECK(core::coreOf(kit.kit.device).pool<Upscaler>().lookup(kit.upscaler).record->lastFrame.frame != 0);
    kit.kit.finish();
}

//======================================================================================================================
TEST_CASE("an upscaler created in a reused slot starts with no frame stamp", "[core]")
{
    UpscaleKit kit;
    kit.upscale();
    kit.kit.device.destroy(kit.upscaler);
    // Retire the frame slots until the destroyed upscaler's slot is free to reuse.
    for (int frame = 0; frame < 3; ++frame)
    {
        kit.nextFrame();
    }
    const Upscaler reused = *kit.kit.device.createUpscaler(kit.desc);
    CHECK(recordOf(kit.kit.device, reused).lastFrame.frame == 0);
    kit.kit.list.upscale(reused, kit.params);
    kit.kit.finish();
}
