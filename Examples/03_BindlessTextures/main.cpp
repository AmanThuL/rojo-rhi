#include "Common/Example.h"
#include "Shaders.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

using namespace rojoRHI;
using example::check;

struct Instance
{
    float offset[2];
    float scale;
    uint32_t texture;
};

struct BindlessRoot
{
    Addr instances;
    uint32_t sampler;
    float time;
    float aspect;
    uint32_t pad;
};

constexpr uint32_t kGrid = 16;
constexpr uint32_t kTextureCount = kGrid * kGrid;
constexpr uint32_t kTextureSize = 64;

uint32_t pack(float r, float g, float b)
{
    return uint32_t(r * 255.0f) | uint32_t(g * 255.0f) << 8 | uint32_t(b * 255.0f) << 16 | 0xFF000000u;
}

std::vector<uint32_t> pattern(uint32_t index)
{
    const float hue = float(index) / float(kTextureCount) * 6.2831853f;
    const float r = 0.5f + 0.5f * std::cos(hue);
    const float g = 0.5f + 0.5f * std::cos(hue - 2.094f);
    const float b = 0.5f + 0.5f * std::cos(hue + 2.094f);
    const uint32_t cells = 2 + index % 7;

    std::vector<uint32_t> texels(kTextureSize * kTextureSize);
    for (uint32_t y = 0; y < kTextureSize; ++y)
    {
        for (uint32_t x = 0; x < kTextureSize; ++x)
        {
            const bool dark = (x * cells / kTextureSize + y * cells / kTextureSize) % 2 == 0;
            const float shade = dark ? 0.35f : 1.0f;
            texels[y * kTextureSize + x] = pack(r * shade, g * shade, b * shade);
        }
    }
    return texels;
}

int main()
{
    example::Window window("RojoRHI - Bindless Textures", 1280, 720);

    Device device = check(createDevice({ .validation = example::kValidation, .label = "bindless" }), "createDevice");
    Queue graphics = device.queue(QueueType::Graphics);
    Swapchain swapchain = check(device.createSwapchain({ .nativeSurface = window.surface(), .label = "bindless" }), "createSwapchain");

    ShaderLibrary library = check(device.loadShaderLibrary(embedded::kBindlessTexturesLibrary, "bindless"), "loadShaderLibrary");
    const std::array colors{ ColorTarget{ .format = Format::BGRA8Unorm } };
    Pipeline pipeline = check(
        device.createPipeline(
            GraphicsPipelineDesc{ .vertex = { library, "bindlessVertex" }, .fragment = { library, "bindlessFragment" }, .colors = colors, .label = "bindless" }
        ),
        "createPipeline"
    );

    std::vector<Texture> textures;
    std::vector<ViewIndex> views;
    std::vector<Instance> instances;
    for (uint32_t i = 0; i < kTextureCount; ++i)
    {
        const std::vector<uint32_t> texels = pattern(i);
        const std::array initial{ TextureData{ .bytes = std::as_bytes(std::span(texels)), .bytesPerRow = kTextureSize * 4 } };
        const Texture texture = check(
            device.createTexture(
                { .format = Format::RGBA8Unorm, .extent = { kTextureSize, kTextureSize, 1 }, .usage = TextureUsage::Sampled, .label = "pattern" }, initial
            ),
            "createTexture"
        );
        const ViewIndex view = check(device.createView(texture, {}), "createView");
        textures.push_back(texture);
        views.push_back(view);

        const float cell = 1.8f / float(kGrid);
        instances.push_back(
            { .offset = { -0.9f + cell * (float(i % kGrid) + 0.5f), -0.9f + cell * (float(i / kGrid) + 0.5f) },
              .scale = cell * 0.4f,
              .texture = uint32_t(view) }
        );
    }

    const SamplerIndex sampler = check(device.createSampler({ .filter = SamplerFilter::Nearest, .address = SamplerAddress::ClampToEdge }), "createSampler");
    Buffer instanceBuffer =
        check(device.createBuffer({ .size = instances.size() * sizeof(Instance), .label = "instances" }, std::as_bytes(std::span(instances))), "createBuffer");

    Extent2D extent = window.extent();
    while (window.poll())
    {
        const Extent2D size = window.extent();
        if (size.width == 0 || size.height == 0)
        {
            continue;
        }
        if (size.width != extent.width || size.height != extent.height)
        {
            swapchain.resize(size);
            extent = size;
        }

        device.beginFrame();
        const Texture image = swapchain.acquire();
        if (image == Texture{})
        {
            device.endFrame();
            continue;
        }

        CommandList list = device.acquireCommandList(QueueType::Graphics, "frame");
        const std::array toRender{ TextureBarrier{ .texture = image,
                                                   .dstStage = Stage::ColorAttachment,
                                                   .dstAccess = Access::ColorAttachmentWrite,
                                                   .oldLayout = Layout::Undefined,
                                                   .newLayout = Layout::ColorAttachment } };
        list.barrier({}, {}, toRender);

        const std::array attachments{ ColorAttachment{ .texture = image, .load = LoadOp::Clear, .clear = { 0.02f, 0.02f, 0.03f, 1.0f } } };
        list.beginRenderPass({ .colors = attachments, .area = { 0, 0, extent.width, extent.height }, .label = "grid" });
        list.setPipeline(pipeline);
        list.setViewport({ .width = float(extent.width), .height = float(extent.height) });
        list.setScissor({ 0, 0, extent.width, extent.height });
        list.pushRoot(
            BindlessRoot{ .instances = device.address(instanceBuffer),
                          .sampler = uint32_t(sampler),
                          .time = float(window.time()),
                          .aspect = float(extent.width) / float(extent.height) }
        );
        list.draw(6, kTextureCount);
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
    }

    for (uint32_t i = 0; i < kTextureCount; ++i)
    {
        device.destroy(views[i]);
        device.destroy(textures[i]);
    }
    device.destroy(sampler);
    device.destroy(instanceBuffer);
    device.destroy(pipeline);
    device.destroy(library);
    device.destroy(swapchain);
    destroyDevice(device);
    return 0;
}
