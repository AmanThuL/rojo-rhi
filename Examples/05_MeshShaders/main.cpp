#include "Common/Example.h"
#include "Shaders.h"

#include <array>
#include <cmath>
#include <cstdint>

using namespace rojoRHI;
using example::check;

struct alignas(16) Mat4
{
    float r[4][4];
};

struct TerrainRoot
{
    Mat4 viewProj;
    float time;
    float patchSize;
    uint32_t patchesPerSide;
    uint32_t pad;
};

constexpr uint32_t kPatchesPerSide = 64;
constexpr uint32_t kTaskThreads = 32;
constexpr float kPatchSize = 7.0f;

Mat4 viewProjection(const float eye[3], const float target[3], float aspect)
{
    const float f = 1.0f / std::tan(0.5f * 1.05f);
    const float nearPlane = 0.1f;
    float forward[3] = { target[0] - eye[0], target[1] - eye[1], target[2] - eye[2] };
    const float forwardLength = std::sqrt(forward[0] * forward[0] + forward[1] * forward[1] + forward[2] * forward[2]);
    for (float& c : forward)
    {
        c /= forwardLength;
    }
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

int main()
{
    example::Window window("RojoRHI - Mesh Shaders", 1280, 720);

    Device device = check(createDevice({ .validation = example::kValidation, .label = "mesh-shaders" }), "createDevice");
    Queue graphics = device.queue(QueueType::Graphics);
    Swapchain swapchain = check(device.createSwapchain({ .nativeSurface = window.surface(), .label = "mesh-shaders" }), "createSwapchain");

    ShaderLibrary geometry = check(device.loadShaderLibrary(embedded::kMeshShadersLibrary, "geometry"), "loadShaderLibrary");
    ShaderLibrary shading = check(device.loadShaderLibrary(embedded::kShadingLibrary, "shading"), "loadShaderLibrary");
    const std::array colors{ ColorTarget{ .format = Format::BGRA8Unorm } };
    Pipeline terrain = check(
        device.createPipeline(
            GraphicsPipelineDesc{ .task = { geometry, "terrainTask" },
                                  .mesh = { geometry, "terrainMesh" },
                                  .fragment = { shading, "terrainFragment" },
                                  .depthStencil = { .depthTest = true, .depthWrite = true },
                                  .colors = colors,
                                  .depthFormat = Format::D32Float,
                                  .label = "terrain" }
        ),
        "createPipeline"
    );

    const auto createDepth = [&](Extent2D size) {
        return check(
            device.createTexture(
                { .format = Format::D32Float, .extent = { size.width, size.height, 1 }, .usage = TextureUsage::DepthStencil, .label = "depth" }
            ),
            "createTexture"
        );
    };

    Extent2D extent = window.extent();
    Texture depth = createDepth(extent);
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
            device.destroy(depth);
            depth = createDepth(size);
            extent = size;
        }

        device.beginFrame();
        const Texture image = swapchain.acquire();
        if (image == Texture{})
        {
            device.endFrame();
            continue;
        }

        const float time = float(window.time());
        const float eye[3] = { 60.0f * std::cos(0.15f * time), 22.0f, 60.0f * std::sin(0.15f * time) };
        const float target[3] = { 0.0f, 0.0f, 0.0f };

        CommandList list = device.acquireCommandList(QueueType::Graphics, "frame");
        const std::array toRender{ TextureBarrier{ .texture = image,
                                                   .dstStage = Stage::ColorAttachment,
                                                   .dstAccess = Access::ColorAttachmentWrite,
                                                   .oldLayout = Layout::Undefined,
                                                   .newLayout = Layout::ColorAttachment },
                                   TextureBarrier{ .texture = depth,
                                                   .srcStage = Stage::DepthStencil,
                                                   .dstStage = Stage::DepthStencil,
                                                   .dstAccess = Access::DepthStencilRead | Access::DepthStencilWrite,
                                                   .oldLayout = Layout::Undefined,
                                                   .newLayout = Layout::DepthStencilWrite } };
        list.barrier({}, {}, toRender);

        const std::array attachments{ ColorAttachment{ .texture = image, .load = LoadOp::Clear, .clear = { 0.6f, 0.75f, 0.95f, 1.0f } } };
        list.beginRenderPass(
            { .colors = attachments,
              .depth = { .texture = depth, .load = LoadOp::Clear, .store = StoreOp::DontCare, .clearDepth = 0.0f },
              .area = { 0, 0, extent.width, extent.height },
              .label = "terrain" }
        );
        list.setPipeline(terrain);
        list.setViewport({ .width = float(extent.width), .height = float(extent.height) });
        list.setScissor({ 0, 0, extent.width, extent.height });
        list.pushRoot(
            TerrainRoot{ .viewProj = viewProjection(eye, target, float(extent.width) / float(extent.height)),
                         .time = time,
                         .patchSize = kPatchSize,
                         .patchesPerSide = kPatchesPerSide }
        );
        list.drawMeshTasks(kPatchesPerSide * kPatchesPerSide / kTaskThreads, 1, 1);
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

    device.destroy(depth);
    device.destroy(terrain);
    device.destroy(shading);
    device.destroy(geometry);
    device.destroy(swapchain);
    destroyDevice(device);
    return 0;
}
