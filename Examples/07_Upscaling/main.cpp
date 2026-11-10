#include "Common/Example.h"
#include "Shaders.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>

using namespace rojoRHI;
using example::check;

struct alignas(16) Mat4
{
    float r[4][4];
};

struct SceneRoot
{
    Mat4 viewProj;
    Mat4 previousViewProj;
    float jitter[2];
    float time;
    float previousTime;
    float inputSize[2];
    uint32_t pad0;
    uint32_t pad1;
};

struct PresentRoot
{
    uint32_t image;
    uint32_t sampler;
};

struct Targets
{
    Extent2D input;
    Extent2D output;
    Texture color;
    Texture motion;
    Texture depth;
    Texture upscaled;
    ViewIndex upscaledView;
    Upscaler upscaler;
};

constexpr uint32_t kCubeCount = 24 * 24;

Mat4 viewProjection(const float eye[3], float aspect)
{
    const float f = 1.0f / std::tan(0.5f * 0.9f);
    const float nearPlane = 0.1f;
    const float length = std::sqrt(eye[0] * eye[0] + eye[1] * eye[1] + eye[2] * eye[2]);
    const float forward[3] = { -eye[0] / length, -eye[1] / length, -eye[2] / length };
    const float sideLength = std::sqrt(forward[0] * forward[0] + forward[2] * forward[2]);
    const float s[3] = { -forward[2] / sideLength, 0.0f, forward[0] / sideLength };
    const float u[3] = { s[1] * forward[2] - s[2] * forward[1], s[2] * forward[0] - s[0] * forward[2], s[0] * forward[1] - s[1] * forward[0] };
    const auto dot = [](const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };

    const Mat4 view{ { { s[0], s[1], s[2], -dot(s, eye) },
                       { u[0], u[1], u[2], -dot(u, eye) },
                       { -forward[0], -forward[1], -forward[2], dot(forward, eye) },
                       { 0.0f, 0.0f, 0.0f, 1.0f } } };
    const Mat4 projection{ { { f / aspect, 0.0f, 0.0f, 0.0f }, { 0.0f, f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f, nearPlane }, { 0.0f, 0.0f, -1.0f, 0.0f } } };

    Mat4 result{};
    for (int i = 0; i < 4; ++i)
    {
        for (int j = 0; j < 4; ++j)
        {
            for (int k = 0; k < 4; ++k)
            {
                result.r[i][j] += projection.r[i][k] * view.r[k][j];
            }
        }
    }
    return result;
}

float halton(uint32_t index, uint32_t base)
{
    float fraction = 1.0f;
    float result = 0.0f;
    for (; index > 0; index /= base)
    {
        fraction /= float(base);
        result += fraction * float(index % base);
    }
    return result;
}

Targets createTargets(Device device, Extent2D output, float scale)
{
    const Extent2D input{ std::max(1u, uint32_t(float(output.width) * scale)), std::max(1u, uint32_t(float(output.height) * scale)) };
    const auto texture = [&](Format format, Extent2D extent, TextureUsage usage, const char* label) {
        return check(device.createTexture({ .format = format, .extent = { extent.width, extent.height, 1 }, .usage = usage, .label = label }), "createTexture");
    };

    Targets targets{ .input = input, .output = output };
    targets.color = texture(Format::RGBA16Float, input, TextureUsage::RenderTarget | TextureUsage::Sampled, "color");
    targets.motion = texture(Format::RG16Float, input, TextureUsage::RenderTarget | TextureUsage::Sampled, "motion");
    targets.depth = texture(Format::D32Float, input, TextureUsage::DepthStencil | TextureUsage::Sampled, "depth");
    targets.upscaled = texture(Format::RGBA16Float, output, TextureUsage::Storage | TextureUsage::RenderTarget | TextureUsage::Sampled, "upscaled");
    targets.upscaledView = check(device.createView(targets.upscaled, {}), "createView");
    targets.upscaler = check(
        device.createUpscaler({ .input = input, .output = output, .flags = UpscalerFlags::HDR | UpscalerFlags::ReversedDepth, .label = "upscaler" }),
        "createUpscaler"
    );
    return targets;
}

void destroyTargets(Device device, const Targets& targets)
{
    device.destroy(targets.upscaler);
    device.destroy(targets.upscaledView);
    device.destroy(targets.upscaled);
    device.destroy(targets.depth);
    device.destroy(targets.motion);
    device.destroy(targets.color);
}

int main()
{
    example::Window window("RojoRHI - Temporal Upscaling", 1280, 720);

    Device device = check(createDevice({ .validation = example::kValidation, .scopeTimings = true, .label = "upscaling" }), "createDevice");
    if (device.capabilities().upscalers.empty())
    {
        std::fprintf(stderr, "this device has no temporal upscaler\n");
        destroyDevice(device);
        return 1;
    }
    const float scale = std::max(0.5f, device.capabilities().upscalers[0].minInputScale);

    Queue graphics = device.queue(QueueType::Graphics);
    Swapchain swapchain = check(device.createSwapchain({ .nativeSurface = window.surface(), .label = "upscaling" }), "createSwapchain");

    ShaderLibrary library = check(device.loadShaderLibrary(embedded::kUpscalingLibrary, "upscaling"), "loadShaderLibrary");
    const std::array sceneColors{ ColorTarget{ .format = Format::RGBA16Float }, ColorTarget{ .format = Format::RG16Float } };
    Pipeline scene = check(
        device.createPipeline(
            GraphicsPipelineDesc{ .vertex = { library, "sceneVertex" },
                                  .fragment = { library, "sceneFragment" },
                                  .depthStencil = { .depthTest = true, .depthWrite = true },
                                  .colors = sceneColors,
                                  .depthFormat = Format::D32Float,
                                  .label = "scene" }
        ),
        "createPipeline"
    );
    const std::array presentColors{ ColorTarget{ .format = Format::BGRA8Unorm } };
    Pipeline present = check(
        device.createPipeline(
            GraphicsPipelineDesc{
                .vertex = { library, "presentVertex" }, .fragment = { library, "presentFragment" }, .colors = presentColors, .label = "present" }
        ),
        "createPipeline"
    );
    const SamplerIndex sampler = check(device.createSampler({ .address = SamplerAddress::ClampToEdge }), "createSampler");

    Targets targets = createTargets(device, window.extent(), scale);
    std::printf("rendering %ux%u, upscaling to %ux%u\n", targets.input.width, targets.input.height, targets.output.width, targets.output.height);

    Mat4 previousViewProj{};
    float previousTime = 0.0f;
    bool reset = true;
    uint64_t frame = 0;
    while (window.poll())
    {
        const Extent2D size = window.extent();
        if (size.width == 0 || size.height == 0)
        {
            continue;
        }
        if (size.width != targets.output.width || size.height != targets.output.height)
        {
            swapchain.resize(size);
            destroyTargets(device, targets);
            targets = createTargets(device, size, scale);
            reset = true;
        }

        device.beginFrame();
        if (++frame % 120 == 0)
        {
            for (const ScopeTiming& timing : device.scopeTimings())
            {
                std::printf("%.*s %.3f ms   ", int(timing.label.size()), timing.label.data(), timing.gpuMilliseconds);
            }
            std::printf("\n");
        }

        const Texture image = swapchain.acquire();
        if (image == Texture{})
        {
            device.endFrame();
            continue;
        }

        const float time = float(window.time());
        const float eye[3] = { 16.0f * std::cos(0.1f * time), 7.0f, 16.0f * std::sin(0.1f * time) };
        const Mat4 viewProj = viewProjection(eye, float(targets.output.width) / float(targets.output.height));
        const float jitterX = halton(uint32_t(frame % 8) + 1, 2) - 0.5f;
        const float jitterY = halton(uint32_t(frame % 8) + 1, 3) - 0.5f;
        if (reset)
        {
            previousViewProj = viewProj;
            previousTime = time;
        }

        CommandList list = device.acquireCommandList(QueueType::Graphics, "frame");
        const std::array toScene{ TextureBarrier{ .texture = targets.color,
                                                  .srcStage = Stage::ComputeShader,
                                                  .dstStage = Stage::ColorAttachment,
                                                  .dstAccess = Access::ColorAttachmentWrite,
                                                  .oldLayout = Layout::Undefined,
                                                  .newLayout = Layout::ColorAttachment },
                                  TextureBarrier{ .texture = targets.motion,
                                                  .srcStage = Stage::ComputeShader,
                                                  .dstStage = Stage::ColorAttachment,
                                                  .dstAccess = Access::ColorAttachmentWrite,
                                                  .oldLayout = Layout::Undefined,
                                                  .newLayout = Layout::ColorAttachment },
                                  TextureBarrier{ .texture = targets.depth,
                                                  .srcStage = Stage::ComputeShader,
                                                  .dstStage = Stage::DepthStencil,
                                                  .dstAccess = Access::DepthStencilRead | Access::DepthStencilWrite,
                                                  .oldLayout = Layout::Undefined,
                                                  .newLayout = Layout::DepthStencilWrite } };
        list.barrier({}, {}, toScene);

        const std::array sceneAttachments{ ColorAttachment{ .texture = targets.color, .load = LoadOp::Clear, .clear = { 0.02f, 0.025f, 0.04f, 1.0f } },
                                           ColorAttachment{ .texture = targets.motion, .load = LoadOp::Clear } };
        list.beginRenderPass(
            { .colors = sceneAttachments,
              .depth = { .texture = targets.depth, .load = LoadOp::Clear, .clearDepth = 0.0f },
              .area = { 0, 0, targets.input.width, targets.input.height },
              .label = "scene" }
        );
        list.setPipeline(scene);
        list.setViewport({ .width = float(targets.input.width), .height = float(targets.input.height) });
        list.setScissor({ 0, 0, targets.input.width, targets.input.height });
        list.pushRoot(
            SceneRoot{ .viewProj = viewProj,
                       .previousViewProj = previousViewProj,
                       .jitter = { 2.0f * jitterX / float(targets.input.width), -2.0f * jitterY / float(targets.input.height) },
                       .time = time,
                       .previousTime = previousTime,
                       .inputSize = { float(targets.input.width), float(targets.input.height) } }
        );
        list.draw(36, kCubeCount);
        list.endRenderPass();

        const std::array toUpscale{ TextureBarrier{ .texture = targets.color,
                                                    .srcStage = Stage::ColorAttachment,
                                                    .srcAccess = Access::ColorAttachmentWrite,
                                                    .dstStage = Stage::ComputeShader,
                                                    .dstAccess = Access::ShaderRead,
                                                    .oldLayout = Layout::ColorAttachment,
                                                    .newLayout = Layout::ShaderRead },
                                    TextureBarrier{ .texture = targets.motion,
                                                    .srcStage = Stage::ColorAttachment,
                                                    .srcAccess = Access::ColorAttachmentWrite,
                                                    .dstStage = Stage::ComputeShader,
                                                    .dstAccess = Access::ShaderRead,
                                                    .oldLayout = Layout::ColorAttachment,
                                                    .newLayout = Layout::ShaderRead },
                                    TextureBarrier{ .texture = targets.depth,
                                                    .srcStage = Stage::DepthStencil,
                                                    .srcAccess = Access::DepthStencilWrite,
                                                    .dstStage = Stage::ComputeShader,
                                                    .dstAccess = Access::ShaderRead,
                                                    .oldLayout = Layout::DepthStencilWrite,
                                                    .newLayout = Layout::ShaderRead },
                                    TextureBarrier{ .texture = targets.upscaled,
                                                    .srcStage = Stage::FragmentShader,
                                                    .dstStage = Stage::ComputeShader,
                                                    .dstAccess = Access::ShaderWrite,
                                                    .oldLayout = Layout::Undefined,
                                                    .newLayout = Layout::ShaderWrite } };
        list.barrier({}, {}, toUpscale);

        list.upscale(
            targets.upscaler,
            UpscaleParams{ .color = targets.color,
                           .depth = targets.depth,
                           .motion = targets.motion,
                           .output = targets.upscaled,
                           .jitterX = jitterX,
                           .jitterY = jitterY,
                           .reset = reset,
                           .label = "upscale" }
        );

        const std::array toPresentPass{ TextureBarrier{ .texture = targets.upscaled,
                                                        .srcStage = Stage::ComputeShader,
                                                        .srcAccess = Access::ShaderWrite,
                                                        .dstStage = Stage::FragmentShader,
                                                        .dstAccess = Access::ShaderRead,
                                                        .oldLayout = Layout::ShaderWrite,
                                                        .newLayout = Layout::ShaderRead },
                                        TextureBarrier{ .texture = image,
                                                        .dstStage = Stage::ColorAttachment,
                                                        .dstAccess = Access::ColorAttachmentWrite,
                                                        .oldLayout = Layout::Undefined,
                                                        .newLayout = Layout::ColorAttachment } };
        list.barrier({}, {}, toPresentPass);

        const std::array presentAttachments{ ColorAttachment{ .texture = image, .load = LoadOp::DontCare } };
        list.beginRenderPass({ .colors = presentAttachments, .area = { 0, 0, targets.output.width, targets.output.height }, .label = "tonemap" });
        list.setPipeline(present);
        list.setViewport({ .width = float(targets.output.width), .height = float(targets.output.height) });
        list.setScissor({ 0, 0, targets.output.width, targets.output.height });
        list.pushRoot(PresentRoot{ .image = uint32_t(targets.upscaledView), .sampler = uint32_t(sampler) });
        const std::array presentAccesses{ ResourceAccess{ .texture = targets.upscaled, .stage = Stage::FragmentShader, .access = Access::ShaderRead } };
        list.declareAccess(presentAccesses);
        list.draw(3);
        list.endRenderPass();

        const std::array toPresent{ TextureBarrier{ .texture = image,
                                                    .srcStage = Stage::ColorAttachment,
                                                    .srcAccess = Access::ColorAttachmentWrite,
                                                    .oldLayout = Layout::ColorAttachment,
                                                    .newLayout = Layout::Present } };
        list.barrier({}, {}, toPresent);
        list.end();

        graphics.submit(std::array{ list });
        graphics.present(swapchain);
        device.endFrame();

        previousViewProj = viewProj;
        previousTime = time;
        reset = false;
    }

    destroyTargets(device, targets);
    device.destroy(sampler);
    device.destroy(present);
    device.destroy(scene);
    device.destroy(library);
    device.destroy(swapchain);
    destroyDevice(device);
    return 0;
}
