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
    constexpr uint32_t kAbiVersion = 3;

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
    /// The `RGBA16Float` texels of mip `mip` of an 8x8 texture, tight rows, texel (x, y) holding `texelValue(mip, x, y, 0)`.
    std::vector<std::byte> mipPattern(uint32_t mip)
    {
        const uint32_t edge = (2 * kEdge) >> mip;
        std::vector<uint16_t> texels;
        for (uint32_t y = 0; y < edge; ++y)
        {
            for (uint32_t x = 0; x < edge; ++x)
            {
                for (const float channel : texelValue(mip, x, y, 0))
                {
                    texels.push_back(halfOf(channel));
                }
            }
        }
        return bytesOf<uint16_t>(texels);
    }

    //==================================================================================================================
    /// Creates an 8x8 `RGBA16Float` texture of two mips with the usage `usage` besides `Sampled`, whose mip `m` holds
    /// `mipPattern(m)`.
    Texture mipPatternTexture(Device device, TextureUsage usage)
    {
        constexpr uint64_t kTexelBytes = 4 * sizeof(uint16_t);
        const std::vector<std::byte> base = mipPattern(0);
        const std::vector<std::byte> second = mipPattern(1);
        const std::array data{ TextureData{ base, 2 * kEdge * kTexelBytes, 0 }, TextureData{ second, kEdge * kTexelBytes, 0 } };
        return *device.createTexture(
            TextureDesc{ .type = TextureType::Tex2D,
                         .format = Format::RGBA16Float,
                         .extent = { 2 * kEdge, 2 * kEdge, 1 },
                         .mips = 2,
                         .usage = TextureUsage::Sampled | usage,
                         .label = "test.mips" },
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

    /// AbiNonUniform's root: each thread takes the even or the odd member of each pair by the parity of its index.
    struct NonUniformRoot
    {
        Addr evenBase;        ///< The buffer even threads load, store or add to.
        Addr oddBase;         ///< The buffer odd threads load, store or add to.
        Addr output;          ///< Where each thread stores what it read, at its own index.
        uint32_t evenView;    ///< The view even threads fetch.
        uint32_t oddView;     ///< The view odd threads fetch.
        uint32_t evenSampler; ///< The sampler even threads sample with.
        uint32_t oddSampler;  ///< The sampler odd threads sample with.
        uint32_t pad0 = 0;    ///< Padding.
        uint32_t pad1 = 0;    ///< Padding.
    };

    /// The threads of AbiNonUniform's one group: two waves of 32 or one of 64, each holding even and odd threads, so the
    /// threads of every wave index different descriptors.
    constexpr uint32_t kThreads = 64;

    /// Four `uint32` words, one `uint4`.
    using Uint4 = std::array<uint32_t, 4>;

    //==================================================================================================================
    /// Runs the AbiNonUniform kernel `entry` as one group of `kThreads` threads with the root `root`.
    void runNonUniform(test::GpuKit& kit, std::string_view entry, const NonUniformRoot& root)
    {
        const ShaderLibrary library = loadLibrary(kit.device, "AbiNonUniform");
        const Pipeline pipeline = computeOf(kit.device, library, entry);
        CommandList list = kit.list(QueueType::Graphics, "test.nonUniform");
        list.setPipeline(pipeline);
        list.pushRoot(root);
        list.dispatch(1, 1, 1);
        kit.run(std::array{ list });
    }

    //==================================================================================================================
    /// Returns `count` `uint4` cells from `seed`, distinct between cells and between seeds.
    std::vector<Uint4> cellsOf(uint32_t count, uint32_t seed)
    {
        std::vector<Uint4> cells(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            cells[i] = Uint4{ seed * 1000 + i, seed * 1000 + i + 100, seed, i };
        }
        return cells;
    }

    //==================================================================================================================
    /// Creates a 2x2 `RGBA16Float` sampled texture whose texels (0, 0), (1, 0), (0, 1) and (1, 1) hold `texels`, each channel
    /// of a texel the same.
    Texture quadTexture(Device device, const std::array<float, 4>& texels)
    {
        std::vector<uint16_t> halves;
        for (const float value : texels)
        {
            for (size_t channel = 0; channel < 4; ++channel)
            {
                halves.push_back(halfOf(value));
            }
        }
        const std::vector<std::byte> bytes = bytesOf<uint16_t>(halves);
        return *device.createTexture(
            TextureDesc{ .format = Format::RGBA16Float, .extent = { 2, 2, 1 }, .usage = TextureUsage::Sampled, .label = "test.quad" },
            std::array{ TextureData{ bytes, 2 * 4 * sizeof(uint16_t), 0 } }
        );
    }

    //==================================================================================================================
    /// Returns the `float4` cells of `bytes`.
    std::vector<Float4> float4sOf(std::span<const std::byte> bytes)
    {
        std::vector<Float4> values(bytes.size() / sizeof(Float4));
        std::memcpy(values.data(), bytes.data(), values.size() * sizeof(Float4));
        return values;
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
TEST_CASE("AbiTextures reads a Storage-only texture under declared-access validation", "[conformance]")
{
    test::requireGpu();
#ifndef NDEBUG
    constexpr bool kValidation = true;
#else
    constexpr bool kValidation = false;
#endif
    test::GpuKit kit(2, false, kValidation);
    constexpr SubresourceRange kRange{ .baseMip = 0, .mipCount = 1, .baseLayer = 0, .layerCount = 2 };
    const Texture texture = *kit.device.createTexture(
        TextureDesc{ .type = TextureType::Tex2DArray,
                     .format = Format::RGBA16Float,
                     .extent = { 2, 2, 1 },
                     .layers = 2,
                     .usage = TextureUsage::Storage,
                     .label = "test.storageRead" }
    );
    const ViewIndex view = *kit.device.createView(texture, TextureViewDesc{ .type = TextureType::Tex2DArray, .range = kRange, .usage = TextureUsage::Storage });
    const ShaderLibrary library = loadLibrary(kit.device, "AbiTextures");
    const Pipeline pipeline = computeOf(kit.device, library, "rwTexture2DArrayMain");
    const std::array<uint32_t, 4> untouched{ 0xDEADBEEFu, 0xDEADBEEFu, 0xDEADBEEFu, 0xDEADBEEFu };
    const Buffer output = kit.buffer(sizeof(untouched), Memory::Gpu, BufferUsage::ShaderWrite, bytesOf<uint32_t>(untouched));
    CommandList list = kit.list(QueueType::Graphics, "test.declaredStorageRead");
    test::transition(list, texture, Layout::Undefined, Layout::CopyDst, kRange);
    list.clearTexture(texture, kRange, ClearValue{ .color = { 7, 3, 1, 1 } });
    const std::array textureBarriers{ TextureBarrier{ .texture = texture,
                                                      .range = kRange,
                                                      .srcStage = Stage::Copy,
                                                      .srcAccess = Access::CopyWrite,
                                                      .dstStage = Stage::ComputeShader,
                                                      .dstAccess = Access::ShaderRead,
                                                      .oldLayout = Layout::CopyDst,
                                                      .newLayout = Layout::ShaderWrite } };
    list.barrier({}, {}, textureBarriers);
    list.setPipeline(pipeline);
    list.pushRoot(FetchRoot{ .view = static_cast<uint32_t>(view), .output = kit.device.address(output) });
    const std::array accesses{ ResourceAccess{ .texture = texture, .range = kRange, .stage = Stage::ComputeShader, .access = Access::ShaderRead },
                               ResourceAccess{
                                   .buffer = output, .offset = 0, .size = sizeof(untouched), .stage = Stage::ComputeShader, .access = Access::ShaderWrite } };
    list.declareAccess(accesses);
    list.dispatch(1, 1, 1);
    const std::array bufferBarriers{ BufferBarrier{ .buffer = output,
                                                    .size = sizeof(untouched),
                                                    .srcStage = Stage::ComputeShader,
                                                    .srcAccess = Access::ShaderWrite,
                                                    .dstStage = Stage::Copy,
                                                    .dstAccess = Access::CopyRead } };
    list.barrier({}, bufferBarriers, {});
    kit.run(std::array{ list });
    CHECK(kit.readBuffer(output) == bytesOf<float>(Float4{ 7, 3, 1, 1 }));
}

//======================================================================================================================
TEST_CASE("AbiTextures rwTexture3DMain reads texel (1, 1, 1) of a 3D storage texture", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    CHECK(storageFetch(kit, "rwTexture3DMain", TextureType::Tex3D, 1, TextureType::Tex3D, 1) == texelValue(0, 1, 1, 1));
}

//======================================================================================================================
TEST_CASE("AbiTextures texture2DMain samples a view of mip 1 alone", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const Texture texture = mipPatternTexture(kit.device, TextureUsage::None);
    const ViewIndex view = *kit.device.createView(
        texture,
        TextureViewDesc{ .type = TextureType::Tex2D, .range = { .baseMip = 1, .mipCount = 1, .baseLayer = 0, .layerCount = 1 }, .usage = TextureUsage::Sampled }
    );
    CHECK(fetch(kit, "texture2DMain", FetchRoot{ .view = static_cast<uint32_t>(view), .sampler = linearClamp(kit.device) }) == centerValue(1, true, false));
}

//======================================================================================================================
TEST_CASE("AbiTextures rwTexture2DMain reads and writes mip 1 through a storage view of that mip alone", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const Texture texture = mipPatternTexture(kit.device, TextureUsage::Storage);
    constexpr SubresourceRange kMip1{ .baseMip = 1, .mipCount = 1 };
    constexpr SubresourceRange kMip0{ .baseMip = 0, .mipCount = 1 };
    // Mip 0 stays in ShaderRead; only mip 1 moves.
    CommandList toWrite = kit.list(QueueType::Graphics, "test.mip1.toWrite");
    test::transition(toWrite, texture, Layout::ShaderRead, Layout::ShaderWrite, kMip1);
    kit.run(std::array{ toWrite });
    const ViewIndex view = *kit.device.createView(
        texture,
        TextureViewDesc{ .type = TextureType::Tex2D, .range = { .baseMip = 1, .mipCount = 1, .baseLayer = 0, .layerCount = 1 }, .usage = TextureUsage::Storage }
    );
    CHECK(fetch(kit, "rwTexture2DMain", FetchRoot{ .view = static_cast<uint32_t>(view) }) == texelValue(1, 1, 1, 0));
    CommandList toCopy = kit.list(QueueType::Graphics, "test.mip1.toCopy");
    test::transition(toCopy, texture, Layout::ShaderWrite, Layout::CopySrc, kMip1);
    test::transition(toCopy, texture, Layout::ShaderRead, Layout::CopySrc, kMip0);
    kit.run(std::array{ toCopy });
    // The kernel wrote the value of texel (1, 1) of mip 1 to its texel (0, 0).
    std::vector<std::byte> expectedMip1 = mipPattern(1);
    const Float4 written = texelValue(1, 1, 1, 0);
    const std::array<uint16_t, 4> writtenBits{ halfOf(written[0]), halfOf(written[1]), halfOf(written[2]), halfOf(written[3]) };
    std::memcpy(expectedMip1.data(), writtenBits.data(), sizeof(writtenBits));
    CHECK(kit.readTexture(texture, TextureRegion{ .mip = 1, .extent = { kEdge, kEdge, 1 } }, Format::RGBA16Float) == expectedMip1);
    // Mip 0 is untouched.
    CHECK(kit.readTexture(texture, TextureRegion{ .mip = 0, .extent = { 2 * kEdge, 2 * kEdge, 1 } }, Format::RGBA16Float) == mipPattern(0));
}

//======================================================================================================================
TEST_CASE("AbiNonUniform loadMain loads through a buffer that differs between the threads of a wave", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const std::vector<Uint4> even = cellsOf(kThreads, 1);
    const std::vector<Uint4> odd = cellsOf(kThreads, 2);
    const std::vector<Uint4> untouched(kThreads, Uint4{ 0xDEADBEEFu, 0xDEADBEEFu, 0xDEADBEEFu, 0xDEADBEEFu });
    const Buffer evenBuffer = kit.buffer(kThreads * sizeof(Uint4), Memory::Gpu, BufferUsage::ShaderWrite, bytesOf<Uint4>(even));
    const Buffer oddBuffer = kit.buffer(kThreads * sizeof(Uint4), Memory::Gpu, BufferUsage::ShaderWrite, bytesOf<Uint4>(odd));
    const Buffer output = kit.buffer(kThreads * sizeof(Uint4), Memory::Gpu, BufferUsage::ShaderWrite, bytesOf<Uint4>(untouched));
    runNonUniform(
        kit,
        "loadMain",
        NonUniformRoot{ .evenBase = kit.device.address(evenBuffer), .oddBase = kit.device.address(oddBuffer), .output = kit.device.address(output) }
    );
    // Each thread loads its own cell of the buffer its parity names.
    std::vector<Uint4> expected(kThreads);
    for (uint32_t thread = 0; thread < kThreads; ++thread)
    {
        expected[thread] = (thread & 1) != 0 ? odd[thread] : even[thread];
    }
    CHECK(kit.readBuffer(output) == bytesOf<Uint4>(expected));
}

//======================================================================================================================
TEST_CASE("AbiNonUniform storeMain stores through a buffer that differs between the threads of a wave", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const Uint4 kUntouched{ 0xDEADBEEFu, 0xDEADBEEFu, 0xDEADBEEFu, 0xDEADBEEFu };
    const std::vector<Uint4> untouched(kThreads, kUntouched);
    const Buffer evenBuffer = kit.buffer(kThreads * sizeof(Uint4), Memory::Gpu, BufferUsage::ShaderWrite, bytesOf<Uint4>(untouched));
    const Buffer oddBuffer = kit.buffer(kThreads * sizeof(Uint4), Memory::Gpu, BufferUsage::ShaderWrite, bytesOf<Uint4>(untouched));
    runNonUniform(
        kit,
        "storeMain",
        NonUniformRoot{ .evenBase = kit.device.address(evenBuffer), .oddBase = kit.device.address(oddBuffer), .output = kit.device.address(evenBuffer) }
    );
    // Each thread stores (index, 0, 0, 1) into its own cell of the buffer its parity names, and only there.
    std::vector<Uint4> expectedEven(kThreads, kUntouched);
    std::vector<Uint4> expectedOdd(kThreads, kUntouched);
    for (uint32_t thread = 0; thread < kThreads; ++thread)
    {
        ((thread & 1) != 0 ? expectedOdd : expectedEven)[thread] = Uint4{ thread, 0, 0, 1 };
    }
    CHECK(kit.readBuffer(evenBuffer) == bytesOf<Uint4>(expectedEven));
    CHECK(kit.readBuffer(oddBuffer) == bytesOf<Uint4>(expectedOdd));
}

//======================================================================================================================
TEST_CASE("AbiNonUniform atomicMain adds to a buffer that differs between the threads of a wave", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const std::vector<uint32_t> zeros(16, 0);
    const Buffer evenBuffer = kit.buffer(zeros.size() * sizeof(uint32_t), Memory::Gpu, BufferUsage::ShaderWrite, bytesOf<uint32_t>(zeros));
    const Buffer oddBuffer = kit.buffer(zeros.size() * sizeof(uint32_t), Memory::Gpu, BufferUsage::ShaderWrite, bytesOf<uint32_t>(zeros));
    const std::vector<uint32_t> untouched(kThreads, 0xDEADBEEFu);
    const Buffer output = kit.buffer(kThreads * sizeof(uint32_t), Memory::Gpu, BufferUsage::ShaderWrite, bytesOf<uint32_t>(untouched));
    runNonUniform(
        kit,
        "atomicMain",
        NonUniformRoot{ .evenBase = kit.device.address(evenBuffer), .oddBase = kit.device.address(oddBuffer), .output = kit.device.address(output) }
    );
    // Each thread adds 1 to word 3 of the buffer its parity names, so each buffer's word ends at the count of its threads,
    // and the old words its threads saw are 0 to that count less one, in some order.
    std::vector<uint32_t> expectedWords(zeros.size(), 0);
    expectedWords[3] = kThreads / 2;
    CHECK(kit.readBuffer(evenBuffer) == bytesOf<uint32_t>(expectedWords));
    CHECK(kit.readBuffer(oddBuffer) == bytesOf<uint32_t>(expectedWords));
    const std::vector<std::byte> bytes = kit.readBuffer(output);
    std::vector<uint32_t> olds(kThreads);
    std::memcpy(olds.data(), bytes.data(), olds.size() * sizeof(uint32_t));
    std::vector<bool> evenSeen(kThreads / 2, false);
    std::vector<bool> oddSeen(kThreads / 2, false);
    uint32_t wrong = 0;
    for (uint32_t thread = 0; thread < kThreads; ++thread)
    {
        std::vector<bool>& seen = (thread & 1) != 0 ? oddSeen : evenSeen;
        if (olds[thread] >= seen.size() || seen[olds[thread]])
        {
            UNSCOPED_INFO("thread " << thread << " saw the old word " << olds[thread]);
            ++wrong;
            continue;
        }
        seen[olds[thread]] = true;
    }
    CHECK(wrong == 0u);
}

//======================================================================================================================
TEST_CASE("AbiNonUniform textureMain loads through a view that differs between the threads of a wave", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const ViewIndex evenView = *kit.device.createView(quadTexture(kit.device, { 0.0f, 0.0f, 0.0f, 4.0f }), TextureViewDesc{});
    const ViewIndex oddView = *kit.device.createView(quadTexture(kit.device, { 7.0f, 7.0f, 7.0f, 7.0f }), TextureViewDesc{});
    const std::vector<Float4> untouched(kThreads, Float4{ -1.0f, -1.0f, -1.0f, -1.0f });
    const Buffer output = kit.buffer(kThreads * sizeof(Float4), Memory::Gpu, BufferUsage::ShaderWrite, bytesOf<Float4>(untouched));
    runNonUniform(
        kit,
        "textureMain",
        NonUniformRoot{ .output = kit.device.address(output), .evenView = static_cast<uint32_t>(evenView), .oddView = static_cast<uint32_t>(oddView) }
    );
    // Each thread loads texel (0, 0) of the view its parity names: 0 from the even texture, 7 from the odd one.
    std::vector<Float4> expected(kThreads);
    for (uint32_t thread = 0; thread < kThreads; ++thread)
    {
        expected[thread] = (thread & 1) != 0 ? Float4{ 7, 7, 7, 7 } : Float4{ 0, 0, 0, 0 };
    }
    CHECK(float4sOf(kit.readBuffer(output)) == expected);
}

//======================================================================================================================
TEST_CASE("AbiNonUniform wTextureMain stores through a view that differs between the threads of a wave", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const auto storage = [&] {
        return *kit.device.createTexture(
            TextureDesc{ .format = Format::RGBA16Float, .extent = { kThreads, 1, 1 }, .usage = TextureUsage::Storage, .label = "test.nonUniformStorage" }
        );
    };
    const std::array<Texture, 2> textures{ storage(), storage() };
    CommandList clear = kit.list(QueueType::Graphics, "test.clearStorage");
    for (const Texture texture : textures)
    {
        test::transition(clear, texture, Layout::Undefined, Layout::CopyDst);
        clear.clearTexture(texture, {}, ClearValue{ .color = { 0.0f, 0.0f, 0.0f, 0.0f } });
        test::transition(clear, texture, Layout::CopyDst, Layout::ShaderWrite);
    }
    kit.run(std::array{ clear });
    const ViewIndex evenView = *kit.device.createView(textures[0], TextureViewDesc{ .usage = TextureUsage::Storage });
    const ViewIndex oddView = *kit.device.createView(textures[1], TextureViewDesc{ .usage = TextureUsage::Storage });
    runNonUniform(kit, "wTextureMain", NonUniformRoot{ .evenView = static_cast<uint32_t>(evenView), .oddView = static_cast<uint32_t>(oddView) });
    CommandList toCopy = kit.list(QueueType::Graphics, "test.toCopy");
    for (const Texture texture : textures)
    {
        test::transition(toCopy, texture, Layout::ShaderWrite, Layout::CopySrc);
    }
    kit.run(std::array{ toCopy });
    // Each thread stores (1, 0, 0, 1) at its own texel of the view its parity names, and only there.
    const std::array<uint16_t, 4> written{ halfOf(1.0f), 0, 0, halfOf(1.0f) };
    for (uint32_t parity = 0; parity < 2; ++parity)
    {
        CAPTURE(parity);
        std::vector<uint16_t> expected(size_t{ kThreads } * 4, 0);
        for (uint32_t x = parity; x < kThreads; x += 2)
        {
            std::memcpy(&expected[size_t{ x } * 4], written.data(), sizeof(written));
        }
        CHECK(kit.readTexture(textures[parity], TextureRegion{ .extent = { kThreads, 1, 1 } }, Format::RGBA16Float) == bytesOf<uint16_t>(expected));
    }
}

//======================================================================================================================
TEST_CASE("AbiNonUniform samplerMain samples through a sampler that differs between the threads of a wave", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    // At the centre of a 2x2 texture whose texels hold 0, 0, 0 and 4, the linear sampler returns their mean, 1, and the
    // nearest sampler one of the texels, never 1.
    const ViewIndex view = *kit.device.createView(quadTexture(kit.device, { 0.0f, 0.0f, 0.0f, 4.0f }), TextureViewDesc{});
    const SamplerIndex linear = *kit.device.createSampler(
        SamplerDesc{ .filter = SamplerFilter::Linear, .mipFilter = SamplerFilter::Nearest, .address = SamplerAddress::ClampToEdge, .label = "test.linear" }
    );
    const SamplerIndex nearest = *kit.device.createSampler(
        SamplerDesc{ .filter = SamplerFilter::Nearest, .mipFilter = SamplerFilter::Nearest, .address = SamplerAddress::ClampToEdge, .label = "test.nearest" }
    );
    const std::vector<Float4> untouched(kThreads, Float4{ -1.0f, -1.0f, -1.0f, -1.0f });
    const Buffer output = kit.buffer(kThreads * sizeof(Float4), Memory::Gpu, BufferUsage::ShaderWrite, bytesOf<Float4>(untouched));
    runNonUniform(
        kit,
        "samplerMain",
        NonUniformRoot{ .output = kit.device.address(output),
                        .evenView = static_cast<uint32_t>(view),
                        .evenSampler = static_cast<uint32_t>(linear),
                        .oddSampler = static_cast<uint32_t>(nearest) }
    );
    const std::vector<Float4> values = float4sOf(kit.readBuffer(output));
    REQUIRE(values.size() == kThreads);
    const Float4 nearestValue = values[1];
    CHECK((nearestValue == Float4{ 0, 0, 0, 0 } || nearestValue == Float4{ 4, 4, 4, 4 }));
    for (uint32_t thread = 0; thread < kThreads; ++thread)
    {
        CAPTURE(thread);
        CHECK(values[thread] == ((thread & 1) != 0 ? nearestValue : Float4{ 1, 1, 1, 1 }));
    }
}
