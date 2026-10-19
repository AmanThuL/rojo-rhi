//----------------------------------------------------------------------------------------------------------------------
/// @file UpscaleConformanceTests.cpp
/// @brief Observes `upscale` on a GPU: uniform inputs reconstruct to their color at every output texel, with and without the optional inputs and across frames.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Support/Conformance.h"
#include "Support/GpuKit.h"

#include <catch2/catch_test_macros.hpp>

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
