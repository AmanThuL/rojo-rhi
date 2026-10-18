//----------------------------------------------------------------------------------------------------------------------
/// @file InitialDataConformanceTests.cpp
/// @brief Observes on a GPU that the initial data of chapters 03 and 04 lands in Gpu buffers and in every subresource of a texture.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Support/Conformance.h"
#include "Support/GpuKit.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

using namespace rojoRHI;

namespace {
    /// The initial data of one subresource: the bytes `TextureData` views, padding included, and the texels without it.
    struct Subresource
    {
        std::vector<std::byte> padded; ///< What `TextureData::bytes` views.
        std::vector<std::byte> tight;  ///< The texels in tight rows, slice after slice: what a readback returns.
        uint64_t bytesPerRow = 0;      ///< The stride between rows.
        uint64_t bytesPerImage = 0;    ///< The stride between slices; nonzero for Tex3D only.
    };

    //==================================================================================================================
    /// Builds the initial data of every subresource of `desc`, whose texels are `texelBytes` bytes, with rows 16 bytes apart
    /// beyond their texels and, for Tex3D, slices 64 bytes apart beyond their rows, seeded by subresource from `seed`. It
    /// creates the texture with it into `texture`.
    std::vector<Subresource> createWithData(Device device, const TextureDesc& desc, uint32_t texelBytes, uint32_t seed, Texture& texture)
    {
        const bool volume = desc.type == TextureType::Tex3D;
        std::vector<Subresource> subresources;
        for (uint32_t index = 0; index < desc.mips * desc.layers; ++index)
        {
            const uint32_t mip = index % desc.mips;
            const uint64_t width = std::max(desc.extent.width >> mip, 1u);
            const uint64_t height = std::max(desc.extent.height >> mip, 1u);
            const uint64_t depth = volume ? std::max(desc.extent.depth >> mip, 1u) : 1u;
            Subresource sub;
            const uint64_t row = width * texelBytes;
            sub.bytesPerRow = row + 16;
            sub.bytesPerImage = volume ? sub.bytesPerRow * height + 64 : 0;
            const uint64_t image = volume ? sub.bytesPerImage : sub.bytesPerRow * height;
            sub.tight = test::pattern(row * height * depth, seed + index);
            sub.padded.assign(image * depth, std::byte{ 0 });
            for (uint64_t slice = 0; slice < depth; ++slice)
            {
                for (uint64_t y = 0; y < height; ++y)
                {
                    std::copy_n(
                        sub.tight.begin() + static_cast<std::ptrdiff_t>((slice * height + y) * row),
                        row,
                        sub.padded.begin() + static_cast<std::ptrdiff_t>(slice * image + y * sub.bytesPerRow)
                    );
                }
            }
            subresources.push_back(std::move(sub));
        }
        std::vector<TextureData> data;
        for (const Subresource& sub : subresources)
        {
            data.push_back(TextureData{ sub.padded, sub.bytesPerRow, sub.bytesPerImage });
        }
        texture = *device.createTexture(desc, data);
        return subresources;
    }

    //==================================================================================================================
    /// Checks that every subresource of `texture`, created from `desc` with `subresources`, reads back its initial texels.
    void checkEverySubresource(test::GpuKit& kit, Texture texture, const TextureDesc& desc, const std::vector<Subresource>& subresources)
    {
        for (uint32_t layer = 0; layer < desc.layers; ++layer)
        {
            for (uint32_t mip = 0; mip < desc.mips; ++mip)
            {
                CAPTURE(layer, mip);
                const TextureRegion region{ .mip = mip,
                                            .layer = layer,
                                            .extent = { std::max(desc.extent.width >> mip, 1u),
                                                        std::max(desc.extent.height >> mip, 1u),
                                                        desc.type == TextureType::Tex3D ? std::max(desc.extent.depth >> mip, 1u) : 1u } };
                CHECK(kit.readTexture(texture, region, desc.format) == subresources[layer * desc.mips + mip].tight);
            }
        }
    }
} // namespace

//======================================================================================================================
TEST_CASE("a Gpu buffer's initial data, dedicated and placed, reads back through a copy", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const std::vector<std::byte> dedicatedBytes = test::pattern(1024, 1);
    const Buffer dedicated = kit.buffer(1024, Memory::Gpu, BufferUsage::None, dedicatedBytes);
    CHECK(kit.readBuffer(dedicated) == dedicatedBytes);

    const PlacementRequirements requirements = kit.device.placementRequirements(BufferDesc{ .size = 1024 });
    const Heap heap = *kit.device.createHeap(HeapDesc{ .size = requirements.alignment + requirements.size, .label = "test.heap" });
    const std::vector<std::byte> placedBytes = test::pattern(1024, 2);
    // Placed one alignment into the heap, so its data does not start at the heap's start.
    const Buffer placed = kit.buffer(1024, Memory::Gpu, BufferUsage::None, placedBytes, Placement{ heap, requirements.alignment });
    CHECK(kit.readBuffer(placed) == placedBytes);
    CHECK(kit.readBuffer(placed, QueueType::Copy) == placedBytes);
}

//======================================================================================================================
TEST_CASE("a texture's initial data reads back from every mip and layer", "[conformance]")
{
    test::requireGpu();
    const Format format = GENERATE(Format::RGBA8Unorm, Format::RGBA16Float, Format::R8Unorm);
    CAPTURE(static_cast<int>(format));
    const uint32_t texelBytes = format == Format::RGBA16Float ? 8 : (format == Format::R8Unorm ? 1 : 4);
    test::GpuKit kit;
    const TextureDesc desc{ .type = TextureType::Tex2DArray,
                            .format = format,
                            .extent = { 8, 8, 1 },
                            .mips = 4,
                            .layers = 3,
                            .usage = TextureUsage::Sampled,
                            .label = "test.texture" };
    Texture texture;
    const std::vector<Subresource> subresources = createWithData(kit.device, desc, texelBytes, 10, texture);
    checkEverySubresource(kit, texture, desc, subresources);
}

//======================================================================================================================
TEST_CASE("a cube's faces and a volume's slices read back their initial data", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const TextureDesc cubeDesc{ .type = TextureType::Cube,
                                .format = Format::RGBA8Unorm,
                                .extent = { 4, 4, 1 },
                                .mips = 2,
                                .layers = 6,
                                .usage = TextureUsage::Sampled,
                                .label = "test.cube" };
    Texture cube;
    const std::vector<Subresource> faces = createWithData(kit.device, cubeDesc, 4, 20, cube);
    checkEverySubresource(kit, cube, cubeDesc, faces);

    const TextureDesc volumeDesc{
        .type = TextureType::Tex3D, .format = Format::RGBA8Unorm, .extent = { 8, 8, 4 }, .mips = 2, .usage = TextureUsage::Sampled, .label = "test.volume"
    };
    Texture volume;
    const std::vector<Subresource> slices = createWithData(kit.device, volumeDesc, 4, 40, volume);
    checkEverySubresource(kit, volume, volumeDesc, slices);
}
