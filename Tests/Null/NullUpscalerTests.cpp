//----------------------------------------------------------------------------------------------------------------------
/// @file NullUpscalerTests.cpp
/// @brief Tests what only the Null backend promises about upscalers: which kinds exist and what Auto selects.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/DeviceCore.h"
#include "Core/Upscalers.h"

#include <catch2/catch_test_macros.hpp>

using namespace rojoRHI;

//======================================================================================================================
TEST_CASE("Auto on Null resolves to MetalFX and the record stores the resolved kind", "[null]")
{
    Device device = *createDevice();
    const Upscaler upscaler = *device.createUpscaler(UpscalerDesc{ .kind = UpscalerKind::Auto, .input = { 8, 8 }, .output = { 16, 16 } });
    CHECK(core::coreOf(device).pool<Upscaler>().lookup(upscaler).record->kind == UpscalerKind::MetalFX);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("Null creates every concrete upscaler kind", "[null]")
{
    Device device = *createDevice();
    for (const UpscalerKind kind : { UpscalerKind::MetalFX, UpscalerKind::DLSS, UpscalerKind::FSR, UpscalerKind::XeSS })
    {
        const Result<Upscaler> upscaler = device.createUpscaler(UpscalerDesc{ .kind = kind, .input = { 8, 8 }, .output = { 16, 16 } });
        REQUIRE(upscaler.has_value());
        CHECK(core::coreOf(device).pool<Upscaler>().lookup(*upscaler).record->kind == kind);
    }
    destroyDevice(device);
}
