//----------------------------------------------------------------------------------------------------------------------
/// @file UpscaleConformanceTests.cpp
/// @brief Observes `upscale` on a GPU: uniform inputs reconstruct to their color, jitter moves an edge, and D32FloatS8 depth upscales as D32Float.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Support/Conformance.h"
#include "Support/GpuKit.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

using namespace rojoRHI;

namespace {
    /// The input extent of every upscaler of the cases, in texels.
    constexpr uint32_t kInput = 64;

    /// The output extent: twice the input in each axis.
    constexpr uint32_t kOutput = 128;

    /// The color every color input texel holds; each channel is exact in `RGBA16Float`.
    constexpr ClearColor kColor{ 0.25f, 0.5f, 0.75f, 1.0f };

    /// A color an input holds before the write a case orders the upscale after; each channel differs from `kColor`'s.
    constexpr ClearColor kStale{ 1.0f, 0.0f, 0.25f, 1.0f };

    /// What the output holds before the upscale, so a texel the upscale does not write fails the comparison; each channel
    /// differs from `kColor`'s.
    constexpr ClearColor kSentinel{ 0.0f, 1.0f, 0.0f, 0.0f };

    /// The bound on the difference between an output channel and the input's. `RGBA16Float` keeps 11 significant bits, a
    /// step of 2^-11 for values in [0.5, 1); a reconstruction that filters, tonemaps and accumulates history in that precision
    /// rounds several times per texel, so the bound is eight such steps, 2^-8. Chapter 12 states no precision of its own; the
    /// bound is fixed from the format and is never widened to fit a result (ADR 0009).
    constexpr float kTolerance = 1.0f / 256.0f;

    /// Every subresource of a single-mip, single-layer texture.
    constexpr SubresourceRange kWhole{ .baseMip = 0, .mipCount = 1, .baseLayer = 0, .layerCount = 1 };

    //==================================================================================================================
    /// Returns the value of the IEEE half-precision bits `bits`.
    float halfToFloat(uint16_t bits)
    {
        const uint32_t sign = (bits >> 15) & 1u;
        const uint32_t exponent = (bits >> 10) & 0x1Fu;
        const uint32_t mantissa = bits & 0x3FFu;
        float magnitude = 0.0f;
        if (exponent == 0)
        {
            magnitude = std::ldexp(static_cast<float>(mantissa), -24);
        }
        else if (exponent == 0x1F)
        {
            magnitude = mantissa == 0 ? INFINITY : NAN;
        }
        else
        {
            magnitude = std::ldexp(static_cast<float>(mantissa | 0x400u), static_cast<int>(exponent) - 25);
        }
        return sign != 0 ? -magnitude : magnitude;
    }

    //==================================================================================================================
    /// Records a barrier that moves the whole of `texture` from `oldLayout` to `newLayout`, from `srcStage` and `srcAccess` to
    /// `dstStage` and `dstAccess`.
    void transition(CommandList list, Texture texture, Layout oldLayout, Layout newLayout, Stage srcStage, Access srcAccess, Stage dstStage, Access dstAccess)
    {
        const std::array<TextureBarrier, 1> barriers{ TextureBarrier{ .texture = texture,
                                                                      .range = kWhole,
                                                                      .srcStage = srcStage,
                                                                      .srcAccess = srcAccess,
                                                                      .dstStage = dstStage,
                                                                      .dstAccess = dstAccess,
                                                                      .oldLayout = oldLayout,
                                                                      .newLayout = newLayout } };
        list.barrier({}, {}, barriers);
    }

    //==================================================================================================================
    /// Discards `texture`, clears it to `value` and moves it to `layout` for the upscale, which reads or writes it as compute
    /// work.
    void clearFor(CommandList list, Texture texture, const ClearValue& value, Layout layout, Access access)
    {
        transition(list, texture, Layout::Undefined, Layout::CopyDst, Stage::None, Access::None, Stage::Copy, Access::CopyWrite);
        list.clearTexture(texture, kWhole, value);
        transition(list, texture, Layout::CopyDst, layout, Stage::Copy, Access::CopyWrite, Stage::ComputeShader, access);
    }

    /// The textures of one upscale of the cases.
    struct Inputs
    {
        Texture color;    ///< `RGBA16Float`, the input extent.
        Texture depth;    ///< `D32Float`, the input extent.
        Texture motion;   ///< `RG16Float`, the input extent.
        Texture exposure; ///< `R16Float`, 1x1; created only when a case asks for it.
        Texture reactive; ///< `R8Unorm`, the input extent; created only when a case asks for it.
    };

    //==================================================================================================================
    /// Creates a 2D texture of `format`, `width` by `height` texels, with `usage`.
    Texture make(Device device, Format format, uint32_t width, uint32_t height, TextureUsage usage, const char* label)
    {
        return *device.createTexture(TextureDesc{ .format = format, .extent = { width, height, 1 }, .usage = usage, .label = label });
    }

    //==================================================================================================================
    /// Creates the inputs, the exposure and reactive textures when `optional` is set, and records on `list` the clears that
    /// give them uniform values (the color `color`, depth 0.5, no motion, exposure 1, reactive 1) and the barriers that make
    /// them readable by the upscale.
    Inputs makeInputs(Device device, CommandList list, const ClearColor& color, bool optional)
    {
        constexpr TextureUsage kColorInput = TextureUsage::Sampled | TextureUsage::RenderTarget;
        Inputs inputs;
        inputs.color = make(device, Format::RGBA16Float, kInput, kInput, kColorInput, "test.color");
        inputs.depth = make(device, Format::D32Float, kInput, kInput, TextureUsage::Sampled | TextureUsage::DepthStencil, "test.depth");
        inputs.motion = make(device, Format::RG16Float, kInput, kInput, kColorInput, "test.motion");
        clearFor(list, inputs.color, ClearValue{ .color = color }, Layout::ShaderRead, Access::ShaderRead);
        clearFor(list, inputs.depth, ClearValue{ .depth = 0.5f }, Layout::ShaderRead, Access::ShaderRead);
        clearFor(list, inputs.motion, ClearValue{}, Layout::ShaderRead, Access::ShaderRead);
        if (optional)
        {
            inputs.exposure = make(device, Format::R16Float, 1, 1, kColorInput, "test.exposure");
            inputs.reactive = make(device, Format::R8Unorm, kInput, kInput, kColorInput, "test.reactive");
            clearFor(list, inputs.exposure, ClearValue{ .color = { 1.0f, 0.0f, 0.0f, 0.0f } }, Layout::ShaderRead, Access::ShaderRead);
            clearFor(list, inputs.reactive, ClearValue{ .color = { 1.0f, 0.0f, 0.0f, 0.0f } }, Layout::ShaderRead, Access::ShaderRead);
        }
        return inputs;
    }

    //==================================================================================================================
    /// Creates an output texture and records on `list` its clear to `kSentinel` and the barrier that makes it writable by the
    /// upscale.
    Texture makeOutput(Device device, CommandList list)
    {
        const Texture output = make(device, Format::RGBA16Float, kOutput, kOutput, TextureUsage::Storage | TextureUsage::RenderTarget, "test.output");
        clearFor(list, output, ClearValue{ .color = kSentinel }, Layout::ShaderWrite, Access::ShaderWrite);
        return output;
    }

    //==================================================================================================================
    /// Returns the parameters of an upscale of `inputs` into `output`.
    UpscaleParams paramsOf(const Inputs& inputs, Texture output, bool reset)
    {
        return UpscaleParams{ .color = inputs.color,
                              .depth = inputs.depth,
                              .motion = inputs.motion,
                              .output = output,
                              .exposure = inputs.exposure,
                              .reactive = inputs.reactive,
                              .reset = reset,
                              .label = "test.upscale" };
    }

    //==================================================================================================================
    /// Records the barrier that orders a copy of `output` after the upscale that wrote it.
    void readyToRead(CommandList list, Texture output)
    {
        transition(list, output, Layout::ShaderWrite, Layout::CopySrc, Stage::ComputeShader, Access::ShaderWrite, Stage::Copy, Access::CopyRead);
    }

    //==================================================================================================================
    /// Checks that every texel of `output` holds `kColor`'s red, green and blue within `kTolerance`. Alpha is not compared:
    /// the implementations chapter 12 fronts reconstruct color, and none states what it writes to alpha.
    void checkTexels(std::span<const std::byte> bytes)
    {
        REQUIRE(bytes.size() == size_t{ kOutput } * kOutput * 8);
        const std::array<float, 3> expected{ kColor.r, kColor.g, kColor.b };
        uint32_t wrong = 0;
        for (size_t texel = 0; texel < size_t{ kOutput } * kOutput; ++texel)
        {
            std::array<uint16_t, 4> halves{};
            std::memcpy(halves.data(), bytes.data() + texel * 8, 8);
            for (size_t channel = 0; channel < expected.size(); ++channel)
            {
                const float value = halfToFloat(halves[channel]);
                if (!(std::fabs(value - expected[channel]) <= kTolerance))
                {
                    if (wrong < 8)
                    {
                        UNSCOPED_INFO("texel " << texel << " channel " << channel << " holds " << value << ", expected " << expected[channel]);
                    }
                    ++wrong;
                }
            }
        }
        CHECK(wrong == 0);
    }

    //==================================================================================================================
    /// Checks every texel of `output` as `checkTexels` does, read back on its own list after a host wait.
    void checkUniform(test::GpuKit& kit, Texture output)
    {
        checkTexels(kit.readTexture(output, TextureRegion{ .extent = { kOutput, kOutput, 1 } }, Format::RGBA16Float));
    }

    //==================================================================================================================
    /// Records on `list` the barrier that orders a copy of `output` after the upscale that wrote it, and that copy into
    /// `readback`, in tight rows of `kOutput` texels.
    void copyOut(CommandList list, Texture output, Buffer readback)
    {
        readyToRead(list, output);
        list.copyTextureToBuffer(
            output, TextureRegion{ .extent = { kOutput, kOutput, 1 } }, readback, BufferTextureLayout{ .rowPitch = kOutput * 8, .rowsPerImage = kOutput }
        );
    }

    //==================================================================================================================
    /// Returns the IEEE half-precision bits of `value`, which is zero or a normal half-precision value held exactly.
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

    /// The darker of the two shades a patterned color input holds; exact in `RGBA16Float`.
    constexpr float kDark = 0.25f;

    /// The lighter of the two shades a patterned color input holds; exact in `RGBA16Float`.
    constexpr float kLight = 0.75f;

    //==================================================================================================================
    /// Creates a color input of the input extent whose texel (x, y) holds `shade(x, y)` in red, green and blue and 1 in
    /// alpha, left in `ShaderRead` by its initial data.
    template<class Shade> Texture shadedColor(Device device, Shade shade)
    {
        std::vector<uint16_t> halves;
        halves.reserve(size_t{ kInput } * kInput * 4);
        for (uint32_t y = 0; y < kInput; ++y)
        {
            for (uint32_t x = 0; x < kInput; ++x)
            {
                const uint16_t value = halfOf(shade(x, y));
                halves.insert(halves.end(), { value, value, value, halfOf(1.0f) });
            }
        }
        std::vector<std::byte> bytes(halves.size() * sizeof(uint16_t));
        std::memcpy(bytes.data(), halves.data(), bytes.size());
        return *device.createTexture(
            TextureDesc{ .format = Format::RGBA16Float,
                         .extent = { kInput, kInput, 1 },
                         .usage = TextureUsage::Sampled | TextureUsage::RenderTarget,
                         .label = "test.shaded" },
            std::array{ TextureData{ bytes, uint64_t{ kInput } * 8, 0 } }
        );
    }

    //==================================================================================================================
    /// Returns the red channel of each texel of `bytes`, an `RGBA16Float` output in tight rows.
    std::vector<float> redOf(std::span<const std::byte> bytes)
    {
        std::vector<float> red(bytes.size() / 8);
        for (size_t texel = 0; texel < red.size(); ++texel)
        {
            uint16_t half = 0;
            std::memcpy(&half, bytes.data() + texel * 8, sizeof(half));
            red[texel] = halfToFloat(half);
        }
        return red;
    }
} // namespace

//======================================================================================================================
TEST_CASE("an upscale of uniform inputs writes the input color to every output texel", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const Upscaler upscaler = *kit.device.createUpscaler(UpscalerDesc{ .input = { kInput, kInput }, .output = { kOutput, kOutput }, .label = "test.upscaler" });
    CommandList list = kit.list(QueueType::Graphics, "test.upscale");
    const Inputs inputs = makeInputs(kit.device, list, kColor, false);
    const Texture output = makeOutput(kit.device, list);
    list.upscale(upscaler, paramsOf(inputs, output, true));
    readyToRead(list, output);
    kit.run(std::array{ list });
    checkUniform(kit, output);
}

//======================================================================================================================
TEST_CASE("an upscale with a content rectangle, exposure and reactive inputs writes every output texel", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const Upscaler upscaler = *kit.device.createUpscaler(UpscalerDesc{ .input = { kInput, kInput }, .output = { kOutput, kOutput }, .label = "test.upscaler" });
    CommandList list = kit.list(QueueType::Graphics, "test.upscale");
    const Inputs inputs = makeInputs(kit.device, list, kColor, true);
    const Texture output = makeOutput(kit.device, list);
    UpscaleParams params = paramsOf(inputs, output, true);
    // Uniform inputs cannot show which texels the rectangle covers: the case checks that the scaler takes a content rectangle,
    // an exposure and a reactive mask and still writes every output texel. 48 of 64 texels in each axis: a scale of 128 / 48,
    // inside the range the upscaler was created for.
    params.inputContent = { 48, 48 };
    list.upscale(upscaler, params);
    readyToRead(list, output);
    kit.run(std::array{ list });
    checkUniform(kit, output);
}

//======================================================================================================================
TEST_CASE("two upscalers upscale in one frame and a third frame reuses their history", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const UpscalerDesc desc{ .input = { kInput, kInput }, .output = { kOutput, kOutput }, .label = "test.upscaler" };
    const std::array<Upscaler, 2> upscalers{ *kit.device.createUpscaler(desc), *kit.device.createUpscaler(desc) };
    CommandList first = kit.list(QueueType::Graphics, "test.prepare");
    const Inputs inputs = makeInputs(kit.device, first, kColor, false);
    const std::array<Texture, 2> outputs{ makeOutput(kit.device, first), makeOutput(kit.device, first) };
    for (uint32_t frame = 0; frame < 3; ++frame)
    {
        CAPTURE(frame);
        CommandList list = frame == 0 ? first : kit.list(QueueType::Graphics, "test.upscale");
        for (size_t i = 0; i < upscalers.size(); ++i)
        {
            if (frame > 0)
            {
                // Each frame's upscale must write every texel again: the output holds the sentinel before it, not the last
                // frame's result.
                clearFor(list, outputs[i], ClearValue{ .color = kSentinel }, Layout::ShaderWrite, Access::ShaderWrite);
            }
            // The first frame discards the history; the later ones accumulate on it.
            list.upscale(upscalers[i], paramsOf(inputs, outputs[i], frame == 0));
            readyToRead(list, outputs[i]);
        }
        kit.run(std::array{ list });
        checkUniform(kit, outputs[0]);
        checkUniform(kit, outputs[1]);
        kit.device.endFrame();
        kit.device.beginFrame();
    }
}

//======================================================================================================================
TEST_CASE("an upscale on a list whose previous pass wrote the color input sees that write", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const Upscaler upscaler = *kit.device.createUpscaler(UpscalerDesc{ .input = { kInput, kInput }, .output = { kOutput, kOutput }, .label = "test.upscaler" });
    CommandList prepare = kit.list(QueueType::Graphics, "test.prepare");
    const Inputs inputs = makeInputs(kit.device, prepare, kStale, false);
    const Texture output = makeOutput(kit.device, prepare);
    kit.run(std::array{ prepare });
    // The list writes the color input, then upscales it: the upscale must read the new color, not the stale one.
    CommandList list = kit.list(QueueType::Graphics, "test.upscale");
    transition(list, inputs.color, Layout::ShaderRead, Layout::CopyDst, Stage::ComputeShader, Access::None, Stage::Copy, Access::CopyWrite);
    list.clearTexture(inputs.color, kWhole, ClearValue{ .color = kColor });
    transition(list, inputs.color, Layout::CopyDst, Layout::ShaderRead, Stage::Copy, Access::CopyWrite, Stage::ComputeShader, Access::ShaderRead);
    list.upscale(upscaler, paramsOf(inputs, output, true));
    readyToRead(list, output);
    kit.run(std::array{ list });
    checkUniform(kit, output);
}

//======================================================================================================================
TEST_CASE("upscales of two frames and the reads of their output on later lists are ordered without a host wait", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const Upscaler upscaler = *kit.device.createUpscaler(UpscalerDesc{ .input = { kInput, kInput }, .output = { kOutput, kOutput }, .label = "test.upscaler" });
    CommandList prepare = kit.list(QueueType::Graphics, "test.prepare");
    const Inputs inputs = makeInputs(kit.device, prepare, kColor, false);
    const Texture output = makeOutput(kit.device, prepare);
    kit.run(std::array{ prepare });
    const std::array<Buffer, 2> readbacks{ kit.buffer(uint64_t{ kOutput } * kOutput * 8, Memory::Readback),
                                           kit.buffer(uint64_t{ kOutput } * kOutput * 8, Memory::Readback) };
    Queue queue = kit.device.queue(QueueType::Graphics);
    const Fence done = *kit.device.createFence(0, "test.done");
    // Each frame upscales on one list and reads the output on the next, each its own submission, and nothing waits on the
    // host until the end: only the barriers recorded on the lists and the backend's own ordering keep the copy after
    // MetalFX's writes, the second frame's clear after the first frame's copy, and the second frame's MetalFX work after the
    // first's on the history. This is a smoke case: on this hardware it passes with that ordering missing too, so it cannot
    // catch a missing barrier. The native call and barrier counts the [metal4] cases of Metal4UpscalerTests.cpp pin carry
    // the ordering.
    for (uint32_t frame = 0; frame < 2; ++frame)
    {
        CAPTURE(frame);
        CommandList upscale = kit.device.acquireCommandList(QueueType::Graphics, "test.upscale");
        if (frame > 0)
        {
            // The sentinel goes back in after the last frame's copy read the output, so this frame's upscale must write
            // every texel again.
            transition(upscale, output, Layout::CopySrc, Layout::CopyDst, Stage::Copy, Access::None, Stage::Copy, Access::CopyWrite);
            upscale.clearTexture(output, kWhole, ClearValue{ .color = kSentinel });
            transition(upscale, output, Layout::CopyDst, Layout::ShaderWrite, Stage::Copy, Access::CopyWrite, Stage::ComputeShader, Access::ShaderWrite);
        }
        // The first frame discards the history; the second accumulates on it.
        upscale.upscale(upscaler, paramsOf(inputs, output, frame == 0));
        upscale.end();
        queue.submit(std::array{ upscale });
        CommandList read = kit.device.acquireCommandList(QueueType::Graphics, "test.read");
        copyOut(read, output, readbacks[frame]);
        read.end();
        if (frame == 0)
        {
            queue.submit(std::array{ read });
            kit.device.endFrame();
            kit.device.beginFrame();
        }
        else
        {
            const std::array<FenceValue, 1> signal{ FenceValue{ done, 1 } };
            queue.submit(std::array{ read }, {}, signal);
        }
    }
    REQUIRE(kit.device.wait(FenceValue{ done, 1 }, UINT64_MAX));
    for (uint32_t frame = 0; frame < 2; ++frame)
    {
        CAPTURE(frame);
        checkTexels(kit.read(readbacks[frame]));
    }
}

//======================================================================================================================
TEST_CASE("a positive jitter moves the reconstructed edge of a still frame left and up", "[conformance]")
{
    test::requireGpu();
    test::requireUpscaleCompletion();
    // A vertical edge for jitterX, a horizontal one for jitterY.
    const bool alongX = GENERATE(true, false);
    CAPTURE(alongX);
    test::GpuKit kit;
    // The input is dark before texel kInput / 2 along the axis and light from it. The jitter is the offset by which the
    // frame's projection moved the scene, in input texels, positive right and down (chapter 12): a scene whose edge lies at
    // e renders that edge at e + jitter, so the upscaler, which reconstructs the unjittered scene, puts the input's edge at
    // kInput / 2 - jitter, scaled to the output.
    const Texture color = shadedColor(kit.device, [&](uint32_t x, uint32_t y) { return (alongX ? x : y) < kInput / 2 ? kDark : kLight; });
    // Where the edge lies in the output, in output texels: across each line of the other axis, the output texels of the
    // light side's share of each texel summed from the far end, averaged over the lines.
    const auto edgeWith = [&](float jitter) {
        const Upscaler upscaler =
            *kit.device.createUpscaler(UpscalerDesc{ .input = { kInput, kInput }, .output = { kOutput, kOutput }, .label = "test.upscaler" });
        CommandList list = kit.list(QueueType::Graphics, "test.jitter");
        const Texture depth = make(kit.device, Format::D32Float, kInput, kInput, TextureUsage::Sampled | TextureUsage::DepthStencil, "test.depth");
        const Texture motion = make(kit.device, Format::RG16Float, kInput, kInput, TextureUsage::Sampled | TextureUsage::RenderTarget, "test.motion");
        clearFor(list, depth, ClearValue{ .depth = 0.5f }, Layout::ShaderRead, Access::ShaderRead);
        clearFor(list, motion, ClearValue{}, Layout::ShaderRead, Access::ShaderRead);
        const Texture output = makeOutput(kit.device, list);
        UpscaleParams params{ .color = color, .depth = depth, .motion = motion, .output = output, .reset = true, .label = "test.upscale" };
        (alongX ? params.jitterX : params.jitterY) = jitter;
        list.upscale(upscaler, params);
        readyToRead(list, output);
        kit.run(std::array{ list });
        const std::vector<float> red = redOf(kit.readTexture(output, TextureRegion{ .extent = { kOutput, kOutput, 1 } }, Format::RGBA16Float));
        kit.device.destroy(upscaler);
        double sum = 0.0;
        for (uint32_t line = 0; line < kOutput; ++line)
        {
            for (uint32_t along = 0; along < kOutput; ++along)
            {
                const size_t texel = alongX ? size_t{ line } * kOutput + along : size_t{ along } * kOutput + line;
                sum += 1.0 - (red[texel] - kDark) / (kLight - kDark);
            }
        }
        return sum / kOutput;
    };
    constexpr float kJitter = 0.375f;
    const double minus = edgeWith(-kJitter);
    const double none = edgeWith(0.0f);
    const double plus = edgeWith(kJitter);
    INFO("edge at " << minus << " for -" << kJitter << ", " << none << " for 0 and " << plus << " for +" << kJitter << " output texels");
    // The edge lies at kOutput / 2 - jitter * kOutput / kInput, 0.75 output texels from the middle for each sign. The bound,
    // a third of that, leaves room for the reconstruction's filter and still tells the signs apart.
    constexpr double kShift = static_cast<double>(kJitter) * kOutput / kInput;
    constexpr double kBound = kShift / 3.0;
    CHECK(std::fabs(none - kOutput / 2.0) < kBound);
    CHECK(std::fabs(minus - (kOutput / 2.0 + kShift)) < kBound);
    CHECK(std::fabs(plus - (kOutput / 2.0 - kShift)) < kBound);
}

//======================================================================================================================
TEST_CASE("a D32FloatS8 depth upscales as the same D32Float depth does, frame after frame", "[conformance]")
{
    test::requireGpu();
    test::requireUpscaleCompletion();
    test::GpuKit kit;
    const ShaderLibrary library = *kit.device.loadShaderLibrary(test::shaderLibraryFile("DrawOracle"), "DrawOracle");
    // Motion of one input texel right and two down in the right half, none in the left.
    std::vector<uint16_t> halves;
    for (uint32_t y = 0; y < kInput; ++y)
    {
        for (uint32_t x = 0; x < kInput; ++x)
        {
            const bool right = x >= kInput / 2;
            halves.insert(halves.end(), { halfOf(right ? 1.0f : 0.0f), halfOf(right ? 2.0f : 0.0f) });
        }
    }
    std::vector<std::byte> motionBytes(halves.size() * sizeof(uint16_t));
    std::memcpy(motionBytes.data(), halves.data(), motionBytes.size());
    // Four frames of a checkerboard whose phase flips each frame, over a depth of 0.8 with one half drawn nearer, at 0.5.
    // Where the halves meet, the upscaler takes each texel's motion from the nearest depth around it, so the texels there
    // reproject the history by the motion of the nearer half, and the output follows which half the depth puts nearer.
    constexpr uint32_t kFrames = 4;
    const auto run = [&](Format depthFormat, bool nearLeft) {
        const Pipeline pipeline = *kit.device.createPipeline(
            GraphicsPipelineDesc{ .vertex = { library, "fullscreenVertexMain" },
                                  .depthStencil = { .depthTest = true, .depthWrite = true, .depthCompare = CompareOp::Always },
                                  .depthFormat = depthFormat,
                                  .label = "test.depthOnly" }
        );
        const Upscaler upscaler = *kit.device.createUpscaler(
            UpscalerDesc{ .input = { kInput, kInput }, .output = { kOutput, kOutput }, .depth = depthFormat, .label = "test.upscaler" }
        );
        std::vector<std::vector<std::byte>> outputs;
        for (uint32_t frame = 0; frame < kFrames; ++frame)
        {
            CAPTURE(frame);
            const Texture color = shadedColor(kit.device, [&](uint32_t x, uint32_t y) { return ((x + y + frame) & 1) != 0 ? kLight : kDark; });
            const Texture motion = *kit.device.createTexture(
                TextureDesc{ .format = Format::RG16Float,
                             .extent = { kInput, kInput, 1 },
                             .usage = TextureUsage::Sampled | TextureUsage::RenderTarget,
                             .label = "test.motion" },
                std::array{ TextureData{ motionBytes, uint64_t{ kInput } * 4, 0 } }
            );
            const Texture depth = make(kit.device, depthFormat, kInput, kInput, TextureUsage::Sampled | TextureUsage::DepthStencil, "test.depth");
            CommandList list = kit.list(QueueType::Graphics, "test.depthFormat");
            transition(list, depth, Layout::Undefined, Layout::DepthStencilWrite, Stage::None, Access::None, Stage::DepthStencil, Access::DepthStencilWrite);
            list.beginRenderPass(
                RenderPassDesc{
                    .depth = DepthAttachment{ .texture = depth, .load = LoadOp::Clear, .clearDepth = 0.8f, .stencilLoad = LoadOp::Clear, .clearStencil = 0x5A },
                    .area = { 0, 0, kInput, kInput },
                    .label = "test.depthPass" }
            );
            list.setPipeline(pipeline);
            list.setViewport(Viewport{ .width = static_cast<float>(kInput), .height = static_cast<float>(kInput) });
            list.setScissor(Rect2D{ nearLeft ? 0 : kInput / 2, 0, kInput / 2, kInput });
            list.draw(3);
            list.endRenderPass();
            transition(
                list,
                depth,
                Layout::DepthStencilWrite,
                Layout::ShaderRead,
                Stage::DepthStencil,
                Access::DepthStencilWrite,
                Stage::ComputeShader,
                Access::ShaderRead
            );
            const Texture output = makeOutput(kit.device, list);
            list.upscale(
                upscaler, UpscaleParams{ .color = color, .depth = depth, .motion = motion, .output = output, .reset = frame == 0, .label = "test.upscale" }
            );
            readyToRead(list, output);
            kit.run(std::array{ list });
            outputs.push_back(kit.readTexture(output, TextureRegion{ .extent = { kOutput, kOutput, 1 } }, Format::RGBA16Float));
            kit.nextFrame();
        }
        kit.device.destroy(upscaler);
        return outputs;
    };
    const std::vector<std::vector<std::byte>> plain = run(Format::D32Float, true);
    const std::vector<std::vector<std::byte>> stenciled = run(Format::D32FloatS8, true);
    const std::vector<std::vector<std::byte>> mirrored = run(Format::D32Float, false);
    uint32_t differing = 0;
    for (uint32_t frame = 0; frame < kFrames; ++frame)
    {
        CAPTURE(frame);
        CHECK(stenciled[frame] == plain[frame]);
        differing += mirrored[frame] == plain[frame] ? 0 : 1;
    }
    // The comparison can fail: the depth with the other half nearer gives other outputs.
    INFO("frames where the other half nearer changes the output: " << differing);
    CHECK(differing > 0u);
}
