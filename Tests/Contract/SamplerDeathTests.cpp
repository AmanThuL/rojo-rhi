//----------------------------------------------------------------------------------------------------------------------
/// @file SamplerDeathTests.cpp
/// @brief Death tests for the sampler, slot-range and release rules of chapter 04, part 2.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Support/Death.h"

#include <catch2/catch_test_macros.hpp>

using namespace rojoRHI;

namespace {
    //==================================================================================================================
    Texture makeTexture(Device device)
    {
        return *device.createTexture({ .format = Format::RGBA8Unorm, .extent = { 4, 4, 1 }, .usage = TextureUsage::Sampled });
    }
} // namespace

//======================================================================================================================
TEST_CASE("VU-rojo-texture-0029 a sampler has anisotropy zero", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-texture-0029", [] {
        Device device = *createDevice();
        (void)device.createSampler({ .maxAnisotropy = 0 });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-texture-0029 a sampler has anisotropy above 16", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-texture-0029", [] {
        Device device = *createDevice();
        (void)device.createSampler({ .maxAnisotropy = 17 });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-texture-0030 an anisotropic sampler has a Nearest filter", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-texture-0030", [] {
        Device device = *createDevice();
        (void)device.createSampler({ .filter = SamplerFilter::Nearest, .maxAnisotropy = 4 });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-texture-0030 an anisotropic sampler has a Nearest mip filter", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-texture-0030", [] {
        Device device = *createDevice();
        (void)device.createSampler({ .mipFilter = SamplerFilter::Nearest, .maxAnisotropy = 2 });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-texture-0032 zero view slots are reserved", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-texture-0032", [] {
        Device device = *createDevice();
        (void)device.reserveViews(0);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-texture-0033 a view is written past the end of its range", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-texture-0033", [] {
        Device device = *createDevice();
        const Texture texture = makeTexture(device);
        const IndexRange range = *device.reserveViews(2);
        device.writeView(range, 2, texture, {});
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-texture-0033 a view is written into a sampler range", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-texture-0033", [] {
        Device device = *createDevice();
        const Texture texture = makeTexture(device);
        const IndexRange range = *device.reserveSamplers(2);
        device.writeView(range, 0, texture, {});
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-texture-0034 a view is written with a usage the texture lacks", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-texture-0034", [] {
        Device device = *createDevice();
        const Texture texture = makeTexture(device);
        const IndexRange range = *device.reserveViews(1);
        device.writeView(range, 0, texture, { .usage = TextureUsage::Storage });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-texture-0035 zero sampler slots are reserved", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-texture-0035", [] {
        Device device = *createDevice();
        (void)device.reserveSamplers(0);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-texture-0036 a sampler is written past the end of its range", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-texture-0036", [] {
        Device device = *createDevice();
        const IndexRange range = *device.reserveSamplers(2);
        device.writeSampler(range, 2, {});
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-texture-0036 a sampler is written into a view range", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-texture-0036", [] {
        Device device = *createDevice();
        const IndexRange range = *device.reserveViews(2);
        device.writeSampler(range, 0, {});
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-texture-0037 a sampler is written with anisotropy above 16", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-texture-0037", [] {
        Device device = *createDevice();
        const IndexRange range = *device.reserveSamplers(1);
        device.writeSampler(range, 0, { .maxAnisotropy = 32 });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-texture-0037 a sampler is written with anisotropy and a Nearest filter", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-texture-0037", [] {
        Device device = *createDevice();
        const IndexRange range = *device.reserveSamplers(1);
        device.writeSampler(range, 0, { .filter = SamplerFilter::Nearest, .maxAnisotropy = 2 });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-texture-0038 a view range is released twice", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-texture-0038", [] {
        Device device = *createDevice();
        const IndexRange range = *device.reserveViews(4);
        device.release(range);
        device.release(range);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-texture-0038 a sampler range is released with a modified count", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-texture-0038", [] {
        Device device = *createDevice();
        IndexRange range = *device.reserveSamplers(4);
        range.count = 2;
        device.release(range);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-device-0010 a sampler index that createSampler never returned is destroyed", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-device-0010", [] {
        Device device = *createDevice();
        device.destroy(SamplerIndex{ 7 });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-device-0010 a slot written into a reserved range is destroyed as a single", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-device-0010", [] {
        Device device = *createDevice();
        const IndexRange range = *device.reserveSamplers(2);
        device.destroy(SamplerIndex{ range.base });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-texture-0028 a texture is destroyed while a view of it sits in a range", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-texture-0028", [] {
        Device device = *createDevice();
        const Texture texture = makeTexture(device);
        const IndexRange range = *device.reserveViews(1);
        device.writeView(range, 0, texture, {});
        device.destroy(texture);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-texture-0031 a view is written into a released range", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-texture-0031", [] {
        Device device = *createDevice();
        const Texture texture = makeTexture(device);
        const IndexRange range = *device.reserveViews(2);
        device.release(range);
        device.writeView(range, 0, texture, {});
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-texture-0031 a view is written into a range with a modified base", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-texture-0031", [] {
        Device device = *createDevice();
        const Texture texture = makeTexture(device);
        IndexRange range = *device.reserveViews(4);
        range.base += 1;
        range.count = 2;
        device.writeView(range, 0, texture, {});
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-texture-0031 a sampler is written into a released range", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-texture-0031", [] {
        Device device = *createDevice();
        const IndexRange range = *device.reserveSamplers(2);
        device.release(range);
        device.writeSampler(range, 0, {});
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-texture-0031 a sampler is written into a range that was never reserved", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-texture-0031", [] {
        Device device = *createDevice();
        device.writeSampler(IndexRange{ 3, 2, IndexRange::Kind::Samplers }, 0, {});
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-texture-0031 a view is written into a range that was never reserved", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-texture-0031", [] {
        Device device = *createDevice();
        const Texture texture = makeTexture(device);
        device.writeView(IndexRange{ 3, 2, IndexRange::Kind::Views }, 0, texture, {});
    });
}
