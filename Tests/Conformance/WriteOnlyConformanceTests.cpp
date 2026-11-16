//----------------------------------------------------------------------------------------------------------------------
/// @file WriteOnlyConformanceTests.cpp
/// @brief Runs the write-only accessor kernels of the shader ABI on a GPU and checks the texels they store.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Support/Conformance.h"
#include "Support/GpuKit.h"
#include "Support/PassKit.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

using namespace rojoRHI;

namespace {
    /// Four floats, one `float4`.
    using Float4 = std::array<float, 4>;

    /// AbiWriteOnly's root.
    struct WriteRoot
    {
        uint32_t view; ///< The written view.
        uint32_t pad0; ///< Padding.
        uint32_t pad1; ///< Padding.
        uint32_t pad2; ///< Padding.
        Float4 value;  ///< The value each entry stores.
    };

    /// Where an entry of AbiWriteOnly stores: texel (`x`, `y`, `z`) of layer `layer`.
    struct Target
    {
        uint32_t x;     ///< The column.
        uint32_t y;     ///< The row.
        uint32_t z;     ///< The depth plane.
        uint32_t layer; ///< The array layer.
    };

    /// The clear color every texel holds before a kernel runs; each channel is exact in half precision.
    constexpr ClearColor kSentinel{ -7.0f, 3.0f, 0.25f, 1024.0f };

    /// The value the kernels store; each channel is exact in half precision.
    constexpr Float4 kValue{ 1.5f, -2.25f, 0.5f, 8.0f };

    /// The half-precision bits of `kSentinel` and `kValue`, channel by channel.
    constexpr std::array<uint16_t, 4> kSentinelHalves{ 0xC700, 0x4200, 0x3400, 0x6400 };
    constexpr std::array<uint16_t, 4> kValueHalves{ 0x3E00, 0xC080, 0x3800, 0x4800 };

    //==================================================================================================================
    /// The bytes of `values`, in memory order.
    template<typename T> std::vector<std::byte> bytesOf(std::span<const T> values)
    {
        std::vector<std::byte> bytes(values.size_bytes());
        std::memcpy(bytes.data(), values.data(), bytes.size());
        return bytes;
    }

    //==================================================================================================================
    /// Creates a Storage texture of `desc`, clears it to `sentinel`, stores `value` through the AbiWriteOnly kernel `entry`
    /// into a Storage view of every layer, and returns every texel read back.
    std::vector<std::byte> writeThrough(test::GpuKit& kit, const TextureDesc& desc, std::string_view entry, const ClearColor& sentinel, const Float4& value)
    {
        const Texture texture = *kit.device.createTexture(desc);
        CommandList clear = kit.list(QueueType::Graphics, "test.sentinel");
        test::transition(clear, texture, Layout::Undefined, Layout::CopyDst);
        clear.clearTexture(texture, {}, ClearValue{ .color = sentinel });
        test::transition(clear, texture, Layout::CopyDst, Layout::ShaderWrite);
        kit.run(std::array{ clear });

        const ViewIndex view = *kit.device.createView(
            texture,
            TextureViewDesc{
                .type = desc.type, .range = { .baseMip = 0, .mipCount = 1, .baseLayer = 0, .layerCount = desc.layers }, .usage = TextureUsage::Storage }
        );
        const std::vector<std::byte> blob = test::shaderLibraryFile("AbiWriteOnly");
        const ShaderLibrary library = *kit.device.loadShaderLibrary(blob, "AbiWriteOnly");
        const Pipeline pipeline = *kit.device.createPipeline(ComputePipelineDesc{ .compute = { library, entry }, .label = "test.writeOnly" });
        CommandList list = kit.list(QueueType::Graphics, "test.writeOnly");
        list.setPipeline(pipeline);
        list.pushRoot(WriteRoot{ .view = static_cast<uint32_t>(view), .pad0 = 0, .pad1 = 0, .pad2 = 0, .value = value });
        list.dispatch(1, 1, 1);
        test::transition(list, texture, Layout::ShaderWrite, Layout::CopySrc);
        kit.run(std::array{ list });
        return kit.readTexture(texture, TextureRegion{ .layers = desc.layers, .extent = desc.extent }, desc.format);
    }

    //==================================================================================================================
    /// The texels `writeThrough` reads back from a texture of `desc` when the kernel wrote `written` at `target` and every
    /// other texel holds `sentinel`.
    std::vector<std::byte> expectedTexels(
        const TextureDesc& desc, const Target& target, std::span<const std::byte> sentinel, std::span<const std::byte> written
    )
    {
        const uint64_t texels = uint64_t{ desc.extent.width } * desc.extent.height * desc.extent.depth * desc.layers;
        std::vector<std::byte> bytes;
        bytes.reserve(texels * sentinel.size());
        for (uint64_t texel = 0; texel < texels; ++texel)
        {
            bytes.insert(bytes.end(), sentinel.begin(), sentinel.end());
        }
        const uint64_t index = ((uint64_t{ target.layer } * desc.extent.depth + target.z) * desc.extent.height + target.y) * desc.extent.width + target.x;
        std::memcpy(bytes.data() + index * written.size(), written.data(), written.size());
        return bytes;
    }
} // namespace

//======================================================================================================================
TEST_CASE("a write-only accessor writes RG16Float at the floor", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    // Chapter 15 guarantees RG16Float storage writes on every device of the floor.
    REQUIRE(kit.device.formatCapabilities(Format::RG16Float).storageWrite);
    const TextureDesc desc{ .format = Format::RG16Float, .extent = { 4, 4, 1 }, .usage = TextureUsage::Storage, .label = "test.rg16" };
    const std::array<uint16_t, 2> sentinel{ kSentinelHalves[0], kSentinelHalves[1] };
    const std::array<uint16_t, 2> written{ kValueHalves[0], kValueHalves[1] };
    const std::vector<std::byte> texels = writeThrough(kit, desc, "wTexture2DMain", kSentinel, kValue);
    CHECK(texels == expectedTexels(desc, Target{ 1, 1, 0, 0 }, bytesOf<uint16_t>(sentinel), bytesOf<uint16_t>(written)));
}

//======================================================================================================================
TEST_CASE("each write-only form stores one texel of an RGBA16Float view and leaves the rest", "[conformance]")
{
    test::requireGpu();
    struct Form
    {
        std::string_view entry; ///< The AbiWriteOnly kernel.
        TextureType type;       ///< The texture and view type.
        Extent3D extent;        ///< The texture's extent.
        uint32_t layers;        ///< The texture's layers.
        Target target;          ///< Where the kernel stores.
    };
    const Form form = GENERATE(
        Form{ "wTexture1DMain", TextureType::Tex1D, { 4, 1, 1 }, 1, { 1, 0, 0, 0 } },
        Form{ "wTexture1DArrayMain", TextureType::Tex1DArray, { 4, 1, 1 }, 3, { 1, 0, 0, 1 } },
        Form{ "wTexture2DMain", TextureType::Tex2D, { 4, 4, 1 }, 1, { 1, 1, 0, 0 } },
        Form{ "wTexture2DArrayMain", TextureType::Tex2DArray, { 4, 4, 1 }, 3, { 1, 1, 0, 1 } },
        Form{ "wTexture3DMain", TextureType::Tex3D, { 4, 4, 4 }, 1, { 1, 1, 1, 0 } }
    );
    CAPTURE(form.entry);
    test::GpuKit kit;
    REQUIRE(kit.device.formatCapabilities(Format::RGBA16Float).storageWrite);
    const TextureDesc desc{
        .type = form.type, .format = Format::RGBA16Float, .extent = form.extent, .layers = form.layers, .usage = TextureUsage::Storage, .label = "test.form"
    };
    const std::vector<std::byte> texels = writeThrough(kit, desc, form.entry, kSentinel, kValue);
    CHECK(texels == expectedTexels(desc, form.target, bytesOf<uint16_t>(kSentinelHalves), bytesOf<uint16_t>(kValueHalves)));
}

//======================================================================================================================
TEST_CASE("BGRA8Unorm and RGB10A2Unorm write through a write-only accessor", "[conformance]")
{
    test::requireGpu();
    // Channels of 0 and 1 are exact in every unorm width; the red and blue channels differ, so a swapped order shows.
    constexpr ClearColor kUnormSentinel{ 0.0f, 1.0f, 1.0f, 0.0f };
    constexpr Float4 kUnormValue{ 1.0f, 0.0f, 0.0f, 1.0f };
    struct Case
    {
        Format format;                   ///< The texture's format.
        std::array<uint8_t, 4> sentinel; ///< `kUnormSentinel` as a texel, in memory order.
        std::array<uint8_t, 4> written;  ///< `kUnormValue` as a texel, in memory order.
    };
    // BGRA8Unorm stores blue, green, red, alpha bytes. RGB10A2Unorm packs red in bits 0-9, green in 10-19, blue in 20-29 and
    // alpha in 30-31 of a little-endian word: the sentinel is 0x3FFFFC00 and the value 0xC00003FF.
    const Case format = GENERATE(
        Case{ Format::BGRA8Unorm, { 255, 255, 0, 0 }, { 0, 0, 255, 255 } }, Case{ Format::RGB10A2Unorm, { 0x00, 0xFC, 0xFF, 0x3F }, { 0xFF, 0x03, 0x00, 0xC0 } }
    );
    CAPTURE(static_cast<uint32_t>(format.format));
    test::GpuKit kit;
    REQUIRE(kit.device.formatCapabilities(format.format).storageWrite);
    const TextureDesc desc{ .format = format.format, .extent = { 4, 4, 1 }, .usage = TextureUsage::Storage, .label = "test.unorm" };
    const std::vector<std::byte> texels = writeThrough(kit, desc, "wTexture2DMain", kUnormSentinel, kUnormValue);
    CHECK(texels == expectedTexels(desc, Target{ 1, 1, 0, 0 }, bytesOf<uint8_t>(format.sentinel), bytesOf<uint8_t>(format.written)));
}
