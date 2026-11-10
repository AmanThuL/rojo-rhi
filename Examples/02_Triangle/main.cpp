#include "Common/Example.h"
#include "Shaders.h"

#include <array>
#include <span>

using namespace rojoRHI;
using example::check;

struct Vertex
{
    float position[4];
    float color[4];
};

struct TriangleRoot
{
    Addr vertices;
    float angle;
    float aspect;
};

int main()
{
    example::Window window("RojoRHI - Triangle", 1280, 720);

    Device device = check(createDevice({ .validation = example::kValidation, .label = "triangle" }), "createDevice");
    Queue graphics = device.queue(QueueType::Graphics);
    Swapchain swapchain = check(device.createSwapchain({ .nativeSurface = window.surface(), .label = "triangle" }), "createSwapchain");

    ShaderLibrary library = check(device.loadShaderLibrary(embedded::kTriangleLibrary, "triangle"), "loadShaderLibrary");
    const std::array colors{ ColorTarget{ .format = Format::BGRA8Unorm } };
    Pipeline pipeline = check(
        device.createPipeline(
            GraphicsPipelineDesc{ .vertex = { library, "triangleVertex" }, .fragment = { library, "triangleFragment" }, .colors = colors, .label = "triangle" }
        ),
        "createPipeline"
    );

    const std::array vertices{
        Vertex{ { 0.0f, 0.7f, 0.0f, 1.0f }, { 1.0f, 0.3f, 0.2f, 1.0f } },
        Vertex{ { -0.6f, -0.45f, 0.0f, 1.0f }, { 0.2f, 1.0f, 0.4f, 1.0f } },
        Vertex{ { 0.6f, -0.45f, 0.0f, 1.0f }, { 0.3f, 0.4f, 1.0f, 1.0f } },
    };

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
        list.beginRenderPass({ .colors = attachments, .area = { 0, 0, extent.width, extent.height }, .label = "triangle" });
        list.setPipeline(pipeline);
        list.setViewport({ .width = float(extent.width), .height = float(extent.height) });
        list.setScissor({ 0, 0, extent.width, extent.height });
        list.pushRoot(
            TriangleRoot{ .vertices = list.uploadFrameData(std::as_bytes(std::span(vertices))),
                          .angle = float(window.time()),
                          .aspect = float(extent.width) / float(extent.height) }
        );
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
    }

    device.destroy(pipeline);
    device.destroy(library);
    device.destroy(swapchain);
    destroyDevice(device);
    return 0;
}
