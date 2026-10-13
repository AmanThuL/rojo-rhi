//----------------------------------------------------------------------------------------------------------------------
/// @file UpscalerTests.cpp
/// @brief Tests the reference behaviour of upscalers and upscales (chapter 12) that every backend shares.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Support/UpscaleKit.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>

using namespace rojoRHI;
using rojoRHI::test::UpscaleKit;

//======================================================================================================================
TEST_CASE("an upscaler is created from the default description with Auto", "[contract]")
{
    Device device = *createDevice();
    const Result<Upscaler> upscaler = device.createUpscaler(UpscalerDesc{ .input = { 64, 64 }, .output = { 128, 128 } });
    REQUIRE(upscaler.has_value());
    device.destroy(*upscaler);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("every kind the device lists can be created", "[contract]")
{
    Device device = *createDevice();
    for (const UpscalerKind kind : device.capabilities().upscalers)
    {
        const Result<Upscaler> upscaler = device.createUpscaler(UpscalerDesc{ .kind = kind, .input = { 64, 64 }, .output = { 64, 64 } });
        CHECK(upscaler.has_value());
    }
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a kind the device does not list is an error, never a substitution", "[contract]")
{
    Device device = *createDevice();
    const std::span<const UpscalerKind> listed = device.capabilities().upscalers;
    for (const UpscalerKind kind : { UpscalerKind::MetalFX, UpscalerKind::DLSS, UpscalerKind::FSR, UpscalerKind::XeSS })
    {
        if (std::find(listed.begin(), listed.end(), kind) == listed.end())
        {
            const Result<Upscaler> upscaler = device.createUpscaler(UpscalerDesc{ .kind = kind, .input = { 64, 64 }, .output = { 64, 64 } });
            REQUIRE_FALSE(upscaler.has_value());
            CHECK(upscaler.error().category == ErrorCategory::Unsupported);
        }
    }
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("an upscaler with every flag and an equal output extent is created", "[contract]")
{
    Device device = *createDevice();
    const UpscalerFlags flags = UpscalerFlags::HDR | UpscalerFlags::ReversedDepth | UpscalerFlags::AutoExposure;
    CHECK(device.createUpscaler(UpscalerDesc{ .input = { 32, 16 }, .output = { 32, 16 }, .flags = flags }).has_value());
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a valid upscale records on a graphics list", "[contract]")
{
    UpscaleKit kit;
    kit.upscale();
    kit.kit.finish();
}

//======================================================================================================================
TEST_CASE("an upscale with the optional inputs, a content rectangle and a label records", "[contract]")
{
    UpscaleKit kit;
    kit.params.exposure = kit.exposureTexture();
    kit.params.reactive = kit.reactiveTexture();
    kit.params.inputContent = { 32, 48 };
    kit.params.jitterX = 0.25f;
    kit.params.jitterY = -0.25f;
    kit.params.preExposure = 2.0f;
    kit.params.sharpness = 1.0f;
    kit.params.reset = true;
    kit.params.label = "test.upscale";
    kit.upscale();
    kit.kit.finish();
}

//======================================================================================================================
TEST_CASE("an upscale takes a content rectangle equal to the whole input", "[contract]")
{
    UpscaleKit kit;
    kit.params.inputContent = kit.desc.input;
    kit.upscale();
    kit.kit.finish();
}

//======================================================================================================================
TEST_CASE("two upscalers each upscale once in the same frame", "[contract]")
{
    UpscaleKit kit;
    const Upscaler other = *kit.kit.device.createUpscaler(kit.desc);
    kit.upscale();
    kit.kit.list.upscale(other, kit.params);
    kit.kit.finish();
}

//======================================================================================================================
TEST_CASE("an upscaler upscales again in a later frame", "[contract]")
{
    UpscaleKit kit;
    kit.upscale();
    kit.nextFrame();
    kit.upscale();
    kit.kit.finish();
}

//======================================================================================================================
TEST_CASE("a destroyed upscaler is gone after its frame slot retires", "[contract]")
{
    UpscaleKit kit;
    kit.kit.device.destroy(kit.upscaler);
    kit.kit.finish();
}
