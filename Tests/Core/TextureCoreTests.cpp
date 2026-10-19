//----------------------------------------------------------------------------------------------------------------------
/// @file TextureCoreTests.cpp
/// @brief Tests the record the core keeps for a texture, its view count, the view heap and the format layouts.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/DeviceCore.h"
#include "Core/Formats.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>

using namespace rojoRHI;

namespace {
    //==================================================================================================================
    const core::TextureRecord& recordOf(Device device, Texture texture)
    {
        return *core::coreOf(device).pool<Texture>().lookup(texture).record;
    }
} // namespace

//======================================================================================================================
TEST_CASE("a texture's record keeps its description, label and initial byte count", "[core]")
{
    Device device = *createDevice();
    const std::array<std::byte, 64> bytes = {};
    const TextureData data{ bytes, 16, 0 };
    const Texture texture = *device.createTexture(
        { .type = TextureType::Tex2DArray,
          .format = Format::RGBA8Unorm,
          .extent = { 4, 4, 1 },
          .layers = 1,
          .usage = TextureUsage::Sampled,
          .label = "test.texture" },
        std::span(&data, 1)
    );
    const core::TextureRecord& record = recordOf(device, texture);
    CHECK(record.label == "test.texture");
    CHECK(record.type == TextureType::Tex2DArray);
    CHECK(record.format == Format::RGBA8Unorm);
    CHECK(record.extent.width == 4);
    CHECK(record.usage == TextureUsage::Sampled);
    CHECK(record.initialBytes == 64);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a texture without a label is recorded as rojorhi.texture.unnamed, and a placed one as placed", "[core]")
{
    Device device = *createDevice();
    const TextureDesc desc{ .format = Format::R8Unorm, .extent = { 4, 4, 1 }, .usage = TextureUsage::Sampled };
    CHECK(recordOf(device, *device.createTexture(desc)).label == "rojorhi.texture.unnamed");
    const Heap heap = *device.createHeap({ .size = 1 << 20 });
    CHECK(recordOf(device, *device.createTexture(desc, {}, { heap, 0 })).label == "rojorhi.texture.placed.unnamed");
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a placed texture counts against its heap until it is destroyed", "[core]")
{
    Device device = *createDevice();
    const Heap heap = *device.createHeap({ .size = 1 << 20 });
    const Texture texture = *device.createTexture({ .format = Format::R8Unorm, .extent = { 4, 4, 1 }, .usage = TextureUsage::Sampled }, {}, { heap, 0 });
    CHECK(core::coreOf(device).pool<Heap>().lookup(heap).record->placedResources == 1);
    device.destroy(texture);
    CHECK(core::coreOf(device).pool<Heap>().lookup(heap).record->placedResources == 0);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a texture counts its live views and stops counting a destroyed one at once", "[core]")
{
    Device device = *createDevice();
    const Texture texture = *device.createTexture({ .format = Format::R8Unorm, .extent = { 4, 4, 1 }, .usage = TextureUsage::Sampled });
    const ViewIndex first = *device.createView(texture, {});
    const ViewIndex second = *device.createView(texture, {});
    CHECK(recordOf(device, texture).views == 2);
    device.destroy(first);
    CHECK(recordOf(device, texture).views == 1);
    device.destroy(second);
    CHECK(recordOf(device, texture).views == 0);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a format with storage writes alone takes Storage usage and a Storage view", "[core]")
{
    Device device = *createDevice();
    // RG16Float on the Metal 4 floor: written through the write-only accessors, never read through a Storage view.
    core::coreOf(device).setFormatCapabilities(
        Format::RG16Float, FormatCapabilities{ .sampled = true, .filterLinear = true, .storageWrite = true, .colorAttachment = true }
    );
    const Texture texture = *device.createTexture({ .format = Format::RG16Float, .extent = { 4, 4, 1 }, .usage = TextureUsage::Storage });
    CHECK(device.createView(texture, TextureViewDesc{ .usage = TextureUsage::Storage }).has_value());
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a full view heap is an out-of-memory error result and a retired slot frees room", "[core]")
{
    Device device = *createDevice();
    core::coreOf(device).capabilities().maxViews = 2;
    const Texture texture = *device.createTexture({ .format = Format::R8Unorm, .extent = { 4, 4, 1 }, .usage = TextureUsage::Sampled });
    const ViewIndex first = *device.createView(texture, {});
    REQUIRE(device.createView(texture, {}).has_value());
    const Result<ViewIndex> full = device.createView(texture, {});
    REQUIRE_FALSE(full.has_value());
    CHECK(full.error().category == ErrorCategory::OutOfMemory);
    device.beginFrame();
    device.destroy(first);
    device.endFrame();
    CHECK_FALSE(device.createView(texture, {}).has_value());
    device.beginFrame();
    device.endFrame();
    device.beginFrame();
    device.endFrame();
    device.beginFrame();
    CHECK(device.createView(texture, {}).has_value());
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a retired view slot is reused", "[core]")
{
    Device device = *createDevice();
    const Texture texture = *device.createTexture({ .format = Format::R8Unorm, .extent = { 4, 4, 1 }, .usage = TextureUsage::Sampled });
    device.beginFrame();
    const ViewIndex first = *device.createView(texture, {});
    device.destroy(first);
    device.endFrame();
    for (uint32_t frame = 0; frame < core::coreOf(device).framesInFlight(); ++frame)
    {
        device.beginFrame();
        device.endFrame();
    }
    device.beginFrame();
    CHECK(*device.createView(texture, {}) == first);
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("every format has a layout whose block bytes are non-zero except Undefined and D32FloatS8", "[core]")
{
    CHECK(core::formatLayout(Format::Undefined).blockBytes == 0);
    CHECK(core::formatLayout(Format::D32FloatS8).blockBytes == 0);
    for (uint32_t value = 1; value < core::kFormatCount; ++value)
    {
        const core::FormatLayout layout = core::formatLayout(static_cast<Format>(value));
        CHECK((layout.blockBytes != 0) == (static_cast<Format>(value) != Format::D32FloatS8));
        CHECK(layout.blockWidth == layout.blockHeight);
    }
    CHECK(core::formatLayout(Format::BC1Unorm).blockWidth == 4);
    CHECK(core::formatLayout(Format::BC1Unorm).blockBytes == 8);
    CHECK(core::formatLayout(Format::RGBA16Float).blockBytes == 8);
}

//======================================================================================================================
TEST_CASE("sRGB counterparts pair up and other formats have none", "[core]")
{
    CHECK(core::srgbCounterpart(Format::RGBA8Unorm) == Format::RGBA8Unorm_sRGB);
    CHECK(core::srgbCounterpart(Format::RGBA8Unorm_sRGB) == Format::RGBA8Unorm);
    CHECK(core::srgbCounterpart(Format::BC1Unorm_sRGB) == Format::BC1Unorm);
    CHECK(core::srgbCounterpart(Format::BGRA8Unorm) == Format::Undefined);
    CHECK(core::isDepthFormat(Format::D32FloatS8));
    CHECK_FALSE(core::isDepthFormat(Format::R32Float));
    CHECK(core::isBlockCompressed(Format::BC1Unorm_sRGB));
}
