//----------------------------------------------------------------------------------------------------------------------
/// @file RenderPassConformanceTests.cpp
/// @brief Observes render passes on a GPU: the load and store operations, the subresources an attachment names, resolves and depth.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Support/Conformance.h"
#include "Support/GpuKit.h"
#include "Support/PassKit.h"

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
    /// The width and height of mip `mip` of a texture of `desc`.
    Extent3D mipExtent(const TextureDesc& desc, uint32_t mip)
    {
        return Extent3D{ std::max(desc.extent.width >> mip, 1u), std::max(desc.extent.height >> mip, 1u), 1 };
    }

    //==================================================================================================================
    /// Creates a texture of `desc`, with `Sampled` added to its usage as initial data needs (VU-rojo-texture-0020), whose every
    /// subresource holds distinct bytes of `texelBytes`-byte texels, seeded by its index from `seed`, and returns the bytes of
    /// each subresource in `TextureData` order: by layer, then by mip. It starts in `ShaderRead`. The texture is placed at
    /// `placement`, a dedicated allocation by default.
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
            bytes.push_back(test::pattern(row * extent.height, seed + index));
            data.push_back(TextureData{ bytes.back(), row, 0 });
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

    //==================================================================================================================
    /// A pass over `colors` and `depth` that renders to `extent`.
    RenderPassDesc passOf(std::span<const ColorAttachment> colors, Extent3D extent, const DepthAttachment& depth = {})
    {
        return RenderPassDesc{ .colors = colors, .depth = depth, .area = { 0, 0, extent.width, extent.height }, .label = "test.pass" };
    }

    //==================================================================================================================
    /// The subresource `mip` and `layer` alone.
    SubresourceRange only(uint32_t mip, uint32_t layer)
    {
        return SubresourceRange{ .baseMip = mip, .mipCount = 1, .baseLayer = layer, .layerCount = 1 };
    }

    /// A clear color each 8-bit normalized format holds exactly: 0.2 and 0.6 are 51 and 153 out of 255.
    constexpr ClearColor kUnormColor{ 1.0f, 0.0f, 0.2f, 0.6f };

    /// `kUnormColor` as an RGBA8Unorm texel.
    constexpr std::array<uint8_t, 4> kUnormTexel{ 255, 0, 51, 153 };

    /// A clear color each float format holds exactly.
    constexpr ClearColor kFloatColor{ 1.0f, 0.0f, 0.25f, -2.0f };

    /// One format a pass clears, with the clear color and the texel it must write.
    struct ClearCase
    {
        Format format;                ///< The attachment's format.
        uint32_t texelBytes;          ///< Bytes per texel.
        ClearColor clear;             ///< The clear color.
        std::vector<std::byte> texel; ///< The texel the clear writes, in memory order.
    };
} // namespace

//======================================================================================================================
TEST_CASE("a pass with Clear and Store writes the clear color to each color format", "[conformance]")
{
    test::requireGpu();
    // Every texel is exact: the unorm channels are multiples of 1/255 (1/3 of 1023 for RGB10A2), the float ones powers of two.
    const ClearCase format = GENERATE(
        ClearCase{ Format::RGBA8Unorm, 4, kUnormColor, bytesOf(kUnormTexel) },
        ClearCase{ Format::BGRA8Unorm, 4, kUnormColor, bytesOf(std::array<uint8_t, 4>{ 51, 0, 255, 153 }) },
        ClearCase{ Format::R8Unorm, 1, kUnormColor, bytesOf(std::array<uint8_t, 1>{ 255 }) },
        ClearCase{ Format::RGBA16Float, 8, kFloatColor, bytesOf(std::array<uint16_t, 4>{ 0x3C00, 0x0000, 0x3400, 0xC000 }) },
        ClearCase{ Format::RG16Float, 4, ClearColor{ 0.25f, -2.0f, 0.0f, 0.0f }, bytesOf(std::array<uint16_t, 2>{ 0x3400, 0xC000 }) },
        ClearCase{ Format::R16Float, 2, ClearColor{ -2.0f, 0.0f, 0.0f, 0.0f }, bytesOf(std::array<uint16_t, 1>{ 0xC000 }) },
        ClearCase{ Format::R32Float, 4, ClearColor{ 0.25f, 0.0f, 0.0f, 0.0f }, bytesOf(std::array<float, 1>{ 0.25f }) },
        ClearCase{ Format::RGB10A2Unorm, 4, ClearColor{ 1.0f, 1.0f / 3.0f, 0.0f, 1.0f }, bytesOf(std::array<uint32_t, 1>{ 0xC00557FFu }) }
    );
    CAPTURE(static_cast<uint32_t>(format.format));
    test::GpuKit kit;
    const TextureDesc desc{
        .type = TextureType::Tex2D, .format = format.format, .extent = { 16, 8, 1 }, .usage = TextureUsage::RenderTarget, .label = "test.target"
    };
    Texture texture;
    fillTexture(kit.device, desc, format.texelBytes, 3, texture);
    CommandList list = kit.list(QueueType::Graphics, "test.pass");
    test::transition(list, texture, Layout::ShaderRead, Layout::ColorAttachment);
    const std::array colors{ ColorAttachment{ .texture = texture, .load = LoadOp::Clear, .clear = format.clear } };
    list.beginRenderPass(passOf(colors, desc.extent));
    list.endRenderPass();
    test::transition(list, texture, Layout::ColorAttachment, Layout::CopySrc);
    kit.run(std::array{ list });
    CHECK(readSubresource(kit, texture, desc, 0, 0) == repeated(format.texel, 16 * 8));
}

//======================================================================================================================
TEST_CASE("a pass with Load and Store keeps the attachment's contents", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const TextureDesc desc{
        .type = TextureType::Tex2D, .format = Format::RGBA16Float, .extent = { 16, 16, 1 }, .usage = TextureUsage::RenderTarget, .label = "test.target"
    };
    Texture texture;
    const std::vector<std::vector<std::byte>> initial = fillTexture(kit.device, desc, 8, 20, texture);
    CommandList list = kit.list(QueueType::Graphics, "test.pass");
    test::transition(list, texture, Layout::ShaderRead, Layout::ColorAttachment);
    const std::array colors{ ColorAttachment{ .texture = texture, .load = LoadOp::Load, .store = StoreOp::Store, .clear = kFloatColor } };
    list.beginRenderPass(passOf(colors, desc.extent));
    list.endRenderPass();
    test::transition(list, texture, Layout::ColorAttachment, Layout::CopySrc);
    kit.run(std::array{ list });
    CHECK(readSubresource(kit, texture, desc, 0, 0) == initial[0]);
}

//======================================================================================================================
TEST_CASE("a pass clears only the mip and layer it names, and a cube face as a layer", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const std::vector<std::byte> texel = bytesOf(kUnormTexel);
    SECTION("a mip and a layer of an array texture, dedicated or placed")
    {
        const bool placed = GENERATE(false, true);
        CAPTURE(placed);
        const TextureDesc desc{ .type = TextureType::Tex2DArray,
                                .format = Format::RGBA8Unorm,
                                .extent = { 16, 16, 1 },
                                .mips = 3,
                                .layers = 3,
                                .usage = TextureUsage::RenderTarget | TextureUsage::Sampled,
                                .label = "test.array" };
        Placement placement;
        if (placed)
        {
            // One alignment into its heap, so the texture does not start at the heap's start.
            const PlacementRequirements requirements = kit.device.placementRequirements(desc);
            placement = Placement{ *kit.device.createHeap(HeapDesc{ .size = requirements.alignment + requirements.size, .label = "test.heap" }),
                                   requirements.alignment };
        }
        Texture texture;
        const std::vector<std::vector<std::byte>> initial = fillTexture(kit.device, desc, 4, 30, texture, placement);
        CommandList list = kit.list(QueueType::Graphics, "test.pass");
        test::transition(list, texture, Layout::ShaderRead, Layout::ColorAttachment, only(1, 2));
        const std::array colors{ ColorAttachment{ .texture = texture, .mip = 1, .layer = 2, .load = LoadOp::Clear, .clear = kUnormColor } };
        list.beginRenderPass(passOf(colors, mipExtent(desc, 1)));
        list.endRenderPass();
        test::transition(list, texture, Layout::ColorAttachment, Layout::CopySrc, only(1, 2));
        kit.run(std::array{ list });
        for (uint32_t layer = 0; layer < desc.layers; ++layer)
        {
            for (uint32_t mip = 0; mip < desc.mips; ++mip)
            {
                CAPTURE(layer, mip);
                const Extent3D extent = mipExtent(desc, mip);
                const std::vector<std::byte> expected =
                    mip == 1 && layer == 2 ? repeated(texel, uint64_t{ extent.width } * extent.height) : initial[layer * desc.mips + mip];
                CHECK(readSubresource(kit, texture, desc, mip, layer) == expected);
            }
        }
    }
    SECTION("a face of a cube")
    {
        const TextureDesc desc{ .type = TextureType::Cube,
                                .format = Format::RGBA8Unorm,
                                .extent = { 8, 8, 1 },
                                .layers = 6,
                                .usage = TextureUsage::RenderTarget,
                                .label = "test.cube" };
        Texture texture;
        const std::vector<std::vector<std::byte>> initial = fillTexture(kit.device, desc, 4, 40, texture);
        CommandList list = kit.list(QueueType::Graphics, "test.pass");
        test::transition(list, texture, Layout::ShaderRead, Layout::ColorAttachment, only(0, 4));
        const std::array colors{ ColorAttachment{ .texture = texture, .layer = 4, .load = LoadOp::Clear, .clear = kUnormColor } };
        list.beginRenderPass(passOf(colors, desc.extent));
        list.endRenderPass();
        test::transition(list, texture, Layout::ColorAttachment, Layout::CopySrc, only(0, 4));
        kit.run(std::array{ list });
        for (uint32_t face = 0; face < 6; ++face)
        {
            CAPTURE(face);
            CHECK(readSubresource(kit, texture, desc, 0, face) == (face == 4 ? repeated(texel, 8 * 8) : initial[face]));
        }
    }
}

//======================================================================================================================
TEST_CASE("a pass clears every color attachment it names, with its own clear color", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const TextureDesc desc{
        .type = TextureType::Tex2D, .format = Format::RGBA8Unorm, .extent = { 8, 8, 1 }, .usage = TextureUsage::RenderTarget, .label = "test.target"
    };
    std::array<Texture, 8> textures{};
    std::array<ColorAttachment, 8> colors{};
    std::array<std::array<uint8_t, 4>, 8> texels{};
    CommandList list = kit.list(QueueType::Graphics, "test.pass");
    for (uint32_t i = 0; i < 8; ++i)
    {
        fillTexture(kit.device, desc, 4, 50 + i, textures[i]);
        test::transition(list, textures[i], Layout::ShaderRead, Layout::ColorAttachment);
        // Each channel a multiple of 1/255, distinct per attachment.
        texels[i] = { static_cast<uint8_t>(20 + 25 * i), static_cast<uint8_t>(230 - 25 * i), static_cast<uint8_t>(3 * i + 1), 255 };
        const ClearColor clear{ texels[i][0] / 255.0f, texels[i][1] / 255.0f, texels[i][2] / 255.0f, 1.0f };
        colors[i] = ColorAttachment{ .texture = textures[i], .load = LoadOp::Clear, .clear = clear };
    }
    list.beginRenderPass(passOf(colors, desc.extent));
    list.endRenderPass();
    for (const Texture texture : textures)
    {
        test::transition(list, texture, Layout::ColorAttachment, Layout::CopySrc);
    }
    kit.run(std::array{ list });
    for (uint32_t i = 0; i < 8; ++i)
    {
        CAPTURE(i);
        CHECK(readSubresource(kit, textures[i], desc, 0, 0) == repeated(bytesOf(texels[i]), 8 * 8));
    }
}

//======================================================================================================================
TEST_CASE("a pass with Clear and a resolve target writes the clear color to the resolve target", "[conformance]")
{
    test::requireGpu();
    const StoreOp store = GENERATE(StoreOp::DontCare, StoreOp::Store);
    CAPTURE(static_cast<uint32_t>(store));
    test::GpuKit kit;
    const TextureDesc desc{
        .type = TextureType::Tex2D, .format = Format::RGBA8Unorm, .extent = { 16, 16, 1 }, .usage = TextureUsage::RenderTarget, .label = "test.resolve"
    };
    Texture resolve;
    fillTexture(kit.device, desc, 4, 60, resolve);
    TextureDesc multisampleDesc = desc;
    multisampleDesc.samples = 4;
    multisampleDesc.label = "test.multisample";
    const Texture multisample = *kit.device.createTexture(multisampleDesc);
    CommandList list = kit.list(QueueType::Graphics, "test.pass");
    test::transition(list, multisample, Layout::Undefined, Layout::ColorAttachment);
    test::transition(list, resolve, Layout::ShaderRead, Layout::ColorAttachment);
    const std::array colors{ ColorAttachment{ .texture = multisample, .load = LoadOp::Clear, .store = store, .clear = kUnormColor, .resolve = resolve } };
    list.beginRenderPass(passOf(colors, desc.extent));
    list.endRenderPass();
    test::transition(list, resolve, Layout::ColorAttachment, Layout::CopySrc);
    kit.run(std::array{ list });
    CHECK(readSubresource(kit, resolve, desc, 0, 0) == repeated(bytesOf(kUnormTexel), 16 * 16));
}

//======================================================================================================================
TEST_CASE("a pass clears a D32Float depth attachment to its clear depth", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const TextureDesc desc{
        .type = TextureType::Tex2D, .format = Format::D32Float, .extent = { 16, 8, 1 }, .usage = TextureUsage::DepthStencil, .label = "test.depth"
    };
    Texture texture;
    fillTexture(kit.device, desc, 4, 70, texture);
    CommandList list = kit.list(QueueType::Graphics, "test.pass");
    test::transition(list, texture, Layout::ShaderRead, Layout::DepthStencilWrite);
    // The pass's only attachment is depth.
    list.beginRenderPass(passOf({}, desc.extent, DepthAttachment{ .texture = texture, .load = LoadOp::Clear, .clearDepth = 0.375f }));
    list.endRenderPass();
    test::transition(list, texture, Layout::DepthStencilWrite, Layout::CopySrc);
    kit.run(std::array{ list });
    CHECK(readSubresource(kit, texture, desc, 0, 0) == repeated(bytesOf(std::array<float, 1>{ 0.375f }), 16 * 8));
}

//======================================================================================================================
TEST_CASE("a pass with a D32FloatS8 attachment clears depth and stencil under the validation layer", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const TextureDesc desc{
        .type = TextureType::Tex2D, .format = Format::D32FloatS8, .extent = { 16, 16, 1 }, .usage = TextureUsage::DepthStencil, .label = "test.depth"
    };
    const Texture texture = *kit.device.createTexture(desc);
    const std::array colorTargets{ ColorAttachment{
        .texture = *kit.device.createTexture(
            TextureDesc{
                .type = TextureType::Tex2D, .format = Format::RGBA8Unorm, .extent = { 16, 16, 1 }, .usage = TextureUsage::RenderTarget, .label = "test.color" }
        ),
        .load = LoadOp::Clear } };
    CommandList list = kit.list(QueueType::Graphics, "test.pass");
    test::transition(list, texture, Layout::Undefined, Layout::DepthStencilWrite);
    test::transition(list, colorTargets[0].texture, Layout::Undefined, Layout::ColorAttachment);
    // Depth discarded and stencil stored, then both stored; a stencil format is never copied (VU-rojo-cmd-0041), so the
    // validation layer is the observer.
    list.beginRenderPass(passOf(
        colorTargets,
        desc.extent,
        DepthAttachment{ .texture = texture,
                         .load = LoadOp::Clear,
                         .store = StoreOp::DontCare,
                         .clearDepth = 1.0f,
                         .stencilLoad = LoadOp::Clear,
                         .stencilStore = StoreOp::Store,
                         .clearStencil = 7 }
    ));
    list.endRenderPass();
    test::transition(list, texture, Layout::DepthStencilWrite, Layout::DepthStencilWrite);
    list.beginRenderPass(passOf(
        {},
        desc.extent,
        DepthAttachment{
            .texture = texture, .load = LoadOp::Clear, .clearDepth = 0.5f, .stencilLoad = LoadOp::Load, .stencilStore = StoreOp::Store, .clearStencil = 0 }
    ));
    list.endRenderPass();
    kit.run(std::array{ list });
}

//======================================================================================================================
TEST_CASE("back-to-back passes on one target with a barrier between them keep the second's clear", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const TextureDesc desc{
        .type = TextureType::Tex2D, .format = Format::RGBA8Unorm, .extent = { 16, 16, 1 }, .usage = TextureUsage::RenderTarget, .label = "test.target"
    };
    Texture texture;
    fillTexture(kit.device, desc, 4, 80, texture);
    CommandList list = kit.list(QueueType::Graphics, "test.pass");
    test::transition(list, texture, Layout::ShaderRead, Layout::ColorAttachment);
    const std::array first{ ColorAttachment{ .texture = texture, .load = LoadOp::Clear, .clear = ClearColor{ 0.0f, 1.0f, 0.0f, 1.0f } } };
    list.beginRenderPass(passOf(first, desc.extent));
    list.endRenderPass();
    test::transition(list, texture, Layout::ColorAttachment, Layout::ColorAttachment);
    const std::array second{ ColorAttachment{ .texture = texture, .load = LoadOp::Clear, .clear = kUnormColor } };
    list.beginRenderPass(passOf(second, desc.extent));
    list.endRenderPass();
    test::transition(list, texture, Layout::ColorAttachment, Layout::CopySrc);
    kit.run(std::array{ list });
    CHECK(readSubresource(kit, texture, desc, 0, 0) == repeated(bytesOf(kUnormTexel), 16 * 16));
}
