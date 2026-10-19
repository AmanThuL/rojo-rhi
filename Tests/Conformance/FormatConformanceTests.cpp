//----------------------------------------------------------------------------------------------------------------------
/// @file FormatConformanceTests.cpp
/// @brief Exercises every format through each capability the device's format table reports for it, on a GPU.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Support/Conformance.h"
#include "Support/GpuKit.h"
#include "Support/PassKit.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace rojoRHI;

// Each case runs once per format, on a 4 by 4 texture (one BC1 block) whose left two columns hold one value and whose right
// two hold another. Values are exact: unorm channels are multiples of 1/3, which 8-bit, 10-bit and 2-bit channels all
// hold; float channels are small binary fractions. Every value a format stores, a clear, a draw, a blend sum, a resolve,
// a shader store or the re-encoding of an sRGB value, is exactly a whole code, so any conversion within ±0.6 ULP of the
// exact result (D3D's tolerance for float to unorm and to sRGB) gives that code; stored texels are compared byte for byte.
// Values a shader reads are compared exactly for float and depth channels. A normalized channel must lie within half a
// step of the stored code (0.5/255 for 8 bits), which identifies it; an sRGB color channel is re-encoded first and
// compared the same way. A linearly filtered sRGB color channel, the mean of two decoded codes, is compared in linear
// space, within the smaller of half an 8-bit step and half its distance to the decoded codes beside the one it rounds to.
namespace {
    /// Four floats, one `float4`.
    using Float4 = std::array<float, 4>;

    /// The edge of every texture, in texels.
    constexpr uint32_t kEdge = 4;

    /// The texels of one texture.
    constexpr uint32_t kTexels = kEdge * kEdge;

    /// The samples `readSamplesMain` stores per texel, the most a texture may have.
    constexpr uint32_t kMaxSamples = 8;

    /// Every format the interface names.
    constexpr std::array kFormats{
        Format::BGRA8Unorm,    Format::RGBA8Unorm, Format::RGBA8Unorm_sRGB, Format::RGBA16Float, Format::RG16Float,  Format::R8Unorm,      Format::BC1Unorm,
        Format::BC1Unorm_sRGB, Format::D32Float,   Format::R16Float,        Format::R32Float,    Format::D32FloatS8, Format::RGB10A2Unorm,
    };

    /// FormatOracle's root for the kernels that read.
    struct ReadRoot
    {
        uint32_t view;             ///< The view read.
        uint32_t sampler;          ///< The sampler of the sampling entries.
        uint32_t samples;          ///< The samples `readSamplesMain` reads per texel.
        uint32_t pad;              ///< Padding.
        Addr output;               ///< Where the kernel stores its `float4` values.
        std::array<float, 2> at{}; ///< The coordinate `sampleAtMain` samples.
    };

    /// FormatOracle's root for the kernels that store.
    struct WriteRoot
    {
        uint32_t view; ///< The view written.
        uint32_t pad0; ///< Padding.
        uint32_t pad1; ///< Padding.
        uint32_t pad2; ///< Padding.
        Float4 left;   ///< The value of the left two columns.
        Float4 right;  ///< The value of the right two columns.
    };

    /// How a format holds its channels.
    enum class Encoding
    {
        Unorm, ///< Normalized integers.
        Srgb,  ///< Normalized integers, color channels with sRGB transfer.
        Float, ///< Half- or single-precision floats.
        Depth, ///< A single-precision depth.
    };

    /// What the cases need to know of one format.
    struct Traits
    {
        std::string_view name;             ///< The enumerator's name.
        Encoding encoding;                 ///< How the channels are held.
        uint32_t channels;                 ///< The channels a shader read compares.
        std::array<uint32_t, 4> maxCode{}; ///< Per normalized channel, its largest code.
        bool block = false;                ///< BC1: stored as one 4 by 4 block.
    };

    //==================================================================================================================
    /// Returns the traits of `format`.
    Traits traitsOf(Format format)
    {
        constexpr std::array<uint32_t, 4> kEightBits{ 255, 255, 255, 255 };
        switch (format)
        {
        case Format::BGRA8Unorm: return { "BGRA8Unorm", Encoding::Unorm, 4, kEightBits };
        case Format::RGBA8Unorm: return { "RGBA8Unorm", Encoding::Unorm, 4, kEightBits };
        case Format::RGBA8Unorm_sRGB: return { "RGBA8Unorm_sRGB", Encoding::Srgb, 4, kEightBits };
        case Format::RGBA16Float: return { "RGBA16Float", Encoding::Float, 4 };
        case Format::RG16Float: return { "RG16Float", Encoding::Float, 2 };
        case Format::R8Unorm: return { "R8Unorm", Encoding::Unorm, 1, kEightBits };
        case Format::BC1Unorm: return { "BC1Unorm", Encoding::Unorm, 4, kEightBits, true };
        case Format::BC1Unorm_sRGB: return { "BC1Unorm_sRGB", Encoding::Srgb, 4, kEightBits, true };
        case Format::D32Float: return { "D32Float", Encoding::Depth, 1 };
        case Format::R16Float: return { "R16Float", Encoding::Float, 1 };
        case Format::R32Float: return { "R32Float", Encoding::Float, 1 };
        case Format::D32FloatS8: return { "D32FloatS8", Encoding::Depth, 1 };
        case Format::RGB10A2Unorm: return { "RGB10A2Unorm", Encoding::Unorm, 4, { 1023, 1023, 1023, 3 } };
        case Format::Undefined: break;
        }
        FAIL_CHECK("no traits for Format::Undefined");
        return {};
    }

    //==================================================================================================================
    /// True for the depth formats, which a pass fills.
    bool isDepth(Format format)
    {
        return traitsOf(format).encoding == Encoding::Depth;
    }

    //==================================================================================================================
    /// The value the left two columns of a texture of `format` hold, as stored: an sRGB color channel before decoding. Its
    /// first channel is not zero, so a blend that ignored the destination would show even in a one-channel format.
    Float4 leftOf(Format format)
    {
        switch (traitsOf(format).encoding)
        {
        case Encoding::Unorm:
        case Encoding::Srgb: return traitsOf(format).block ? Float4{ 0.0f, 0.0f, 0.0f, 1.0f } : Float4{ 1.0f / 3.0f, 0.0f, 2.0f / 3.0f, 1.0f / 3.0f };
        case Encoding::Float: return { 0.25f, -1.5f, 3.0f, 0.5f };
        case Encoding::Depth: return { 0.25f, 0.0f, 0.0f, 0.0f };
        }
        return {};
    }

    //==================================================================================================================
    /// The value the right two columns of a texture of `format` hold, as stored. Each channel is at least the left one's, and
    /// the two codes of a normalized channel are both even or both odd, so their mean is a code too.
    Float4 rightOf(Format format)
    {
        switch (traitsOf(format).encoding)
        {
        case Encoding::Unorm:
        case Encoding::Srgb: return traitsOf(format).block ? Float4{ 0.0f, 2.0f / 3.0f, 0.0f, 1.0f } : Float4{ 1.0f, 2.0f / 3.0f, 2.0f / 3.0f, 1.0f };
        case Encoding::Float: return { 0.75f, 2.5f, 3.0f, 1.5f };
        case Encoding::Depth: return { 0.75f, 0.0f, 0.0f, 0.0f };
        }
        return {};
    }

    //==================================================================================================================
    /// The linear value of the sRGB-encoded `value` (IEC 61966-2-1).
    float decodeSrgb(float value)
    {
        const double encoded = value;
        return static_cast<float>(encoded <= 0.04045 ? encoded / 12.92 : std::pow((encoded + 0.055) / 1.055, 2.4));
    }

    //==================================================================================================================
    /// The value a shader reads from, or writes to store, `stored` in `format`: sRGB color channels are decoded.
    Float4 visibleOf(Format format, Float4 stored)
    {
        if (traitsOf(format).encoding == Encoding::Srgb)
        {
            for (size_t channel = 0; channel < 3; ++channel)
            {
                stored[channel] = decodeSrgb(stored[channel]);
            }
        }
        return stored;
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
    /// Appends the bytes of `value` to `bytes`.
    template<class T> void append(std::vector<std::byte>& bytes, T value)
    {
        const auto* first = reinterpret_cast<const std::byte*>(&value);
        bytes.insert(bytes.end(), first, first + sizeof(value));
    }

    //==================================================================================================================
    /// The code of the stored normalized value `stored` in a channel whose largest code is `maxCode`.
    uint32_t codeOf(float stored, uint32_t maxCode)
    {
        return static_cast<uint32_t>(std::lround(static_cast<double>(stored) * maxCode));
    }

    //==================================================================================================================
    /// Appends the texel `format` packs for the stored value `stored` to `bytes`. Not for BC1, which packs blocks.
    void appendTexel(std::vector<std::byte>& bytes, Format format, const Float4& stored)
    {
        const auto byteOf = [&](size_t channel) { return static_cast<uint8_t>(codeOf(stored[channel], 255)); };
        switch (format)
        {
        case Format::RGBA8Unorm:
        case Format::RGBA8Unorm_sRGB: append(bytes, std::array{ byteOf(0), byteOf(1), byteOf(2), byteOf(3) }); break;
        case Format::BGRA8Unorm: append(bytes, std::array{ byteOf(2), byteOf(1), byteOf(0), byteOf(3) }); break;
        case Format::R8Unorm: append(bytes, byteOf(0)); break;
        case Format::RGB10A2Unorm:
            append(bytes, codeOf(stored[0], 1023) | (codeOf(stored[1], 1023) << 10) | (codeOf(stored[2], 1023) << 20) | (codeOf(stored[3], 3) << 30));
            break;
        case Format::RGBA16Float: append(bytes, std::array{ halfOf(stored[0]), halfOf(stored[1]), halfOf(stored[2]), halfOf(stored[3]) }); break;
        case Format::RG16Float: append(bytes, std::array{ halfOf(stored[0]), halfOf(stored[1]) }); break;
        case Format::R16Float: append(bytes, halfOf(stored[0])); break;
        case Format::R32Float:
        case Format::D32Float: append(bytes, stored[0]); break;
        default: FAIL("the format packs no texel");
        }
    }

    //==================================================================================================================
    /// The tight bytes of a texture of `format` whose left two columns hold its left value and right two its right value. For BC1
    /// it is one block: color0 is (0, 42 of 63, 0), which expands to 170 of 255, color1 is black, and color0 > color1 picks
    /// the four-color mode, so index 0 is color0 and index 1 is color1; the left columns take index 1, the right index 0.
    std::vector<std::byte> imageOf(Format format)
    {
        if (traitsOf(format).block)
        {
            // Little-endian color0 0x0540 and color1 0x0000, then a row of indices per byte, texel x at bits 2x: 01 01 00 00.
            constexpr std::array<uint8_t, 8> kBlock{ 0x40, 0x05, 0x00, 0x00, 0x05, 0x05, 0x05, 0x05 };
            std::vector<std::byte> bytes;
            append(bytes, kBlock);
            return bytes;
        }
        std::vector<std::byte> bytes;
        for (uint32_t texel = 0; texel < kTexels; ++texel)
        {
            appendTexel(bytes, format, texel % kEdge < kEdge / 2 ? leftOf(format) : rightOf(format));
        }
        return bytes;
    }

    //==================================================================================================================
    /// The bytes of one row of a texture of `format`, or of one row of blocks.
    uint64_t rowBytesOf(Format format)
    {
        return imageOf(format).size() / (traitsOf(format).block ? 1 : kEdge);
    }

    //==================================================================================================================
    /// The mean of `a` and `b`, channel by channel.
    Float4 meanOf(const Float4& a, const Float4& b)
    {
        Float4 mean{};
        for (size_t channel = 0; channel < 4; ++channel)
        {
            mean[channel] = (a[channel] + b[channel]) / 2.0f;
        }
        return mean;
    }

    //==================================================================================================================
    /// The sRGB encoding of the linear `value` (IEC 61966-2-1).
    double encodeSrgb(double value)
    {
        return value <= 0.0031308 ? value * 12.92 : 1.055 * std::pow(value, 1.0 / 2.4) - 0.055;
    }

    //==================================================================================================================
    /// Describes `actual` against `expected` when they disagree; empty otherwise.
    std::string describe(const Float4& actual, const Float4& expected, bool agrees)
    {
        if (agrees)
        {
            return {};
        }
        return std::format(
            "({}, {}, {}, {}), expected ({}, {}, {}, {})", actual[0], actual[1], actual[2], actual[3], expected[0], expected[1], expected[2], expected[3]
        );
    }

    //==================================================================================================================
    /// True when channel `channel` of a format with `traits` is an sRGB color channel.
    bool srgbColor(const Traits& traits, size_t channel)
    {
        return traits.encoding == Encoding::Srgb && channel < 3;
    }

    //==================================================================================================================
    /// Describes where `actual`, a value a shader read from `format`, differs from the texel value `stored` holds; empty when
    /// they agree. Float and depth channels agree when equal; a normalized channel, re-encoded first when it is an sRGB color
    /// channel, when it lies within half a step of the stored code.
    std::string mismatch(Format format, const Float4& actual, const Float4& stored)
    {
        const Traits traits = traitsOf(format);
        bool agrees = true;
        for (size_t channel = 0; channel < traits.channels; ++channel)
        {
            if (traits.encoding != Encoding::Unorm && traits.encoding != Encoding::Srgb)
            {
                agrees = agrees && actual[channel] == stored[channel];
                continue;
            }
            const double read = srgbColor(traits, channel) ? encodeSrgb(actual[channel]) : actual[channel];
            agrees = agrees && std::fabs(read - stored[channel]) <= 0.5 / traits.maxCode[channel];
        }
        return describe(actual, visibleOf(format, stored), agrees);
    }

    //==================================================================================================================
    /// The bound a linearly filtered sRGB color channel whose exact value is `mean` is compared within: half an 8-bit step, or
    /// half the distance from `mean` to the nearest decoded code other than the one it rounds to, when that is smaller.
    double srgbMeanBound(double mean)
    {
        const double code = std::round(encodeSrgb(mean) * 255.0);
        double bound = 0.5 / 255.0;
        for (const double neighbour : { code - 1.0, code + 1.0 })
        {
            if (neighbour >= 0.0 && neighbour <= 255.0)
            {
                bound = std::min(bound, std::fabs(decodeSrgb(static_cast<float>(neighbour / 255.0)) - mean) / 2.0);
            }
        }
        return bound;
    }

    //==================================================================================================================
    /// Describes where `actual`, a linear sample of `format` halfway between texels holding `left` and `right` as stored,
    /// differs from the mean of the two values a shader reads; empty when they agree. A normalized channel's mean is a code
    /// and agrees within half a step; an sRGB color channel's mean is compared in linear space within `srgbMeanBound`.
    std::string mismatchMean(Format format, const Float4& actual, const Float4& left, const Float4& right)
    {
        const Traits traits = traitsOf(format);
        const Float4 mean = meanOf(visibleOf(format, left), visibleOf(format, right));
        bool agrees = true;
        for (size_t channel = 0; channel < traits.channels; ++channel)
        {
            if (srgbColor(traits, channel))
            {
                const double exact = (static_cast<double>(decodeSrgb(left[channel])) + decodeSrgb(right[channel])) / 2.0;
                agrees = agrees && std::fabs(actual[channel] - exact) <= srgbMeanBound(exact);
            }
            else if (traits.encoding == Encoding::Unorm || traits.encoding == Encoding::Srgb)
            {
                agrees = agrees && std::fabs(static_cast<double>(actual[channel]) - mean[channel]) <= 0.5 / traits.maxCode[channel];
            }
            else
            {
                agrees = agrees && actual[channel] == mean[channel];
            }
        }
        return describe(actual, mean, agrees);
    }

    //==================================================================================================================
    /// `a` minus `b`, channel by channel.
    Float4 difference(const Float4& a, const Float4& b)
    {
        Float4 result{};
        for (size_t channel = 0; channel < 4; ++channel)
        {
            result[channel] = a[channel] - b[channel];
        }
        return result;
    }

    //==================================================================================================================
    /// The bytes of `values`.
    template<class T> std::vector<std::byte> bytesOf(std::span<const T> values)
    {
        std::vector<std::byte> bytes(values.size_bytes());
        std::memcpy(bytes.data(), values.data(), bytes.size());
        return bytes;
    }

    /// The two oracle libraries on one kit's device, with the pipelines, samplers and kernel runs a case makes from them.
    struct Oracle
    {
        //==============================================================================================================
        /// Loads the format and draw oracle libraries on `kit`'s device.
        explicit Oracle(test::GpuKit& gpu)
            : kit(gpu), library(*gpu.device.loadShaderLibrary(test::shaderLibraryFile("FormatOracle"), "FormatOracle")),
              drawLibrary(*gpu.device.loadShaderLibrary(test::shaderLibraryFile("DrawOracle"), "DrawOracle"))
        {
        }

        //==============================================================================================================
        /// The compute pipeline of FormatOracle's `entry`.
        Pipeline compute(std::string_view entry) const
        {
            return *kit.device.createPipeline(ComputePipelineDesc{ .compute = { library, entry }, .label = "test.formatKernel" });
        }

        //==============================================================================================================
        /// A pipeline that covers the target with the root's color into one `format` target of `samples` samples, blending
        /// with `blend`.
        Pipeline color(Format format, uint32_t samples = 1, const BlendState& blend = {}) const
        {
            const std::array targets{ ColorTarget{ .format = format, .blend = blend } };
            Result<Pipeline> pipeline = kit.device.createPipeline(
                GraphicsPipelineDesc{ .vertex = { drawLibrary, "fullscreenVertexMain" },
                                      .fragment = { library, "colorFragmentMain" },
                                      .colors = targets,
                                      .samples = samples,
                                      .label = "test.formatColor" }
            );
            if (!pipeline)
            {
                FAIL(pipeline.error().message);
            }
            return *pipeline;
        }

        //==============================================================================================================
        /// A depth-only pipeline that covers the target and writes its depth into a `format` attachment of `samples` samples.
        Pipeline depth(Format format, uint32_t samples = 1) const
        {
            Result<Pipeline> pipeline = kit.device.createPipeline(
                GraphicsPipelineDesc{ .vertex = { drawLibrary, "fullscreenVertexMain" },
                                      .depthStencil = { .depthTest = true, .depthWrite = true, .depthCompare = CompareOp::Always },
                                      .depthFormat = format,
                                      .samples = samples,
                                      .label = "test.formatDepth" }
            );
            if (!pipeline)
            {
                FAIL(pipeline.error().message);
            }
            return *pipeline;
        }

        //==============================================================================================================
        /// A clamping sampler of `filter`.
        uint32_t sampler(SamplerFilter filter) const
        {
            const SamplerIndex index = *kit.device.createSampler(
                SamplerDesc{ .filter = filter, .mipFilter = SamplerFilter::Nearest, .address = SamplerAddress::ClampToEdge, .label = "test.formatSampler" }
            );
            return static_cast<uint32_t>(index);
        }

        //==============================================================================================================
        /// Runs FormatOracle's reading kernel `entry` over `view` with `root`, whose view and output it fills in, and returns
        /// the `count` values it stored.
        std::vector<Float4> read(std::string_view entry, ViewIndex view, ReadRoot root, uint32_t count) const
        {
            const Pipeline pipeline = compute(entry);
            const std::vector<uint32_t> untouched(size_t{ count } * 4, 0xDEADBEEFu);
            const Buffer output = kit.buffer(untouched.size() * sizeof(uint32_t), Memory::Gpu, BufferUsage::ShaderWrite, bytesOf<uint32_t>(untouched));
            root.view = static_cast<uint32_t>(view);
            root.output = kit.device.address(output);
            CommandList list = kit.list(QueueType::Graphics, "test.formatRead");
            list.setPipeline(pipeline);
            list.pushRoot(root);
            list.dispatch(1, 1, 1);
            kit.run(std::array{ list });
            const std::vector<std::byte> bytes = kit.readBuffer(output);
            std::vector<Float4> values(count);
            std::memcpy(values.data(), bytes.data(), values.size() * sizeof(Float4));
            return values;
        }

        test::GpuKit& kit;         ///< The kit whose device the pipelines live on.
        ShaderLibrary library;     ///< FormatOracle.
        ShaderLibrary drawLibrary; ///< DrawOracle, whose fullscreen vertex entry touches no reserved binding.
    };

    //==================================================================================================================
    /// The viewport over the whole texture, whose depth range is the single value `depth`.
    Viewport viewportAt(float depth = 0.0f)
    {
        return Viewport{ .width = static_cast<float>(kEdge), .height = static_cast<float>(kEdge), .minDepth = depth, .maxDepth = depth };
    }

    /// The scissor over the right two columns.
    constexpr Rect2D kRightHalf{ kEdge / 2, 0, kEdge / 2, kEdge };

    /// The area of every pass.
    constexpr Rect2D kWhole{ 0, 0, kEdge, kEdge };

    //==================================================================================================================
    /// Creates a `kEdge` square 2D texture of `format` with `samples` samples and `usage`.
    Texture textureOf(Device device, Format format, TextureUsage usage, uint32_t samples = 1, std::span<const TextureData> initial = {})
    {
        Result<Texture> texture = device.createTexture(
            TextureDesc{ .format = format, .extent = { kEdge, kEdge, 1 }, .samples = samples, .usage = usage, .label = "test.format" }, initial
        );
        if (!texture)
        {
            FAIL(texture.error().message);
        }
        return *texture;
    }

    //==================================================================================================================
    /// Records a pass that clears the color attachment `target` to the visible left value of `format` and covers its right
    /// two columns with `color` through `pipeline`, resolving into `resolve` when it is not null.
    void recordColorPass(CommandList list, Format format, Texture target, Pipeline pipeline, const Float4& color, Texture resolve = {})
    {
        const Float4 clear = visibleOf(format, leftOf(format));
        const std::array colors{ ColorAttachment{
            .texture = target, .load = LoadOp::Clear, .store = StoreOp::Store, .clear = { clear[0], clear[1], clear[2], clear[3] }, .resolve = resolve } };
        list.beginRenderPass(RenderPassDesc{ .colors = colors, .area = kWhole, .label = "test.formatPass" });
        list.setPipeline(pipeline);
        list.setViewport(viewportAt());
        list.setScissor(kRightHalf);
        list.pushRoot(color);
        list.draw(3);
        list.endRenderPass();
    }

    //==================================================================================================================
    /// Records a pass that clears the depth attachment `target` of `format` to the left depth and writes the right depth
    /// over its right two columns through `pipeline`.
    void recordDepthPass(CommandList list, Format format, Texture target, Pipeline pipeline)
    {
        list.beginRenderPass(
            RenderPassDesc{ .depth = { .texture = target, .load = LoadOp::Clear, .store = StoreOp::Store, .clearDepth = leftOf(format)[0] },
                            .area = kWhole,
                            .label = "test.formatDepthPass" }
        );
        list.setPipeline(pipeline);
        list.setViewport(viewportAt(rightOf(format)[0]));
        list.setScissor(kRightHalf);
        list.draw(3);
        list.endRenderPass();
    }

    //==================================================================================================================
    /// Creates a single-sample texture of `format` with `usage` besides `Sampled`, whose left two columns hold the left value
    /// and right two the right value, left in `Layout::ShaderRead` (`Layout::DepthStencilRead` for a depth format). A
    /// depth format is filled by a pass, any other by initial data.
    Texture filledTexture(test::GpuKit& kit, const Oracle& oracle, Format format, TextureUsage usage = TextureUsage::None)
    {
        if (!isDepth(format))
        {
            const std::vector<std::byte> bytes = imageOf(format);
            const std::array initial{ TextureData{ .bytes = bytes, .bytesPerRow = rowBytesOf(format) } };
            return textureOf(kit.device, format, TextureUsage::Sampled | usage, 1, initial);
        }
        const Texture texture = textureOf(kit.device, format, TextureUsage::Sampled | TextureUsage::DepthStencil | usage);
        CommandList list = kit.list(QueueType::Graphics, "test.formatFill");
        test::transition(list, texture, Layout::Undefined, Layout::DepthStencilWrite);
        recordDepthPass(list, format, texture, oracle.depth(format));
        test::transition(list, texture, Layout::DepthStencilWrite, Layout::DepthStencilRead);
        kit.run(std::array{ list });
        return texture;
    }

    //==================================================================================================================
    /// A sampled view of every texel of `texture`, of the depth aspect for a depth format.
    ViewIndex sampledView(Device device, Texture texture, Format format)
    {
        return *device.createView(texture, TextureViewDesc{ .aspect = isDepth(format) ? TextureAspect::Depth : TextureAspect::Color });
    }

    //==================================================================================================================
    /// Checks that `values`, read by a shader from texel i in turn, hold the left value in the left two columns and the right
    /// value in the right two.
    void checkTexels(Format format, std::span<const Float4> values, uint32_t samples = 1)
    {
        const Float4 left = leftOf(format);
        const Float4 right = rightOf(format);
        for (uint32_t texel = 0; texel < kTexels; ++texel)
        {
            for (uint32_t sample = 0; sample < samples; ++sample)
            {
                const Float4& expected = texel % kEdge < kEdge / 2 ? left : right;
                const std::string difference = mismatch(format, values[texel * samples + sample], expected);
                INFO(std::format("texel ({}, {}) sample {}: {}", texel % kEdge, texel / kEdge, sample, difference));
                CHECK(difference.empty());
            }
        }
    }

    //==================================================================================================================
    /// Checks that `texture` of `format` holds, byte for byte, the left value in its left two columns and the right value in
    /// its right two.
    void checkStored(test::GpuKit& kit, Texture texture, Format format)
    {
        const std::vector<std::byte> actual = kit.readTexture(texture, TextureRegion{ .extent = { kEdge, kEdge, 1 } }, format);
        CHECK(actual == imageOf(format));
    }

    //==================================================================================================================
    /// Stores the left and right values of `format` into a new Storage texture through FormatOracle's kernel `entry` and
    /// checks the texels it holds.
    void storeTexels(test::GpuKit& kit, Format format, std::string_view entry)
    {
        const Oracle oracle(kit);
        const Pipeline pipeline = oracle.compute(entry);
        const Texture texture = textureOf(kit.device, format, TextureUsage::Storage);
        const ViewIndex view = *kit.device.createView(texture, TextureViewDesc{ .usage = TextureUsage::Storage });
        CommandList list = kit.list(QueueType::Graphics, "test.formatStore");
        test::transition(list, texture, Layout::Undefined, Layout::ShaderWrite);
        list.setPipeline(pipeline);
        list.pushRoot(WriteRoot{ .view = static_cast<uint32_t>(view), .left = visibleOf(format, leftOf(format)), .right = visibleOf(format, rightOf(format)) });
        list.dispatch(1, 1, 1);
        test::transition(list, texture, Layout::ShaderWrite, Layout::CopySrc);
        kit.run(std::array{ list });
        checkStored(kit, texture, format);
    }

    //==================================================================================================================
    /// Skips the running case for `format` unless the device's table reports `capability` (named `name`) for it.
    void requireReported(bool capability, Format format, std::string_view name)
    {
        if (!capability)
        {
            SKIP(std::format("the device does not report {} for {}", name, traitsOf(format).name));
        }
    }
} // namespace

//======================================================================================================================
TEST_CASE("every format with a packed size round-trips its texels through buffer copies", "[conformance]")
{
    test::requireGpu();
    const Format format = GENERATE(from_range(kFormats));
    CAPTURE(traitsOf(format).name);
    if (format == Format::D32FloatS8)
    {
        SKIP("chapter 07 copies no format with a stencil component (VU-rojo-cmd-0041); D32FloatS8 depth is read by sampling");
    }
    test::GpuKit kit;
    const std::vector<std::byte> image = imageOf(format);
    const uint64_t rowBytes = rowBytesOf(format);
    const uint64_t rows = image.size() / rowBytes;
    constexpr uint32_t kPitch = 256;
    std::vector<std::byte> padded(kPitch * rows);
    for (uint64_t row = 0; row < rows; ++row)
    {
        std::memcpy(padded.data() + row * kPitch, image.data() + row * rowBytes, rowBytes);
    }
    const Buffer upload = kit.buffer(padded.size(), Memory::Upload, BufferUsage::None, padded);
    const Texture texture = textureOf(kit.device, format, TextureUsage::Sampled);
    CommandList list = kit.list(QueueType::Graphics, "test.formatCopy");
    test::transition(list, texture, Layout::Undefined, Layout::CopyDst);
    list.copyBufferToTexture(upload, BufferTextureLayout{ .rowPitch = kPitch }, texture, TextureRegion{ .extent = { kEdge, kEdge, 1 } });
    test::transition(list, texture, Layout::CopyDst, Layout::CopySrc);
    kit.run(std::array{ list });
    CHECK(kit.readTexture(texture, TextureRegion{ .extent = { kEdge, kEdge, 1 } }, format) == image);
}

//======================================================================================================================
TEST_CASE("a sampled format returns each texel through a nearest sampler", "[conformance]")
{
    test::requireGpu();
    const Format format = GENERATE(from_range(kFormats));
    CAPTURE(traitsOf(format).name);
    test::GpuKit kit;
    requireReported(kit.device.formatCapabilities(format).sampled, format, "sampled");
    const Oracle oracle(kit);
    const Texture texture = filledTexture(kit, oracle, format);
    const ViewIndex view = sampledView(kit.device, texture, format);
    checkTexels(format, oracle.read("sampleTexelsMain", view, ReadRoot{ .sampler = oracle.sampler(SamplerFilter::Nearest) }, kTexels));
}

//======================================================================================================================
TEST_CASE("a linearly filtered format returns the mean of the two texels a sample falls between", "[conformance]")
{
    test::requireGpu();
    const Format format = GENERATE(from_range(kFormats));
    CAPTURE(traitsOf(format).name);
    test::GpuKit kit;
    requireReported(kit.device.formatCapabilities(format).filterLinear, format, "filterLinear");
    const Oracle oracle(kit);
    const Texture texture = filledTexture(kit, oracle, format);
    const ViewIndex view = sampledView(kit.device, texture, format);
    // The centre of the texture lies halfway between columns 1 and 2 and between rows 1 and 2, which hold the same values.
    const ReadRoot root{ .sampler = oracle.sampler(SamplerFilter::Linear), .at = { 0.5f, 0.5f } };
    const std::string difference = mismatchMean(format, oracle.read("sampleAtMain", view, root, 1)[0], leftOf(format), rightOf(format));
    INFO(difference);
    CHECK(difference.empty());
}

//======================================================================================================================
TEST_CASE("a storage-readable format loads each texel through rwTexture2D", "[conformance]")
{
    test::requireGpu();
    const Format format = GENERATE(from_range(kFormats));
    CAPTURE(traitsOf(format).name);
    test::GpuKit kit;
    requireReported(kit.device.formatCapabilities(format).storageRead, format, "storageRead");
    const Oracle oracle(kit);
    const Texture texture = filledTexture(kit, oracle, format, TextureUsage::Storage);
    const ViewIndex view = *kit.device.createView(texture, TextureViewDesc{ .usage = TextureUsage::Storage });
    CommandList list = kit.list(QueueType::Graphics, "test.formatToStorage");
    test::transition(list, texture, Layout::ShaderRead, Layout::ShaderWrite);
    kit.run(std::array{ list });
    checkTexels(format, oracle.read("loadTexelsMain", view, ReadRoot{}, kTexels));
}

//======================================================================================================================
TEST_CASE("a storage-readable and -writable format stores each texel through rwTexture2D", "[conformance]")
{
    test::requireGpu();
    const Format format = GENERATE(from_range(kFormats));
    CAPTURE(traitsOf(format).name);
    test::GpuKit kit;
    const FormatCapabilities capabilities = kit.device.formatCapabilities(format);
    requireReported(capabilities.storageWrite, format, "storageWrite");
    requireReported(capabilities.storageRead, format, "storageRead");
    storeTexels(kit, format, "storeTexelsMain");
}

//======================================================================================================================
TEST_CASE("a format with storage writes but no storage reads stores each texel through wTexture2D", "[conformance]")
{
    test::requireGpu();
    const Format format = GENERATE(from_range(kFormats));
    CAPTURE(traitsOf(format).name);
    test::GpuKit kit;
    const FormatCapabilities capabilities = kit.device.formatCapabilities(format);
    requireReported(capabilities.storageWrite, format, "storageWrite");
    if (capabilities.storageRead)
    {
        SKIP(std::format("{} is read and written through rwTexture2D", traitsOf(format).name));
    }
    // Chapter 15: a format whose storage writes are not matched by reads is written through the write-only accessors.
    storeTexels(kit, format, "writeTexelsMain");
}

//======================================================================================================================
TEST_CASE("a color-attachment format keeps the clear color and stores a drawn color", "[conformance]")
{
    test::requireGpu();
    const Format format = GENERATE(from_range(kFormats));
    CAPTURE(traitsOf(format).name);
    test::GpuKit kit;
    requireReported(kit.device.formatCapabilities(format).colorAttachment, format, "colorAttachment");
    const Oracle oracle(kit);
    const Texture target = textureOf(kit.device, format, TextureUsage::RenderTarget);
    CommandList list = kit.list(QueueType::Graphics, "test.formatColor");
    test::transition(list, target, Layout::Undefined, Layout::ColorAttachment);
    recordColorPass(list, format, target, oracle.color(format), visibleOf(format, rightOf(format)));
    test::transition(list, target, Layout::ColorAttachment, Layout::CopySrc);
    kit.run(std::array{ list });
    checkStored(kit, target, format);
}

//======================================================================================================================
TEST_CASE("a blendable format adds a drawn color to the cleared destination", "[conformance]")
{
    test::requireGpu();
    const Format format = GENERATE(from_range(kFormats));
    CAPTURE(traitsOf(format).name);
    test::GpuKit kit;
    requireReported(kit.device.formatCapabilities(format).blend, format, "blend");
    const Oracle oracle(kit);
    const BlendState add{ .enable = true, .dstColor = BlendFactor::One, .dstAlpha = BlendFactor::One };
    const Texture target = textureOf(kit.device, format, TextureUsage::RenderTarget);
    CommandList list = kit.list(QueueType::Graphics, "test.formatBlend");
    test::transition(list, target, Layout::Undefined, Layout::ColorAttachment);
    // The draw adds the right value less the left one, in the space blending works in, to the cleared left value.
    const Float4 added = difference(visibleOf(format, rightOf(format)), visibleOf(format, leftOf(format)));
    recordColorPass(list, format, target, oracle.color(format, 1, add), added);
    test::transition(list, target, Layout::ColorAttachment, Layout::CopySrc);
    kit.run(std::array{ list });
    checkStored(kit, target, format);
}

//======================================================================================================================
TEST_CASE("a resolvable format receives the mean of a multisampled pass's samples", "[conformance]")
{
    test::requireGpu();
    const Format format = GENERATE(from_range(kFormats));
    CAPTURE(traitsOf(format).name);
    test::GpuKit kit;
    const FormatCapabilities capabilities = kit.device.formatCapabilities(format);
    requireReported(capabilities.resolve, format, "resolve");
    // The smallest multisampled count the format supports; every sample of a texel holds one value, so the mean is exact.
    const uint32_t samples = capabilities.sampleCounts & ~1u & (0u - (capabilities.sampleCounts & ~1u));
    requireReported(samples != 0, format, "a sample count above 1");
    CAPTURE(samples);
    const Oracle oracle(kit);
    const Texture target = textureOf(kit.device, format, TextureUsage::RenderTarget, samples);
    const Texture resolve = textureOf(kit.device, format, TextureUsage::RenderTarget);
    CommandList list = kit.list(QueueType::Graphics, "test.formatResolve");
    test::transition(list, target, Layout::Undefined, Layout::ColorAttachment);
    test::transition(list, resolve, Layout::Undefined, Layout::ColorAttachment);
    recordColorPass(list, format, target, oracle.color(format, samples), visibleOf(format, rightOf(format)), resolve);
    test::transition(list, resolve, Layout::ColorAttachment, Layout::CopySrc);
    kit.run(std::array{ list });
    checkStored(kit, resolve, format);
}

//======================================================================================================================
TEST_CASE("each reported sample count of a format holds the clear and the drawn value in every sample", "[conformance]")
{
    test::requireGpu();
    const Format format = GENERATE(from_range(kFormats));
    const uint32_t samples = GENERATE(2u, 4u, 8u);
    CAPTURE(traitsOf(format).name, samples);
    test::GpuKit kit;
    const FormatCapabilities capabilities = kit.device.formatCapabilities(format);
    // A count of 1 is the single-sample attachment the color and depth cases render to.
    requireReported((capabilities.sampleCounts & samples) != 0, format, std::format("{} samples", samples));
    requireReported(capabilities.colorAttachment || capabilities.depthAttachment, format, "an attachment");
    // The samples are read through a sampled view.
    requireReported(capabilities.sampled, format, "sampled");
    const Oracle oracle(kit);
    CommandList list = kit.list(QueueType::Graphics, "test.formatSamples");
    Texture target;
    if (isDepth(format))
    {
        target = textureOf(kit.device, format, TextureUsage::Sampled | TextureUsage::DepthStencil, samples);
        test::transition(list, target, Layout::Undefined, Layout::DepthStencilWrite);
        recordDepthPass(list, format, target, oracle.depth(format, samples));
        test::transition(list, target, Layout::DepthStencilWrite, Layout::DepthStencilRead);
    }
    else
    {
        target = textureOf(kit.device, format, TextureUsage::Sampled | TextureUsage::RenderTarget, samples);
        test::transition(list, target, Layout::Undefined, Layout::ColorAttachment);
        recordColorPass(list, format, target, oracle.color(format, samples), visibleOf(format, rightOf(format)));
        test::transition(list, target, Layout::ColorAttachment, Layout::ShaderRead);
    }
    kit.run(std::array{ list });
    const ViewIndex view = sampledView(kit.device, target, format);
    const std::vector<Float4> values = oracle.read("readSamplesMain", view, ReadRoot{ .samples = samples }, kTexels * kMaxSamples);
    std::vector<Float4> read;
    for (uint32_t texel = 0; texel < kTexels; ++texel)
    {
        read.insert(read.end(), values.begin() + texel * kMaxSamples, values.begin() + texel * kMaxSamples + samples);
    }
    checkTexels(format, read, samples);
}

//======================================================================================================================
TEST_CASE("a depth format keeps a cleared and a drawn depth for copies and sampling", "[conformance]")
{
    test::requireGpu();
    const Format format = GENERATE(from_range(kFormats));
    CAPTURE(traitsOf(format).name);
    test::GpuKit kit;
    const FormatCapabilities capabilities = kit.device.formatCapabilities(format);
    requireReported(capabilities.depthAttachment, format, "depthAttachment");
    // The depth is read back through a sampled view, and through a copy where chapter 07 allows one.
    requireReported(capabilities.sampled, format, "sampled");
    const Oracle oracle(kit);
    const Texture texture = filledTexture(kit, oracle, format);
    // Chapter 07 copies no format with a stencil component, so D32FloatS8's depth is read by sampling alone.
    if (format != Format::D32FloatS8)
    {
        CommandList list = kit.list(QueueType::Graphics, "test.formatToCopy");
        test::transition(list, texture, Layout::DepthStencilRead, Layout::CopySrc);
        kit.run(std::array{ list });
        checkStored(kit, texture, format);
        CommandList back = kit.list(QueueType::Graphics, "test.formatToRead");
        test::transition(back, texture, Layout::CopySrc, Layout::DepthStencilRead);
        kit.run(std::array{ back });
    }
    const ViewIndex view = sampledView(kit.device, texture, format);
    checkTexels(format, oracle.read("sampleTexelsMain", view, ReadRoot{ .sampler = oracle.sampler(SamplerFilter::Nearest) }, kTexels));
}
