//----------------------------------------------------------------------------------------------------------------------
/// @file ClearConformanceTests.cpp
/// @brief Observes `clearTexture` on a GPU: the value each format holds, the subresources of the range and those outside it.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Support/Conformance.h"
#include "Support/GpuKit.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

using namespace rojoRHI;

namespace {
    //==================================================================================================================
    /// The width, height and depth of mip `mip` of a texture of `desc`.
    Extent3D mipExtent(const TextureDesc& desc, uint32_t mip)
    {
        return Extent3D{ std::max(desc.extent.width >> mip, 1u),
                         std::max(desc.extent.height >> mip, 1u),
                         desc.type == TextureType::Tex3D ? std::max(desc.extent.depth >> mip, 1u) : 1u };
    }

    //==================================================================================================================
    /// Creates a texture of `desc`, with `Sampled` added to its usage as initial data needs (VU-rojo-texture-0020), whose every
    /// subresource holds distinct bytes of `texelBytes`-byte texels, seeded by its index from `seed`, and returns the bytes of
    /// each subresource in `TextureData` order: by layer, then by mip. The texture is placed at `placement`, a dedicated
    /// allocation by default.
    std::vector<std::vector<std::byte>> fillTexture(
        Device device, TextureDesc desc, uint32_t texelBytes, uint32_t seed, Texture& texture, Placement placement = {}
    )
    {
        desc.usage = desc.usage | TextureUsage::Sampled;
        std::vector<std::vector<std::byte>> bytes;
        std::vector<TextureData> data;
        const uint32_t subresources = desc.mips * desc.layers;
        bytes.reserve(subresources);
        for (uint32_t index = 0; index < subresources; ++index)
        {
            const Extent3D extent = mipExtent(desc, index % desc.mips);
            const uint64_t row = uint64_t{ extent.width } * texelBytes;
            bytes.push_back(test::pattern(row * extent.height * extent.depth, seed + index));
            data.push_back(TextureData{ bytes.back(), row, desc.type == TextureType::Tex3D ? row * extent.height : 0 });
        }
        texture = *device.createTexture(desc, data, placement);
        return bytes;
    }

    //==================================================================================================================
    /// Returns the texels of mip `mip` of layer `layer` of `texture`, a texture of `desc`.
    std::vector<std::byte> readSubresource(test::GpuKit& kit, Texture texture, const TextureDesc& desc, uint32_t mip, uint32_t layer)
    {
        return kit.readTexture(texture, TextureRegion{ .mip = mip, .layer = layer, .layers = 1, .extent = mipExtent(desc, mip) }, desc.format);
    }

    //==================================================================================================================
    /// Returns `count` copies of the texel `texel`.
    std::vector<std::byte> repeated(std::span<const std::byte> texel, uint64_t count)
    {
        std::vector<std::byte> bytes;
        bytes.reserve(texel.size() * count);
        for (uint64_t i = 0; i < count; ++i)
        {
            bytes.insert(bytes.end(), texel.begin(), texel.end());
        }
        return bytes;
    }

    //==================================================================================================================
    /// Returns the bytes of `values`, in memory order.
    template<typename T, size_t N> std::vector<std::byte> bytesOf(const std::array<T, N>& values)
    {
        std::vector<std::byte> bytes(sizeof(values));
        std::memcpy(bytes.data(), values.data(), sizeof(values));
        return bytes;
    }

    /// A clear color each format of the cases holds exactly: 0.2 and 0.6 are 51 and 153 out of 255.
    constexpr ClearColor kColor{ 1.0f, 0.0f, 0.2f, 0.6f };

    /// `kColor` as an RGBA8Unorm texel.
    const std::array<uint8_t, 4> kColorRGBA8{ 255, 0, 51, 153 };

    //==================================================================================================================
    /// Checks every subresource of `texture`, a texture of `desc` created with `initial`: those in mips `[baseMip, endMip)` of
    /// layers `[baseLayer, endLayer)` hold `texel` at every texel, every other one its initial bytes.
    void checkCleared(
        test::GpuKit& kit,
        Texture texture,
        const TextureDesc& desc,
        const std::vector<std::vector<std::byte>>& initial,
        std::span<const std::byte> texel,
        uint32_t baseMip,
        uint32_t endMip,
        uint32_t baseLayer,
        uint32_t endLayer
    )
    {
        for (uint32_t layer = 0; layer < desc.layers; ++layer)
        {
            for (uint32_t mip = 0; mip < desc.mips; ++mip)
            {
                CAPTURE(layer, mip);
                const Extent3D extent = mipExtent(desc, mip);
                const bool inside = mip >= baseMip && mip < endMip && layer >= baseLayer && layer < endLayer;
                const std::vector<std::byte> expected =
                    inside ? repeated(texel, uint64_t{ extent.width } * extent.height * extent.depth) : initial[layer * desc.mips + mip];
                CHECK(readSubresource(kit, texture, desc, mip, layer) == expected);
            }
        }
    }
} // namespace

//======================================================================================================================
TEST_CASE("clearTexture clears the mips and layers of a render target's range and nothing else", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const TextureDesc desc{ .type = TextureType::Tex2DArray,
                            .format = Format::RGBA8Unorm,
                            .extent = { 16, 16, 1 },
                            .mips = 3,
                            .layers = 4,
                            .usage = TextureUsage::RenderTarget,
                            .label = "test.target" };
    Texture texture;
    const std::vector<std::vector<std::byte>> initial = fillTexture(kit.device, desc, 4, 10, texture);
    const std::vector<std::byte> texel = bytesOf(kColorRGBA8);
    CommandList list = kit.list(QueueType::Graphics, "test.clear");
    SECTION("an inner range")
    {
        list.clearTexture(texture, SubresourceRange{ .baseMip = 1, .mipCount = 1, .baseLayer = 1, .layerCount = 2 }, ClearValue{ .color = kColor });
        kit.run(std::array{ list });
        checkCleared(kit, texture, desc, initial, texel, 1, 2, 1, 3);
    }
    SECTION("kAllMips and kAllLayers reach the last mip and layer")
    {
        list.clearTexture(texture, SubresourceRange{ .baseMip = 1, .baseLayer = 2 }, ClearValue{ .color = kColor });
        kit.run(std::array{ list });
        checkCleared(kit, texture, desc, initial, texel, 1, 3, 2, 4);
    }
}

//======================================================================================================================
TEST_CASE("clearTexture writes RGBA8Unorm, RGBA16Float and R32Float render targets with values each holds exactly", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    // 0.25, -2, 1.5 and 1024 are exact in half precision: 0x3400, 0xC000, 0x3E00 and 0x6400.
    const ClearColor wide{ 0.25f, -2.0f, 1.5f, 1024.0f };
    struct Case
    {
        Format format;
        ClearColor color;
        std::vector<std::byte> texel;
    };
    const std::array<Case, 3> cases{
        Case{ Format::RGBA8Unorm, kColor, bytesOf(kColorRGBA8) },
        Case{ Format::RGBA16Float, wide, bytesOf(std::array<uint16_t, 4>{ 0x3400, 0xC000, 0x3E00, 0x6400 }) },
        Case{ Format::R32Float, ClearColor{ 0.1f, 0.0f, 0.0f, 0.0f }, bytesOf(std::array<float, 1>{ 0.1f }) },
    };
    for (const Case& clear : cases)
    {
        CAPTURE(static_cast<int>(clear.format));
        const TextureDesc desc{ .format = clear.format, .extent = { 8, 4, 1 }, .usage = TextureUsage::RenderTarget, .label = "test.target" };
        Texture texture;
        const uint32_t texelBytes = static_cast<uint32_t>(clear.texel.size());
        const std::vector<std::vector<std::byte>> initial = fillTexture(kit.device, desc, texelBytes, 20, texture);
        CommandList list = kit.list(QueueType::Graphics, "test.clear");
        list.clearTexture(texture, {}, ClearValue{ .color = clear.color });
        kit.run(std::array{ list });
        CHECK(readSubresource(kit, texture, desc, 0, 0) == repeated(clear.texel, 8 * 4));
    }
}

//======================================================================================================================
TEST_CASE("clearTexture clears a cube render target's faces", "[conformance]")
{
    test::requireGpu();
    const TextureType type = GENERATE(TextureType::Cube, TextureType::CubeArray);
    CAPTURE(static_cast<int>(type));
    test::GpuKit kit;
    const uint32_t layers = type == TextureType::Cube ? 6 : 12;
    const TextureDesc desc{ .type = type,
                            .format = Format::RGBA8Unorm,
                            .extent = { 8, 8, 1 },
                            .mips = 2,
                            .layers = layers,
                            .usage = TextureUsage::RenderTarget,
                            .label = "test.cube" };
    Texture texture;
    const std::vector<std::vector<std::byte>> initial = fillTexture(kit.device, desc, 4, 30, texture);
    CommandList list = kit.list(QueueType::Graphics, "test.clear");
    // Faces 3 to 7 of a cube array cross from its first cube into its second.
    const uint32_t count = type == TextureType::Cube ? 3 : 5;
    list.clearTexture(texture, SubresourceRange{ .baseLayer = 3, .layerCount = count }, ClearValue{ .color = kColor });
    kit.run(std::array{ list });
    checkCleared(kit, texture, desc, initial, bytesOf(kColorRGBA8), 0, 2, 3, 3 + count);
}

//======================================================================================================================
TEST_CASE("clearTexture clears every depth plane of each mip of a 3D render target's range", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    // Mip 1 has two planes and mip 2 one; mip 0's four planes stay outside the range.
    const TextureDesc desc{
        .type = TextureType::Tex3D, .format = Format::RGBA8Unorm, .extent = { 16, 8, 4 }, .mips = 3, .usage = TextureUsage::RenderTarget, .label = "test.volume"
    };
    Texture texture;
    const std::vector<std::vector<std::byte>> initial = fillTexture(kit.device, desc, 4, 70, texture);
    CommandList list = kit.list(QueueType::Graphics, "test.clear");
    list.clearTexture(texture, SubresourceRange{ .baseMip = 1 }, ClearValue{ .color = kColor });
    kit.run(std::array{ list });
    // Each mip reads back whole, every plane of it, so a clear that missed a plane leaves that plane's initial bytes.
    checkCleared(kit, texture, desc, initial, bytesOf(kColorRGBA8), 1, 3, 0, 1);
}

//======================================================================================================================
TEST_CASE("clearTexture clears a D32Float texture to the clear depth", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const TextureDesc desc{ .type = TextureType::Tex2DArray,
                            .format = Format::D32Float,
                            .extent = { 16, 8, 1 },
                            .mips = 2,
                            .layers = 3,
                            .usage = TextureUsage::DepthStencil | TextureUsage::Sampled,
                            .label = "test.depth" };
    // Each subresource starts with its own depths in [0, 1], every texel distinct, so one the clear left alone is told apart.
    std::vector<std::vector<std::byte>> initial;
    std::vector<TextureData> data;
    for (uint32_t index = 0; index < desc.mips * desc.layers; ++index)
    {
        const Extent3D extent = mipExtent(desc, index % desc.mips);
        std::vector<float> depths(uint64_t{ extent.width } * extent.height);
        for (size_t texel = 0; texel < depths.size(); ++texel)
        {
            depths[texel] = static_cast<float>(index + 1) / 16.0f + static_cast<float>(texel) / 4096.0f;
        }
        initial.emplace_back(depths.size() * sizeof(float));
        std::memcpy(initial.back().data(), depths.data(), initial.back().size());
        data.push_back(TextureData{ initial.back(), uint64_t{ extent.width } * sizeof(float), 0 });
    }
    const Texture texture = *kit.device.createTexture(desc, data);
    CommandList list = kit.list(QueueType::Graphics, "test.clear");
    list.clearTexture(texture, SubresourceRange{ .baseMip = 1, .baseLayer = 1, .layerCount = 1 }, ClearValue{ .depth = 0.75f });
    kit.run(std::array{ list });
    checkCleared(kit, texture, desc, initial, bytesOf(std::array<float, 1>{ 0.75f }), 1, 2, 1, 2);
}

//======================================================================================================================
TEST_CASE("clearTexture clears Storage-only textures of every type", "[conformance]")
{
    test::requireGpu();
    struct Shape
    {
        TextureType type;
        Extent3D extent;
        uint32_t mips;
        uint32_t layers;
    };
    // Unequal extents catch a kernel that swaps them; each case leaves mip 0 and layer 0 outside the range where it has more.
    const Shape shape = GENERATE(
        Shape{ TextureType::Tex1D, { 64, 1, 1 }, 1, 1 },
        Shape{ TextureType::Tex1DArray, { 64, 1, 1 }, 1, 4 },
        Shape{ TextureType::Tex2D, { 16, 8, 1 }, 3, 1 },
        Shape{ TextureType::Tex2DArray, { 16, 8, 1 }, 3, 4 },
        Shape{ TextureType::Tex3D, { 16, 8, 4 }, 3, 1 },
        Shape{ TextureType::Cube, { 8, 8, 1 }, 2, 6 },
        Shape{ TextureType::CubeArray, { 8, 8, 1 }, 2, 12 }
    );
    CAPTURE(static_cast<int>(shape.type));
    test::GpuKit kit;
    const TextureDesc desc{ .type = shape.type,
                            .format = Format::RGBA8Unorm,
                            .extent = shape.extent,
                            .mips = shape.mips,
                            .layers = shape.layers,
                            .usage = TextureUsage::Storage,
                            .label = "test.storage" };
    Texture texture;
    const std::vector<std::vector<std::byte>> initial = fillTexture(kit.device, desc, 4, 40, texture);
    const uint32_t baseMip = shape.mips > 1 ? 1 : 0;
    const uint32_t baseLayer = shape.layers > 1 ? 1 : 0;
    const uint32_t endLayer = shape.layers > 2 ? shape.layers - 1 : shape.layers;
    CommandList list = kit.list(QueueType::Graphics, "test.clear");
    list.clearTexture(
        texture, SubresourceRange{ .baseMip = baseMip, .baseLayer = baseLayer, .layerCount = endLayer - baseLayer }, ClearValue{ .color = kColor }
    );
    kit.run(std::array{ list });
    checkCleared(kit, texture, desc, initial, bytesOf(kColorRGBA8), baseMip, shape.mips, baseLayer, endLayer);
}

//======================================================================================================================
TEST_CASE("clearTexture clears placed textures as it clears dedicated ones", "[conformance]")
{
    test::requireGpu();
    const TextureUsage usage = GENERATE(TextureUsage::RenderTarget, TextureUsage::Storage);
    CAPTURE(static_cast<uint32_t>(usage));
    test::GpuKit kit;
    const TextureDesc desc{ .type = TextureType::Tex2DArray,
                            .format = Format::RGBA8Unorm,
                            .extent = { 16, 8, 1 },
                            .mips = 2,
                            .layers = 3,
                            .usage = usage | TextureUsage::Sampled,
                            .label = "test.placed" };
    // One alignment into its heap, so the texture does not start at the heap's start.
    const PlacementRequirements requirements = kit.device.placementRequirements(desc);
    const Heap heap = *kit.device.createHeap(HeapDesc{ .size = requirements.alignment + requirements.size, .label = "test.heap" });
    Texture texture;
    const std::vector<std::vector<std::byte>> initial = fillTexture(kit.device, desc, 4, 60, texture, Placement{ heap, requirements.alignment });
    CommandList list = kit.list(QueueType::Graphics, "test.clear");
    list.clearTexture(texture, SubresourceRange{ .baseMip = 1, .baseLayer = 1, .layerCount = 1 }, ClearValue{ .color = kColor });
    kit.run(std::array{ list });
    checkCleared(kit, texture, desc, initial, bytesOf(kColorRGBA8), 1, 2, 1, 2);
}

//======================================================================================================================
TEST_CASE("clearTexture clears a Storage-only texture on a Compute list", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const TextureDesc desc{ .type = TextureType::Tex2DArray,
                            .format = Format::RGBA16Float,
                            .extent = { 32, 16, 1 },
                            .mips = 2,
                            .layers = 2,
                            .usage = TextureUsage::Storage,
                            .label = "test.storage" };
    Texture texture;
    const std::vector<std::vector<std::byte>> initial = fillTexture(kit.device, desc, 8, 50, texture);
    CommandList list = kit.list(QueueType::Compute, "test.clear");
    list.clearTexture(texture, SubresourceRange{ .baseLayer = 1 }, ClearValue{ .color = { 0.25f, -2.0f, 1.5f, 1024.0f } });
    kit.run(std::array{ list });
    checkCleared(kit, texture, desc, initial, bytesOf(std::array<uint16_t, 4>{ 0x3400, 0xC000, 0x3E00, 0x6400 }), 0, 2, 1, 2);
}

//======================================================================================================================
TEST_CASE("a barrier orders a load-action clear before a copy that reads it", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    // A large target, so a read left unordered would run before the clear reaches its last rows.
    constexpr uint32_t kEdge = 4096;
    // A corner of 64 texels a row reads back in rows of 256 bytes, the row pitch a copy needs (VU-rojo-cmd-0035).
    constexpr uint32_t kTail = 64;
    const TextureDesc desc{ .format = Format::RGBA8Unorm, .extent = { kEdge, kEdge, 1 }, .usage = TextureUsage::RenderTarget, .label = "test.target" };
    const Texture texture = *kit.device.createTexture(desc);
    const TextureRegion corner{ .offset = { kEdge - kTail, kEdge - kTail, 0 }, .extent = { kTail, kTail, 1 } };
    const BufferTextureLayout layout{ .rowPitch = kTail * 4 };
    const Buffer readback = kit.buffer(uint64_t{ kTail } * kTail * 4, Memory::Readback);
    const TextureBarrier clearedToRead{ .texture = texture,
                                        .srcStage = Stage::Copy,
                                        .srcAccess = Access::CopyWrite,
                                        .dstStage = Stage::Copy,
                                        .dstAccess = Access::CopyRead,
                                        .oldLayout = Layout::CopyDst,
                                        .newLayout = Layout::CopySrc };
    // Each round clears to a fresh color over what the round before left.
    constexpr uint32_t kRounds = 4;
    const auto color = [](uint32_t round) { return ClearColor{ static_cast<float>(round + 1) / 255.0f, 0.0f, 0.2f, 0.6f }; };
    const auto expected = [](uint32_t round) {
        return repeated(bytesOf(std::array<uint8_t, 4>{ static_cast<uint8_t>(round + 1), 0, 51, 153 }), uint64_t{ kTail } * kTail);
    };
    SECTION("within one list")
    {
        for (uint32_t round = 0; round < kRounds; ++round)
        {
            CAPTURE(round);
            CommandList list = kit.list(QueueType::Graphics, "test.list");
            list.clearTexture(texture, {}, ClearValue{ .color = color(round) });
            list.barrier({}, {}, std::array{ clearedToRead });
            list.copyTextureToBuffer(texture, corner, readback, layout);
            kit.run(std::array{ list });
            CHECK(kit.read(readback) == expected(round));
        }
    }
    SECTION("the read in the next list")
    {
        for (uint32_t round = 0; round < kRounds; ++round)
        {
            CAPTURE(round);
            CommandList producer = kit.list(QueueType::Graphics, "test.producer");
            producer.clearTexture(texture, {}, ClearValue{ .color = color(round) });
            producer.barrier({}, {}, std::array{ clearedToRead });
            CommandList consumer = kit.list(QueueType::Graphics, "test.consumer");
            consumer.copyTextureToBuffer(texture, corner, readback, layout);
            kit.run(std::array{ producer, consumer });
            CHECK(kit.read(readback) == expected(round));
        }
    }
}

//======================================================================================================================
TEST_CASE("a barrier orders a copy into a texture before a load-action clear of it", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    // A large copy, so a clear left unordered would finish before the copy reaches its last rows and the copy would win.
    constexpr uint32_t kEdge = 4096;
    constexpr uint32_t kRowBytes = kEdge * 4;
    // A corner of 64 texels a row reads back in rows of 256 bytes, the row pitch a copy needs (VU-rojo-cmd-0035).
    constexpr uint32_t kTail = 64;
    const TextureDesc desc{ .format = Format::RGBA8Unorm, .extent = { kEdge, kEdge, 1 }, .usage = TextureUsage::RenderTarget, .label = "test.target" };
    const Texture texture = *kit.device.createTexture(desc);
    const Buffer source = kit.buffer(uint64_t{ kRowBytes } * kEdge, Memory::Upload, BufferUsage::None, test::pattern(uint64_t{ kRowBytes } * kEdge, 80));
    const TextureRegion whole{ .extent = { kEdge, kEdge, 1 } };
    const TextureRegion corner{ .offset = { kEdge - kTail, kEdge - kTail, 0 }, .extent = { kTail, kTail, 1 } };
    const Buffer readback = kit.buffer(uint64_t{ kTail } * kTail * 4, Memory::Readback);
    const TextureBarrier copiedToCleared{ .texture = texture,
                                          .srcStage = Stage::Copy,
                                          .srcAccess = Access::CopyWrite,
                                          .dstStage = Stage::Copy,
                                          .dstAccess = Access::CopyWrite,
                                          .oldLayout = Layout::CopyDst,
                                          .newLayout = Layout::CopyDst };
    const TextureBarrier clearedToRead{ .texture = texture,
                                        .srcStage = Stage::Copy,
                                        .srcAccess = Access::CopyWrite,
                                        .dstStage = Stage::Copy,
                                        .dstAccess = Access::CopyRead,
                                        .oldLayout = Layout::CopyDst,
                                        .newLayout = Layout::CopySrc };
    // Each round copies the same bytes and clears to a fresh color.
    constexpr uint32_t kRounds = 4;
    const auto color = [](uint32_t round) { return ClearColor{ static_cast<float>(round + 1) / 255.0f, 0.0f, 0.2f, 0.6f }; };
    const auto expected = [](uint32_t round) {
        return repeated(bytesOf(std::array<uint8_t, 4>{ static_cast<uint8_t>(round + 1), 0, 51, 153 }), uint64_t{ kTail } * kTail);
    };
    SECTION("within one list")
    {
        for (uint32_t round = 0; round < kRounds; ++round)
        {
            CAPTURE(round);
            CommandList list = kit.list(QueueType::Graphics, "test.list");
            list.copyBufferToTexture(source, BufferTextureLayout{ .rowPitch = kRowBytes }, texture, whole);
            list.barrier({}, {}, std::array{ copiedToCleared });
            list.clearTexture(texture, {}, ClearValue{ .color = color(round) });
            list.barrier({}, {}, std::array{ clearedToRead });
            list.copyTextureToBuffer(texture, corner, readback, BufferTextureLayout{ .rowPitch = kTail * 4 });
            kit.run(std::array{ list });
            CHECK(kit.read(readback) == expected(round));
        }
    }
    SECTION("the clear in the next list")
    {
        for (uint32_t round = 0; round < kRounds; ++round)
        {
            CAPTURE(round);
            CommandList producer = kit.list(QueueType::Graphics, "test.producer");
            producer.copyBufferToTexture(source, BufferTextureLayout{ .rowPitch = kRowBytes }, texture, whole);
            producer.barrier({}, {}, std::array{ copiedToCleared });
            CommandList consumer = kit.list(QueueType::Graphics, "test.consumer");
            consumer.clearTexture(texture, {}, ClearValue{ .color = color(round) });
            consumer.barrier({}, {}, std::array{ clearedToRead });
            consumer.copyTextureToBuffer(texture, corner, readback, BufferTextureLayout{ .rowPitch = kTail * 4 });
            kit.run(std::array{ producer, consumer });
            CHECK(kit.read(readback) == expected(round));
        }
    }
}
