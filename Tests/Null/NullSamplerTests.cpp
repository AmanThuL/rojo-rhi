//----------------------------------------------------------------------------------------------------------------------
/// @file NullSamplerTests.cpp
/// @brief Tests what only the Null backend promises about samplers and slot ranges: heap sizes and slot numbering.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include <catch2/catch_test_macros.hpp>

#include <utility>

using namespace rojoRHI;

//======================================================================================================================
TEST_CASE("the Null sampler heap holds maxSamplers slots, handed out from zero", "[null]")
{
    Device device = *createDevice();
    CHECK(device.capabilities().maxSamplers == 996);
    CHECK(std::to_underlying(*device.createSampler({})) == 0);
    CHECK(std::to_underlying(*device.createSampler({})) == 1);
    const IndexRange range = *device.reserveSamplers(994);
    CHECK(range.base == 2);
    CHECK_FALSE(device.createSampler({}).has_value());
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("the Null view heap reserves ranges after the single slots and fills at maxViews", "[null]")
{
    Device device = *createDevice();
    const Texture texture = *device.createTexture({ .format = Format::R8Unorm, .extent = { 4, 4, 1 }, .usage = TextureUsage::Sampled });
    CHECK(std::to_underlying(*device.createView(texture, {})) == 0);
    const IndexRange range = *device.reserveViews(10);
    CHECK(range.base == 1);
    CHECK(std::to_underlying(*device.createView(texture, {})) == 11);
    CHECK_FALSE(device.reserveViews(device.capabilities().maxViews - 11).has_value());
    CHECK(device.reserveViews(device.capabilities().maxViews - 12).has_value());
    destroyDevice(device);
}
