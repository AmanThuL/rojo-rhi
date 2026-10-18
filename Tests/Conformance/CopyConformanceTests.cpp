//----------------------------------------------------------------------------------------------------------------------
/// @file CopyConformanceTests.cpp
/// @brief Observes the copies of chapter 07 on a GPU: the bytes each one moves and the neighbours it leaves alone.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Support/Conformance.h"
#include "Support/GpuKit.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

using namespace rojoRHI;

namespace {
    //==================================================================================================================
    /// Copies `rows` rows of `rowBytes` bytes from `source`, rows `sourcePitch` apart from `sourceOffset`, into `target`, rows
    /// `targetPitch` apart from `targetOffset`.
    void blit(
        std::vector<std::byte>& target,
        uint64_t targetOffset,
        uint64_t targetPitch,
        std::span<const std::byte> source,
        uint64_t sourceOffset,
        uint64_t sourcePitch,
        uint64_t rowBytes,
        uint64_t rows
    )
    {
        for (uint64_t row = 0; row < rows; ++row)
        {
            std::memcpy(target.data() + targetOffset + row * targetPitch, source.data() + sourceOffset + row * sourcePitch, rowBytes);
        }
    }

    //==================================================================================================================
    /// Creates a texture of `desc` whose every subresource holds tight rows of `texelBytes`-byte texels (or blocks of
    /// `blockEdge` texels), seeded by its index from `seed`, and returns the bytes of each subresource in `TextureData` order.
    std::vector<std::vector<std::byte>> fillTexture(
        Device device, const TextureDesc& desc, uint32_t texelBytes, uint32_t seed, Texture& texture, uint32_t blockEdge = 1
    )
    {
        std::vector<std::vector<std::byte>> bytes;
        std::vector<TextureData> data;
        const uint32_t subresources = desc.mips * desc.layers;
        bytes.reserve(subresources);
        for (uint32_t index = 0; index < subresources; ++index)
        {
            const uint32_t mip = index % desc.mips;
            const uint32_t width = std::max(desc.extent.width >> mip, 1u);
            const uint32_t height = std::max(desc.extent.height >> mip, 1u);
            const uint32_t depth = desc.type == TextureType::Tex3D ? std::max(desc.extent.depth >> mip, 1u) : 1u;
            const uint64_t row = uint64_t{ (width + blockEdge - 1) / blockEdge } * texelBytes;
            const uint64_t rows = (height + blockEdge - 1) / blockEdge;
            bytes.push_back(test::pattern(row * rows * depth, seed + index));
            data.push_back(TextureData{ bytes.back(), row, desc.type == TextureType::Tex3D ? row * rows : 0 });
        }
        texture = *device.createTexture(desc, data);
        return bytes;
    }

    //==================================================================================================================
    /// The whole of mip `mip` of every layer of a `width` by `height` (by `depth`) texture.
    TextureRegion wholeMip(uint32_t mip, uint32_t layers, uint32_t width, uint32_t height, uint32_t depth = 1)
    {
        return TextureRegion{
            .mip = mip, .layer = 0, .layers = layers, .extent = { std::max(width >> mip, 1u), std::max(height >> mip, 1u), std::max(depth >> mip, 1u) }
        };
    }

    //==================================================================================================================
    /// The subresources `bytes` of mip `mip` for layers `0` to `layers - 1`, one after the other, of a texture of `mips` mips.
    std::vector<std::byte> layersOf(const std::vector<std::vector<std::byte>>& bytes, uint32_t mip, uint32_t mips, uint32_t layers)
    {
        std::vector<std::byte> joined;
        for (uint32_t layer = 0; layer < layers; ++layer)
        {
            const std::vector<std::byte>& part = bytes[layer * mips + mip];
            joined.insert(joined.end(), part.begin(), part.end());
        }
        return joined;
    }
} // namespace

//======================================================================================================================
TEST_CASE("copyBuffer moves bytes between buffers on the Graphics, Compute and Copy queues", "[conformance]")
{
    test::requireGpu();
    const QueueType type = GENERATE(QueueType::Graphics, QueueType::Compute, QueueType::Copy);
    CAPTURE(static_cast<int>(type));
    test::GpuKit kit;
    const std::vector<std::byte> source = test::pattern(256, 1);
    const std::vector<std::byte> background = test::pattern(512, 2);
    const Buffer upload = kit.buffer(256, Memory::Upload, BufferUsage::None, source);
    const Buffer gpu = kit.buffer(512, Memory::Gpu, BufferUsage::None, background);
    const Buffer readback = kit.buffer(256, Memory::Readback);
    CommandList list = kit.list(type, "test.copy");
    // From an Upload buffer into a Gpu one, and straight into a Readback one.
    list.copyBuffer(upload, 64, gpu, 128, 128);
    list.copyBuffer(upload, 0, readback, 0, 256);
    kit.run(std::array{ list });
    std::vector<std::byte> expected = background;
    std::memcpy(expected.data() + 128, source.data() + 64, 128);
    CHECK(kit.readBuffer(gpu, type) == expected);
    CHECK(kit.read(readback) == source);
}

//======================================================================================================================
TEST_CASE("copyBuffer within one buffer copies between disjoint ranges", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const std::vector<std::byte> initial = test::pattern(1024, 3);
    const Buffer buffer = kit.buffer(1024, Memory::Gpu, BufferUsage::None, initial);
    CommandList list = kit.list(QueueType::Graphics, "test.copy");
    list.copyBuffer(buffer, 4, buffer, 512, 100);
    kit.run(std::array{ list });
    std::vector<std::byte> expected = initial;
    std::memcpy(expected.data() + 512, initial.data() + 4, 100);
    CHECK(kit.readBuffer(buffer) == expected);
}

//======================================================================================================================
TEST_CASE("copyBufferToTexture writes a region of one mip with rows rowPitch apart from a 512-byte offset", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const TextureDesc desc{ .format = Format::RGBA8Unorm, .extent = { 16, 16, 1 }, .mips = 3, .usage = TextureUsage::Sampled, .label = "test.texture" };
    Texture texture;
    const std::vector<std::vector<std::byte>> initial = fillTexture(kit.device, desc, 4, 10, texture);
    const std::vector<std::byte> source = test::pattern(512 + 3 * 256, 5);
    const Buffer upload = kit.buffer(source.size(), Memory::Upload, BufferUsage::None, source);
    CommandList list = kit.list(QueueType::Graphics, "test.copy");
    const TextureRegion region{ .mip = 1, .offset = { 2, 1, 0 }, .extent = { 4, 3, 1 } };
    list.copyBufferToTexture(upload, BufferTextureLayout{ .offset = 512, .rowPitch = 256 }, texture, region);
    kit.run(std::array{ list });
    // Mip 1 is 8 texels wide: 32 bytes a row.
    std::vector<std::byte> expected = initial[1];
    blit(expected, 1 * 32 + 2 * 4, 32, source, 512, 256, 4 * 4, 3);
    CHECK(kit.readTexture(texture, wholeMip(1, 1, 16, 16), Format::RGBA8Unorm) == expected);
    CHECK(kit.readTexture(texture, wholeMip(0, 1, 16, 16), Format::RGBA8Unorm) == initial[0]);
    CHECK(kit.readTexture(texture, wholeMip(2, 1, 16, 16), Format::RGBA8Unorm) == initial[2]);
}

//======================================================================================================================
TEST_CASE("copyBufferToTexture writes array layers, cube faces and 3D slices with rowsPerImage padding", "[conformance]")
{
    test::requireGpu();
    const TextureType type = GENERATE(TextureType::Tex2DArray, TextureType::Cube, TextureType::Tex3D);
    CAPTURE(static_cast<int>(type));
    test::GpuKit kit;
    const bool volume = type == TextureType::Tex3D;
    const uint32_t layers = type == TextureType::Tex2DArray ? 4 : (type == TextureType::Cube ? 6 : 1);
    const TextureDesc desc{ .type = type,
                            .format = Format::RGBA8Unorm,
                            .extent = { 8, 8, volume ? 4u : 1u },
                            .layers = layers,
                            .usage = TextureUsage::Sampled,
                            .label = "test.texture" };
    Texture texture;
    const std::vector<std::vector<std::byte>> initial = fillTexture(kit.device, desc, 4, 20, texture);
    // Five texels by four rows of each image, from layer 1 (face 2) or slice 1, three images; images six rows apart.
    TextureRegion region{ .offset = { 1, 2, 0 }, .extent = { 5, 4, 1 } };
    if (volume)
    {
        region.offset.z = 1;
        region.extent.depth = 3;
    }
    else
    {
        region.layer = type == TextureType::Cube ? 2 : 1;
        region.layers = 3;
    }
    const BufferTextureLayout layout{ .offset = 512, .rowPitch = 256, .rowsPerImage = 6 };
    const uint64_t imageBytes = 256 * 6;
    const std::vector<std::byte> source = test::pattern(512 + 3 * imageBytes, 7);
    const Buffer upload = kit.buffer(source.size(), Memory::Upload, BufferUsage::None, source);
    CommandList list = kit.list(QueueType::Graphics, "test.copy");
    list.copyBufferToTexture(upload, layout, texture, region);
    kit.run(std::array{ list });
    // Each image of the texture is 8 rows of 32 bytes; the region's images are consecutive layers or slices.
    std::vector<std::byte> expected = volume ? initial[0] : layersOf(initial, 0, 1, layers);
    const uint64_t firstImage = volume ? region.offset.z : region.layer;
    for (uint64_t image = 0; image < 3; ++image)
    {
        blit(expected, (firstImage + image) * 256 + 2 * 32 + 1 * 4, 32, source, 512 + image * imageBytes, 256, 5 * 4, 4);
    }
    CHECK(kit.readTexture(texture, wholeMip(0, layers, 8, 8, volume ? 4 : 1), Format::RGBA8Unorm) == expected);
}

//======================================================================================================================
TEST_CASE("copyBufferToTexture writes BC1 blocks", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    if (!kit.device.formatCapabilities(Format::BC1Unorm).sampled)
    {
        SKIP("the device does not sample BC1Unorm");
    }
    // 12 by 12 texels: 3 by 3 blocks at mip 0, and 6 by 6 texels (2 by 2 blocks, the last ones partly outside) at mip 1.
    const TextureDesc desc{ .format = Format::BC1Unorm, .extent = { 12, 12, 1 }, .mips = 2, .usage = TextureUsage::Sampled, .label = "test.bc1" };
    Texture texture;
    const std::vector<std::vector<std::byte>> initial = fillTexture(kit.device, desc, 8, 30, texture, 4);
    const std::vector<std::byte> source = test::pattern(1024, 9);
    const Buffer upload = kit.buffer(source.size(), Memory::Upload, BufferUsage::None, source);
    CommandList list = kit.list(QueueType::Graphics, "test.copy");
    // Two by two blocks of mip 0 from block (1, 1), and the last block of mip 1, a region reaching the mip's 6-texel edge.
    list.copyBufferToTexture(
        upload, BufferTextureLayout{ .offset = 0, .rowPitch = 256 }, texture, TextureRegion{ .offset = { 4, 4, 0 }, .extent = { 8, 8, 1 } }
    );
    list.copyBufferToTexture(
        upload, BufferTextureLayout{ .offset = 512, .rowPitch = 256 }, texture, TextureRegion{ .mip = 1, .offset = { 4, 4, 0 }, .extent = { 2, 2, 1 } }
    );
    kit.run(std::array{ list });
    std::vector<std::byte> mip0 = initial[0];
    blit(mip0, 1 * 24 + 1 * 8, 24, source, 0, 256, 2 * 8, 2);
    std::vector<std::byte> mip1 = initial[1];
    blit(mip1, 1 * 16 + 1 * 8, 16, source, 512, 256, 8, 1);
    CHECK(kit.readTexture(texture, wholeMip(0, 1, 12, 12), Format::BC1Unorm) == mip0);
    CHECK(kit.readTexture(texture, wholeMip(1, 1, 12, 12), Format::BC1Unorm) == mip1);
}

//======================================================================================================================
TEST_CASE("copyTextureToBuffer leaves the padding between rows and images untouched", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const TextureDesc desc{ .type = TextureType::Tex2DArray,
                            .format = Format::RGBA8Unorm,
                            .extent = { 4, 4, 1 },
                            .layers = 2,
                            .usage = TextureUsage::Sampled,
                            .label = "test.texture" };
    Texture texture;
    const std::vector<std::vector<std::byte>> initial = fillTexture(kit.device, desc, 4, 40, texture);
    const uint64_t imageBytes = 256 * 6;
    const uint64_t size = 512 + 2 * imageBytes;
    const Buffer readback = kit.buffer(size, Memory::Readback);
    std::memset(kit.device.cpuPointer(readback), 0xCD, size);
    CommandList list = kit.list(QueueType::Graphics, "test.copy");
    list.copyTextureToBuffer(
        texture, TextureRegion{ .layers = 2, .extent = { 4, 4, 1 } }, readback, BufferTextureLayout{ .offset = 512, .rowPitch = 256, .rowsPerImage = 6 }
    );
    kit.run(std::array{ list });
    std::vector<std::byte> expected(size, std::byte{ 0xCD });
    for (uint64_t layer = 0; layer < 2; ++layer)
    {
        blit(expected, 512 + layer * imageBytes, 256, initial[layer], 0, 16, 16, 4);
    }
    CHECK(kit.read(readback) == expected);
}

//======================================================================================================================
TEST_CASE("copyTexture copies regions between mips and layers of two textures", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const TextureDesc sourceDesc{ .type = TextureType::Tex2DArray,
                                  .format = Format::RGBA8Unorm,
                                  .extent = { 16, 16, 1 },
                                  .mips = 2,
                                  .layers = 2,
                                  .usage = TextureUsage::Sampled,
                                  .label = "test.source" };
    TextureDesc targetDesc = sourceDesc;
    targetDesc.layers = 3;
    targetDesc.label = "test.target";
    Texture source;
    Texture target;
    const std::vector<std::vector<std::byte>> sourceBytes = fillTexture(kit.device, sourceDesc, 4, 50, source);
    const std::vector<std::vector<std::byte>> targetBytes = fillTexture(kit.device, targetDesc, 4, 60, target);
    CommandList list = kit.list(QueueType::Graphics, "test.copy");
    // Mip 1 of both source layers (8 texels wide) into mip 0 of target layers 1 and 2 (16 texels wide).
    list.copyTexture(
        source,
        TextureRegion{ .mip = 1, .layer = 0, .layers = 2, .offset = { 1, 1, 0 }, .extent = { 4, 3, 1 } },
        target,
        TextureRegion{ .mip = 0, .layer = 1, .layers = 2, .offset = { 5, 6, 0 }, .extent = { 4, 3, 1 } }
    );
    kit.run(std::array{ list });
    std::vector<std::byte> mip0 = layersOf(targetBytes, 0, 2, 3);
    for (uint64_t layer = 0; layer < 2; ++layer)
    {
        blit(mip0, (layer + 1) * 1024 + 6 * 64 + 5 * 4, 64, sourceBytes[layer * 2 + 1], 1 * 32 + 1 * 4, 32, 4 * 4, 3);
    }
    CHECK(kit.readTexture(target, wholeMip(0, 3, 16, 16), Format::RGBA8Unorm) == mip0);
    CHECK(kit.readTexture(target, wholeMip(1, 3, 16, 16), Format::RGBA8Unorm) == layersOf(targetBytes, 1, 2, 3));
    CHECK(kit.readTexture(source, wholeMip(1, 2, 16, 16), Format::RGBA8Unorm) == layersOf(sourceBytes, 1, 2, 2));
}

//======================================================================================================================
TEST_CASE("copyTexture copies between an sRGB format and its linear partner bit for bit", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const auto desc = [](Format format) {
        return TextureDesc{ .format = format, .extent = { 8, 8, 1 }, .usage = TextureUsage::Sampled, .label = "test.texture" };
    };
    Texture srgb;
    Texture linear;
    Texture srgbTarget;
    Texture linearTarget;
    const std::vector<std::vector<std::byte>> srgbBytes = fillTexture(kit.device, desc(Format::RGBA8Unorm_sRGB), 4, 70, srgb);
    const std::vector<std::vector<std::byte>> linearBytes = fillTexture(kit.device, desc(Format::RGBA8Unorm), 4, 80, linear);
    fillTexture(kit.device, desc(Format::RGBA8Unorm), 4, 90, linearTarget);
    fillTexture(kit.device, desc(Format::RGBA8Unorm_sRGB), 4, 100, srgbTarget);
    CommandList list = kit.list(QueueType::Graphics, "test.copy");
    const TextureRegion whole{ .extent = { 8, 8, 1 } };
    list.copyTexture(srgb, whole, linearTarget, whole);
    list.copyTexture(linear, whole, srgbTarget, whole);
    kit.run(std::array{ list });
    CHECK(kit.readTexture(linearTarget, whole, Format::RGBA8Unorm) == srgbBytes[0]);
    CHECK(kit.readTexture(srgbTarget, whole, Format::RGBA8Unorm_sRGB) == linearBytes[0]);
}

namespace {
    //==================================================================================================================
    /// Creates the two BC1 textures of the edge cases: `interior`, 16 by 16 texels (4 by 4 blocks of 32 bytes a row), and
    /// `edged`, 12 by 12 with a second mip of 6 by 6 texels (2 by 2 blocks of 16 bytes a row, the last ones partly outside).
    /// Returns false when the device does not sample BC1Unorm.
    bool bc1Pair(
        test::GpuKit& kit,
        Texture& interior,
        Texture& edged,
        std::vector<std::vector<std::byte>>& interiorBytes,
        std::vector<std::vector<std::byte>>& edgedBytes
    )
    {
        if (!kit.device.formatCapabilities(Format::BC1Unorm).sampled)
        {
            return false;
        }
        interiorBytes = fillTexture(
            kit.device,
            TextureDesc{ .format = Format::BC1Unorm, .extent = { 16, 16, 1 }, .usage = TextureUsage::Sampled, .label = "test.interior" },
            8,
            110,
            interior,
            4
        );
        edgedBytes = fillTexture(
            kit.device,
            TextureDesc{ .format = Format::BC1Unorm, .extent = { 12, 12, 1 }, .mips = 2, .usage = TextureUsage::Sampled, .label = "test.edged" },
            8,
            120,
            edged,
            4
        );
        return true;
    }
} // namespace

//======================================================================================================================
TEST_CASE("copyTexture copies BC1 blocks from an interior region to a region reaching a mip's edge", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    Texture interior;
    Texture edged;
    std::vector<std::vector<std::byte>> interiorBytes;
    std::vector<std::vector<std::byte>> edgedBytes;
    if (!bc1Pair(kit, interior, edged, interiorBytes, edgedBytes))
    {
        SKIP("the device does not sample BC1Unorm");
    }
    // Two by two blocks from block (1, 1) of the interior texture into the whole 6-texel mip 1: 8 by 8 texels against 6 by 6,
    // equal in blocks.
    CommandList list = kit.list(QueueType::Graphics, "test.copy");
    list.copyTexture(interior, TextureRegion{ .offset = { 4, 4, 0 }, .extent = { 8, 8, 1 } }, edged, TextureRegion{ .mip = 1, .extent = { 6, 6, 1 } });
    kit.run(std::array{ list });
    std::vector<std::byte> mip1(edgedBytes[1].size());
    blit(mip1, 0, 16, interiorBytes[0], 1 * 32 + 1 * 8, 32, 2 * 8, 2);
    CHECK(kit.readTexture(edged, wholeMip(1, 1, 12, 12), Format::BC1Unorm) == mip1);
    CHECK(kit.readTexture(edged, wholeMip(0, 1, 12, 12), Format::BC1Unorm) == edgedBytes[0]);
}

//======================================================================================================================
TEST_CASE("copyTexture copies BC1 blocks from a region reaching a mip's edge to an interior region", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    Texture interior;
    Texture edged;
    std::vector<std::vector<std::byte>> interiorBytes;
    std::vector<std::vector<std::byte>> edgedBytes;
    if (!bc1Pair(kit, interior, edged, interiorBytes, edgedBytes))
    {
        SKIP("the device does not sample BC1Unorm");
    }
    CommandList list = kit.list(QueueType::Graphics, "test.copy");
    list.copyTexture(edged, TextureRegion{ .mip = 1, .extent = { 6, 6, 1 } }, interior, TextureRegion{ .offset = { 4, 4, 0 }, .extent = { 8, 8, 1 } });
    kit.run(std::array{ list });
    std::vector<std::byte> mip0 = interiorBytes[0];
    blit(mip0, 1 * 32 + 1 * 8, 32, edgedBytes[1], 0, 16, 2 * 8, 2);
    CHECK(kit.readTexture(interior, wholeMip(0, 1, 16, 16), Format::BC1Unorm) == mip0);
}
