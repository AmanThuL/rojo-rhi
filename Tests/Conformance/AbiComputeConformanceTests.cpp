//----------------------------------------------------------------------------------------------------------------------
/// @file AbiComputeConformanceTests.cpp
/// @brief Runs the compute conformance shaders of the shader ABI on a GPU and checks what they store.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "AbiExamples.h"
#include "Support/Backend.h"
#include "Support/Conformance.h"
#include "Support/GpuKit.h"
#include "Support/PassKit.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <bit>
#include <cmath>
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

    /// The shader ABI version `ROJORHI_SHADER_ABI_VERSION` holds.
    constexpr uint32_t kAbiVersion = 1;

    /// AbiTextures' root.
    struct FetchRoot
    {
        uint32_t view;       ///< The fetched view.
        uint32_t sampler;    ///< The linear-clamp sampler.
        uint32_t comparison; ///< The comparison sampler.
        uint32_t pad;        ///< Padding.
        Addr output;         ///< Where the entry stores its `float4`.
    };

    /// The edge of the textures, in texels.
    constexpr uint32_t kEdge = 4;

    //==================================================================================================================
    /// Loads the conformance library `name` on `device`.
    ShaderLibrary loadLibrary(Device device, std::string_view name)
    {
        const std::vector<std::byte> blob = test::shaderLibraryFile(name);
        return *device.loadShaderLibrary(blob, name);
    }

    //==================================================================================================================
    /// Creates the compute pipeline of `entry` of `library` on `device`.
    Pipeline computeOf(Device device, ShaderLibrary library, std::string_view entry)
    {
        return *device.createPipeline(ComputePipelineDesc{ .compute = { library, entry }, .label = "test.compute" });
    }

    //==================================================================================================================
    /// The bytes of `values`.
    template<class T> std::vector<std::byte> bytesOf(std::span<const T> values)
    {
        std::vector<std::byte> bytes(values.size_bytes());
        std::memcpy(bytes.data(), values.data(), bytes.size());
        return bytes;
    }

    //==================================================================================================================
    /// The value texel (`x`, `y`, `z`) of layer `layer` of a fetched texture holds: distinct for every texel, exact in
    /// half precision.
    Float4 texelValue(uint32_t layer, uint32_t x, uint32_t y, uint32_t z)
    {
        return Float4{ static_cast<float>(layer * 100 + z * 16 + y * 4 + x + 1), 0.5f, 0.25f, 8.0f };
    }

    //==================================================================================================================
    /// What a linear sample at the centre of layer `layer` returns: the mean of the texels 1 and 2 along each axis the texture
    /// has (`rows` adds the y axis, `planes` the z axis), each weighted equally.
    Float4 centerValue(uint32_t layer, bool rows, bool planes)
    {
        Float4 sum{};
        uint32_t count = 0;
        for (uint32_t z = planes ? 1 : 0; z <= (planes ? 2u : 0u); ++z)
        {
            for (uint32_t y = rows ? 1 : 0; y <= (rows ? 2u : 0u); ++y)
            {
                for (uint32_t x = 1; x <= 2; ++x)
                {
                    const Float4 value = texelValue(layer, x, y, z);
                    for (size_t channel = 0; channel < 4; ++channel)
                    {
                        sum[channel] += value[channel];
                    }
                    ++count;
                }
            }
        }
        for (float& channel : sum)
        {
            channel /= static_cast<float>(count);
        }
        return sum;
    }

    //==================================================================================================================
    /// The half-precision bits of `value`, which is zero or a normal half-precision value held exactly.
    uint16_t halfOf(float value)
    {
        if (value == 0.0f)
        {
            return 0;
        }
        const auto bits = std::bit_cast<uint32_t>(value);
        const uint32_t sign = (bits >> 16) & 0x8000u;
        const auto exponent = static_cast<uint32_t>(static_cast<int32_t>((bits >> 23) & 0xFFu) - 127 + 15);
        return static_cast<uint16_t>(sign | (exponent << 10) | ((bits >> 13) & 0x3FFu));
    }

    //==================================================================================================================
    /// Creates a `RGBA16Float` texture of `type` with `layers` layers, one mip and the usage `usage` besides `Sampled`, in
    /// whose texels hold `texelValue`.
    Texture patternTexture(Device device, TextureType type, uint32_t layers, TextureUsage usage)
    {
        const bool oneDimensional = type == TextureType::Tex1D || type == TextureType::Tex1DArray;
        const Extent3D extent{ kEdge, oneDimensional ? 1u : kEdge, type == TextureType::Tex3D ? kEdge : 1u };
        const uint32_t texels = extent.width * extent.height * extent.depth;
        std::vector<std::vector<std::byte>> bytes;
        std::vector<TextureData> data;
        for (uint32_t layer = 0; layer < layers; ++layer)
        {
            std::vector<uint16_t> layerTexels;
            for (uint32_t texel = 0; texel < texels; ++texel)
            {
                const Float4 value = texelValue(layer, texel % extent.width, texel / extent.width % extent.height, texel / (extent.width * extent.height));
                for (const float channel : value)
                {
                    layerTexels.push_back(halfOf(channel));
                }
            }
            bytes.push_back(bytesOf<uint16_t>(layerTexels));
            constexpr uint64_t kTexelBytes = 4 * sizeof(uint16_t);
            data.push_back(
                TextureData{ bytes.back(),
                             uint64_t{ extent.width } * kTexelBytes,
                             type == TextureType::Tex3D ? uint64_t{ extent.width } * extent.height * kTexelBytes : 0 }
            );
        }
        return *device.createTexture(
            TextureDesc{ .type = type,
                         .format = Format::RGBA16Float,
                         .extent = extent,
                         .layers = layers,
                         .usage = TextureUsage::Sampled | usage,
                         .label = "test.pattern" },
            data
        );
    }

    //==================================================================================================================
    /// Runs the kernel `entry` of AbiTextures with the root `root`, whose output the helper fills in, and returns the `float4`
    /// it stored.
    Float4 fetch(test::GpuKit& kit, std::string_view entry, FetchRoot root)
    {
        const ShaderLibrary library = loadLibrary(kit.device, "AbiTextures");
        const Pipeline pipeline = computeOf(kit.device, library, entry);
        const std::array<uint32_t, 4> untouched{ 0xDEADBEEFu, 0xDEADBEEFu, 0xDEADBEEFu, 0xDEADBEEFu };
        const Buffer output = kit.buffer(sizeof(untouched), Memory::Gpu, BufferUsage::ShaderWrite, bytesOf<uint32_t>(untouched));
        root.output = kit.device.address(output);
        CommandList list = kit.list(QueueType::Graphics, "test.fetch");
        list.setPipeline(pipeline);
        list.pushRoot(root);
        list.dispatch(1, 1, 1);
        kit.run(std::array{ list });
        const std::vector<std::byte> bytes = kit.readBuffer(output);
        Float4 value{};
        std::memcpy(value.data(), bytes.data(), sizeof(value));
        return value;
    }

    //==================================================================================================================
    /// Clears layer `layer` of the multisample `texture` to (1, 0, 1, 1) through a render pass and leaves it readable by a
    /// shader, as the fetch kernels need. Every layer changes layout together, so no view spans mixed layouts.
    void clearMultisample(test::GpuKit& kit, Texture texture, uint32_t layer)
    {
        CommandList list = kit.list(QueueType::Graphics, "test.clearMultisample");
        test::transition(list, texture, Layout::Undefined, Layout::ColorAttachment);
        const std::array colors{ ColorAttachment{
            .texture = texture, .layer = layer, .load = LoadOp::Clear, .store = StoreOp::Store, .clear = ClearColor{ 1.0f, 0.0f, 1.0f, 1.0f } } };
        list.beginRenderPass(RenderPassDesc{ .colors = colors, .area = { 0, 0, kEdge, kEdge }, .label = "test.clearMultisample.pass" });
        list.endRenderPass();
        test::transition(list, texture, Layout::ColorAttachment, Layout::ShaderRead);
        kit.run(std::array{ list });
    }

    //==================================================================================================================
    /// Creates a linear-clamp sampler on `device` and returns its index.
    uint32_t linearClamp(Device device)
    {
        const SamplerIndex index = *device.createSampler(
            SamplerDesc{ .filter = SamplerFilter::Linear, .mipFilter = SamplerFilter::Nearest, .address = SamplerAddress::ClampToEdge, .label = "test.linear" }
        );
        return static_cast<uint32_t>(index);
    }

    //==================================================================================================================
    /// Fetches a sampled view of the patterned texture of `type` and `layers` layers through the kernel `entry`, over a view of
    /// `viewType` and `viewLayers` layers.
    Float4 sampledFetch(test::GpuKit& kit, std::string_view entry, TextureType type, uint32_t layers, TextureType viewType, uint32_t viewLayers)
    {
        const Texture texture = patternTexture(kit.device, type, layers, TextureUsage::None);
        const ViewIndex view = *kit.device.createView(
            texture,
            TextureViewDesc{
                .type = viewType, .range = { .baseMip = 0, .mipCount = 1, .baseLayer = 0, .layerCount = viewLayers }, .usage = TextureUsage::Sampled }
        );
        return fetch(kit, entry, FetchRoot{ .view = static_cast<uint32_t>(view), .sampler = linearClamp(kit.device) });
    }

    //==================================================================================================================
    /// Fetches a storage view of the patterned texture of `type` and `layers` layers through the kernel `entry`, over a view of
    /// `viewType` and `viewLayers` layers.
    Float4 storageFetch(
        test::GpuKit& kit, std::string_view entry, TextureType type, uint32_t layers, TextureType viewType, uint32_t viewLayers, Texture* created = nullptr
    )
    {
        const Texture texture = patternTexture(kit.device, type, layers, TextureUsage::Storage);
        if (created != nullptr)
        {
            *created = texture;
        }
        // Initial data leaves the texture in ShaderRead, which permits no Storage view access (chapter 09).
        CommandList move = kit.list(QueueType::Graphics, "test.storage");
        test::transition(move, texture, Layout::ShaderRead, Layout::ShaderWrite);
        kit.run(std::array{ move });
        const ViewIndex view = *kit.device.createView(
            texture,
            TextureViewDesc{
                .type = viewType, .range = { .baseMip = 0, .mipCount = 1, .baseLayer = 0, .layerCount = viewLayers }, .usage = TextureUsage::Storage }
        );
        return fetch(kit, entry, FetchRoot{ .view = static_cast<uint32_t>(view) });
    }
} // namespace

//======================================================================================================================
TEST_CASE("AbiCompute loads, stores and reads the ABI version through the root", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const ShaderLibrary library = loadLibrary(kit.device, "AbiCompute");
    const Pipeline pipeline = computeOf(kit.device, library, "computeMain");
    const std::array<Float4, 3> vertices{ Float4{ 1, 2, 3, 4 }, Float4{ 5, 6, 7, 8 }, Float4{ 9, 10, 11, 12 } };
    const std::array<Float4, 3> instances{ Float4{ 20, 21, 22, 23 }, Float4{ 30, 31, 32, 33 }, Float4{ 40, 41, 42, 43 } };
    const Buffer vertexBuffer = kit.buffer(sizeof(vertices), Memory::Gpu, BufferUsage::ShaderWrite, bytesOf<Float4>(vertices));
    const Buffer instanceBuffer = kit.buffer(sizeof(instances), Memory::Gpu, BufferUsage::ShaderWrite, bytesOf<Float4>(instances));
    constexpr uint32_t kAlbedo = 6;
    constexpr uint32_t kDrawId = 2;
    CommandList list = kit.list(QueueType::Graphics, "test.abiCompute");
    list.setPipeline(pipeline);
    list.pushRoot(
        test::DrawRoot{ .vertices = kit.device.address(vertexBuffer),
                        .instances = kit.device.address(instanceBuffer),
                        .albedo = kAlbedo,
                        .linearClamp = 0,
                        .drawId = kDrawId,
                        .pad = 0 }
    );
    list.dispatch(1, 1, 1);
    kit.run(std::array{ list });
    // The kernel's three stores, applied in program order on the host with thread 0: the sum into instances[0], the loaded
    // instance into vertices[0], then the `uint` that overwrites instances[0].y.
    std::array<Float4, 3> expectedVertices = vertices;
    std::array<Float4, 3> expectedInstances = instances;
    const Float4 v = vertices[0];
    const Float4 w = instances[kDrawId];
    for (size_t i = 0; i < 4; ++i)
    {
        expectedInstances[0][i] = v[i] + w[i];
    }
    expectedVertices[0] = w;
    const uint32_t version = kAbiVersion + kAlbedo;
    std::memcpy(&expectedInstances[0][1], &version, sizeof(version));
    CHECK(kit.readBuffer(vertexBuffer) == bytesOf<Float4>(expectedVertices));
    CHECK(kit.readBuffer(instanceBuffer) == bytesOf<Float4>(expectedInstances));
}

//======================================================================================================================
TEST_CASE("AbiTextures texture1DMain samples a 1D texture", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    CHECK(sampledFetch(kit, "texture1DMain", TextureType::Tex1D, 1, TextureType::Tex1D, 1) == centerValue(0, false, false));
}

//======================================================================================================================
TEST_CASE("AbiTextures texture1DArrayMain samples layer 1 of a 1D array", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    CHECK(sampledFetch(kit, "texture1DArrayMain", TextureType::Tex1DArray, 3, TextureType::Tex1DArray, 3) == centerValue(1, false, false));
}

//======================================================================================================================
TEST_CASE("AbiTextures texture2DMain samples a 2D texture", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    CHECK(sampledFetch(kit, "texture2DMain", TextureType::Tex2D, 1, TextureType::Tex2D, 1) == centerValue(0, true, false));
}

//======================================================================================================================
TEST_CASE("AbiTextures texture2DMain samples a linear texture through its sRGB view and the reverse", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    // Every texel holds the code 128 in its color channels: 128/255 read as linear, and its sRGB decode (IEC 61966-2-1)
    // read through the partner view.
    const std::vector<std::byte> texels(kEdge * kEdge * 4, std::byte{ 128 });
    const float linear = 128.0f / 255.0f;
    const float decoded = std::pow((linear + 0.055f) / 1.055f, 2.4f);
    const auto sampledAs = [&](Format format, Format viewFormat) {
        const Texture texture = *kit.device.createTexture(
            TextureDesc{ .format = format, .extent = { kEdge, kEdge, 1 }, .usage = TextureUsage::Sampled, .label = "test.family" },
            std::array{ TextureData{ texels, kEdge * 4, 0 } }
        );
        const ViewIndex view = *kit.device.createView(texture, TextureViewDesc{ .format = viewFormat });
        return fetch(kit, "texture2DMain", FetchRoot{ .view = static_cast<uint32_t>(view), .sampler = linearClamp(kit.device) });
    };
    const Float4 toLinear = sampledAs(Format::RGBA8Unorm_sRGB, Format::RGBA8Unorm);
    const Float4 toSrgb = sampledAs(Format::RGBA8Unorm, Format::RGBA8Unorm_sRGB);
    for (size_t channel = 0; channel < 3; ++channel)
    {
        CHECK(std::abs(toLinear[channel] - linear) < 1e-6f);
        CHECK(std::abs(toSrgb[channel] - decoded) < 1e-4f);
    }
}

//======================================================================================================================
TEST_CASE("AbiTextures texture2DMain samples a BC1 texture through its sRGB view and the reverse", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    // One block whose four-color palette index is 0 everywhere: every texel is color0 = RGB565 (r 17, g 34, b 9).
    const std::vector<std::byte> block{ std::byte{ 0x49 }, std::byte{ 0x8C }, std::byte{ 0 }, std::byte{ 0 },
                                        std::byte{ 0 },    std::byte{ 0 },    std::byte{ 0 }, std::byte{ 0 } };
    const std::array<float, 3> linear{ static_cast<float>((17 << 3) | (17 >> 2)) / 255.0f,
                                       static_cast<float>((34 << 2) | (34 >> 4)) / 255.0f,
                                       static_cast<float>((9 << 3) | (9 >> 2)) / 255.0f };
    const auto sampledAs = [&](Format format, Format viewFormat) {
        const Texture texture = *kit.device.createTexture(
            TextureDesc{ .format = format, .extent = { kEdge, kEdge, 1 }, .usage = TextureUsage::Sampled, .label = "test.bc1" },
            std::array{ TextureData{ block, 8, 0 } }
        );
        const ViewIndex view = *kit.device.createView(texture, TextureViewDesc{ .format = viewFormat });
        return fetch(kit, "texture2DMain", FetchRoot{ .view = static_cast<uint32_t>(view), .sampler = linearClamp(kit.device) });
    };
    // The same bits through each view: the sRGB texture read as linear returns the code, the linear texture read as sRGB
    // returns its decode (IEC 61966-2-1).
    const Float4 toLinear = sampledAs(Format::BC1Unorm_sRGB, Format::BC1Unorm);
    const Float4 toSrgb = sampledAs(Format::BC1Unorm, Format::BC1Unorm_sRGB);
    for (size_t channel = 0; channel < 3; ++channel)
    {
        CHECK(std::abs(toLinear[channel] - linear[channel]) < 2e-3f);
        CHECK(std::abs(toSrgb[channel] - std::pow((linear[channel] + 0.055f) / 1.055f, 2.4f)) < 2e-3f);
    }
}

//======================================================================================================================
TEST_CASE("AbiTextures texture2DArrayMain samples layer 1 of a 2D array", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    CHECK(sampledFetch(kit, "texture2DArrayMain", TextureType::Tex2DArray, 3, TextureType::Tex2DArray, 3) == centerValue(1, true, false));
}

//======================================================================================================================
TEST_CASE("AbiTextures texture3DMain samples a 3D texture", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    CHECK(sampledFetch(kit, "texture3DMain", TextureType::Tex3D, 1, TextureType::Tex3D, 1) == centerValue(0, true, true));
}

//======================================================================================================================
TEST_CASE("AbiTextures textureCubeMain samples the +Z face of a cube", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    // The faces are +X, -X, +Y, -Y, +Z, -Z, so direction (0, 0, 1) lies in layer 4.
    CHECK(sampledFetch(kit, "textureCubeMain", TextureType::Cube, 6, TextureType::Cube, 6) == centerValue(4, true, false));
}

//======================================================================================================================
TEST_CASE("AbiTextures textureCubeArrayMain samples the +Z face of cube 1 of a cube array", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    CHECK(sampledFetch(kit, "textureCubeArrayMain", TextureType::CubeArray, 12, TextureType::CubeArray, 12) == centerValue(6 + 4, true, false));
}

//======================================================================================================================
TEST_CASE("AbiTextures compareMain returns the comparison of a depth texture against 0.5", "[conformance]")
{
    test::requireGpu();
    // The comparison is `LessOrEqual` on the reference: 0.5 passes against a stored 0.75 and fails against 0.25.
    const float depth = GENERATE(0.25f, 0.75f);
    CAPTURE(depth);
    test::GpuKit kit;
    const Texture texture = *kit.device.createTexture(
        TextureDesc{ .type = TextureType::Tex2D,
                     .format = Format::D32Float,
                     .extent = { kEdge, kEdge, 1 },
                     .usage = TextureUsage::Sampled | TextureUsage::DepthStencil,
                     .label = "test.depth" }
    );
    CommandList clear = kit.list(QueueType::Graphics, "test.clearDepth");
    test::transition(clear, texture, Layout::Undefined, Layout::CopyDst);
    clear.clearTexture(texture, {}, ClearValue{ .depth = depth });
    test::transition(clear, texture, Layout::CopyDst, Layout::DepthStencilRead);
    kit.run(std::array{ clear });
    const ViewIndex view = *kit.device.createView(texture, TextureViewDesc{ .aspect = TextureAspect::Depth });
    const SamplerIndex comparison = *kit.device.createSampler(
        SamplerDesc{ .filter = SamplerFilter::Nearest,
                     .mipFilter = SamplerFilter::Nearest,
                     .address = SamplerAddress::ClampToEdge,
                     .compareEnable = true,
                     .compare = CompareOp::LessOrEqual,
                     .label = "test.comparison" }
    );
    const Float4 value = fetch(kit, "compareMain", FetchRoot{ .view = static_cast<uint32_t>(view), .comparison = static_cast<uint32_t>(comparison) });
    CHECK(value == Float4{ depth > 0.5f ? 1.0f : 0.0f, 0, 0, 0 });
}

//======================================================================================================================
TEST_CASE("AbiTextures texture2DMSMain reads a cleared multisample texture", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const Texture texture = *kit.device.createTexture(
        TextureDesc{ .type = TextureType::Tex2D,
                     .format = Format::RGBA8Unorm,
                     .extent = { kEdge, kEdge, 1 },
                     .samples = 4,
                     .usage = TextureUsage::Sampled | TextureUsage::RenderTarget,
                     .label = "test.ms" }
    );
    clearMultisample(kit, texture, 0);
    const ViewIndex view = *kit.device.createView(texture, TextureViewDesc{});
    // Other targets load sample 0 of texel (1, 1), the cleared color. On Metal the kernel stores (width, height, samples, 0)
    // instead: the pinned Slang miscompiles the multisample `Load`.
    const Float4 expected = test::multisampleReadsSize() ? Float4{ 4, 4, 4, 0 } : Float4{ 1, 0, 1, 1 };
    CHECK(fetch(kit, "texture2DMSMain", FetchRoot{ .view = static_cast<uint32_t>(view) }) == expected);
}

//======================================================================================================================
TEST_CASE("AbiTextures texture2DMSArrayMain reads a cleared multisample array", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const Texture texture = *kit.device.createTexture(
        TextureDesc{ .type = TextureType::Tex2DArray,
                     .format = Format::RGBA8Unorm,
                     .extent = { kEdge, kEdge, 1 },
                     .layers = 3,
                     .samples = 4,
                     .usage = TextureUsage::Sampled | TextureUsage::RenderTarget,
                     .label = "test.msArray" }
    );
    clearMultisample(kit, texture, 1);
    const ViewIndex view = *kit.device.createView(texture, TextureViewDesc{ .type = TextureType::Tex2DArray });
    // Other targets load sample 0 of texel (1, 1) of layer 1, the cleared color. On Metal the kernel stores (width, height,
    // array size, samples) instead, for the same reason.
    const Float4 expected = test::multisampleReadsSize() ? Float4{ 4, 4, 3, 4 } : Float4{ 1, 0, 1, 1 };
    CHECK(fetch(kit, "texture2DMSArrayMain", FetchRoot{ .view = static_cast<uint32_t>(view) }) == expected);
}

//======================================================================================================================
TEST_CASE("AbiTextures rwTexture1DMain reads texel 1 of a 1D storage texture", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    CHECK(storageFetch(kit, "rwTexture1DMain", TextureType::Tex1D, 1, TextureType::Tex1D, 1) == texelValue(0, 1, 0, 0));
}

//======================================================================================================================
TEST_CASE("AbiTextures rwTexture1DArrayMain reads texel 1 of layer 1 of a 1D array", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    CHECK(storageFetch(kit, "rwTexture1DArrayMain", TextureType::Tex1DArray, 3, TextureType::Tex1DArray, 3) == texelValue(1, 1, 0, 0));
}

//======================================================================================================================
TEST_CASE("AbiTextures rwTexture2DMain reads texel (1, 1) of a 2D storage texture and writes it to (0, 0)", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    Texture texture;
    const Float4 value = storageFetch(kit, "rwTexture2DMain", TextureType::Tex2D, 1, TextureType::Tex2D, 1, &texture);
    CHECK(value == texelValue(0, 1, 1, 0));
    CommandList move = kit.list(QueueType::Graphics, "test.toCopy");
    test::transition(move, texture, Layout::ShaderWrite, Layout::CopySrc);
    kit.run(std::array{ move });
    // The kernel also wrote that value to texel (0, 0).
    const std::vector<std::byte> texels = kit.readTexture(texture, TextureRegion{ .extent = { kEdge, kEdge, 1 } }, Format::RGBA16Float);
    std::array<uint16_t, 4> first{};
    std::memcpy(first.data(), texels.data(), sizeof(first));
    const Float4 expected = texelValue(0, 1, 1, 0);
    CHECK(first == std::array<uint16_t, 4>{ halfOf(expected[0]), halfOf(expected[1]), halfOf(expected[2]), halfOf(expected[3]) });
}

//======================================================================================================================
TEST_CASE("AbiTextures rwTexture2DArrayMain reads texel (1, 1) of layer 1 of a 2D array", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    CHECK(storageFetch(kit, "rwTexture2DArrayMain", TextureType::Tex2DArray, 3, TextureType::Tex2DArray, 3) == texelValue(1, 1, 1, 0));
}

//======================================================================================================================
TEST_CASE("AbiTextures rwTexture3DMain reads texel (1, 1, 1) of a 3D storage texture", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    CHECK(storageFetch(kit, "rwTexture3DMain", TextureType::Tex3D, 1, TextureType::Tex3D, 1) == texelValue(0, 1, 1, 1));
}
