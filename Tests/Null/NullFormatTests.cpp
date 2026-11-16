//----------------------------------------------------------------------------------------------------------------------
/// @file NullFormatTests.cpp
/// @brief Tests what the Null backend reports for formats, which no other backend need match.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include <catch2/catch_test_macros.hpp>

using namespace rojoRHI;

//======================================================================================================================
TEST_CASE("the Null device reports every capability the reference allows a color format", "[null]")
{
    Device device = *createDevice();
    for (const Format format : { Format::BGRA8Unorm,
                                 Format::RGBA8Unorm,
                                 Format::RGBA16Float,
                                 Format::RG16Float,
                                 Format::R8Unorm,
                                 Format::R16Float,
                                 Format::R32Float,
                                 Format::RGB10A2Unorm })
    {
        CAPTURE(static_cast<uint32_t>(format));
        const FormatCapabilities caps = device.formatCapabilities(format);
        CHECK(caps.sampled);
        CHECK(caps.filterLinear);
        CHECK(caps.storageRead);
        CHECK(caps.storageWrite);
        CHECK(caps.colorAttachment);
        CHECK(caps.blend);
        CHECK(caps.resolve);
        CHECK(caps.sparse);
        CHECK_FALSE(caps.depthAttachment);
        CHECK(caps.sampleCounts == 0b1111);
    }
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("the Null device reports no storage for an sRGB format", "[null]")
{
    Device device = *createDevice();
    const FormatCapabilities caps = device.formatCapabilities(Format::RGBA8Unorm_sRGB);
    CHECK(caps.sampled);
    CHECK(caps.colorAttachment);
    CHECK(caps.blend);
    CHECK_FALSE(caps.storageRead);
    CHECK_FALSE(caps.storageWrite);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("the Null device reports a block-compressed format as sampled only", "[null]")
{
    Device device = *createDevice();
    for (const Format format : { Format::BC1Unorm, Format::BC1Unorm_sRGB })
    {
        const FormatCapabilities caps = device.formatCapabilities(format);
        CHECK(caps.sampled);
        CHECK(caps.filterLinear);
        CHECK(caps.sparse);
        CHECK_FALSE(caps.storageRead);
        CHECK_FALSE(caps.storageWrite);
        CHECK_FALSE(caps.colorAttachment);
        CHECK_FALSE(caps.blend);
        CHECK_FALSE(caps.depthAttachment);
        CHECK_FALSE(caps.resolve);
        CHECK(caps.sampleCounts == 1);
    }
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("the Null device reports a depth format as a depth attachment without storage or color use", "[null]")
{
    Device device = *createDevice();
    for (const Format format : { Format::D32Float, Format::D32FloatS8 })
    {
        const FormatCapabilities caps = device.formatCapabilities(format);
        CHECK(caps.sampled);
        CHECK(caps.filterLinear);
        CHECK(caps.depthAttachment);
        CHECK(caps.sparse);
        CHECK(caps.sampleCounts == 0b1111);
        CHECK_FALSE(caps.storageRead);
        CHECK_FALSE(caps.storageWrite);
        CHECK_FALSE(caps.colorAttachment);
        CHECK_FALSE(caps.blend);
        CHECK_FALSE(caps.resolve);
    }
    destroyDevice(device);
}
