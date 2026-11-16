#include "Common/Example.h"
#include "Shaders.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

using namespace rojoRHI;
using example::check;

struct alignas(16) Mat4
{
    float r[4][4];
};

struct Instance
{
    float positionScale[4];
    float color[4];
};

struct MeshVertex
{
    float position[4];
    float normal[4];
};

struct CullRoot
{
    Mat4 viewProj;
    Addr instances;
    Addr records;
    Addr count;
    uint32_t instanceCount;
    uint32_t indexCount;
};

struct DrawRoot
{
    Mat4 viewProj;
    Addr instances;
    Addr vertices;
};

constexpr uint32_t kGrid = 128;
constexpr uint32_t kInstanceCount = kGrid * kGrid;

Mat4 viewProjection(const float eye[3], const float forward[3], float aspect)
{
    const float f = 1.0f / std::tan(0.5f * 1.05f);
    const float nearPlane = 0.1f;
    const float side[3] = { -forward[2], 0.0f, forward[0] };
    const float sideLength = std::sqrt(side[0] * side[0] + side[2] * side[2]);
    const float s[3] = { side[0] / sideLength, 0.0f, side[2] / sideLength };
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

void buildCube(std::vector<MeshVertex>& vertices, std::vector<uint16_t>& indices)
{
    const float normals[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
    const float corners[4][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };
    for (const auto& n : normals)
    {
        const float u[3] = { n[1], n[2], n[0] };
        const float v[3] = { n[1] * u[2] - n[2] * u[1], n[2] * u[0] - n[0] * u[2], n[0] * u[1] - n[1] * u[0] };
        const auto base = uint16_t(vertices.size());
        for (const auto& c : corners)
        {
            vertices.push_back(
                { { 0.5f * (n[0] + c[0] * u[0] + c[1] * v[0]), 0.5f * (n[1] + c[0] * u[1] + c[1] * v[1]), 0.5f * (n[2] + c[0] * u[2] + c[1] * v[2]), 1.0f },
                  { n[0], n[1], n[2], 0.0f } }
            );
        }
        for (const int i : { 0, 1, 2, 0, 2, 3 })
        {
            indices.push_back(uint16_t(base + i));
        }
    }
}

std::vector<Instance> buildField()
{
    std::vector<Instance> instances;
    for (uint32_t z = 0; z < kGrid; ++z)
    {
        for (uint32_t x = 0; x < kGrid; ++x)
        {
            const float px = (float(x) - 0.5f * float(kGrid)) * 1.5f;
            const float pz = (float(z) - 0.5f * float(kGrid)) * 1.5f;
            const float py = 1.5f * std::sin(px * 0.15f) * std::cos(pz * 0.15f);
            const float hue = std::atan2(pz, px);
            instances.push_back(
                { { px, py, pz, 0.5f + 0.4f * std::fabs(std::sin(px * pz)) },
                  { 0.55f + 0.45f * std::cos(hue), 0.55f + 0.45f * std::cos(hue - 2.1f), 0.55f + 0.45f * std::cos(hue + 2.1f), 1.0f } }
            );
        }
    }
    return instances;
}

int main()
{
    example::Window window("RojoRHI - GPU-Driven Culling", 1280, 720);

    Device device = check(createDevice({ .validation = example::kValidation, .scopeTimings = true, .label = "culling" }), "createDevice");
    Queue graphics = device.queue(QueueType::Graphics);
    Swapchain swapchain = check(device.createSwapchain({ .nativeSurface = window.surface(), .label = "culling" }), "createSwapchain");

    ShaderLibrary library = check(device.loadShaderLibrary(embedded::kGpuDrivenCullingLibrary, "culling"), "loadShaderLibrary");
    Pipeline cull = check(device.createPipeline(ComputePipelineDesc{ .compute = { library, "cull" }, .label = "cull" }), "createPipeline");
    const std::array colors{ ColorTarget{ .format = Format::BGRA8Unorm } };
    Pipeline scene = check(
        device.createPipeline(
            GraphicsPipelineDesc{ .vertex = { library, "sceneVertex" },
                                  .fragment = { library, "sceneFragment" },
                                  .depthStencil = { .depthTest = true, .depthWrite = true },
                                  .colors = colors,
                                  .depthFormat = Format::D32Float,
                                  .label = "scene" }
        ),
        "createPipeline"
    );

    std::vector<MeshVertex> cubeVertices;
    std::vector<uint16_t> cubeIndices;
    buildCube(cubeVertices, cubeIndices);
    const std::vector<Instance> field = buildField();

    Buffer vertices = check(
        device.createBuffer({ .size = cubeVertices.size() * sizeof(MeshVertex), .label = "vertices" }, std::as_bytes(std::span(cubeVertices))), "createBuffer"
    );
    Buffer indices = check(
        device.createBuffer(
            { .size = cubeIndices.size() * sizeof(uint16_t), .usage = BufferUsage::Index, .label = "indices" }, std::as_bytes(std::span(cubeIndices))
        ),
        "createBuffer"
    );
    Buffer instances =
        check(device.createBuffer({ .size = field.size() * sizeof(Instance), .label = "instances" }, std::as_bytes(std::span(field))), "createBuffer");
    Buffer records = check(
        device.createBuffer(
            { .size = kInstanceCount * sizeof(DrawIndexedIndirectArgs), .usage = BufferUsage::ShaderWrite | BufferUsage::IndirectArgs, .label = "records" }
        ),
        "createBuffer"
    );
    Buffer count = check(
        device.createBuffer({ .size = sizeof(uint32_t), .usage = BufferUsage::ShaderWrite | BufferUsage::IndirectArgs, .label = "count" }), "createBuffer"
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
    uint64_t frame = 0;
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

        const float time = float(window.time()) * 0.3f;
        const float eye[3] = { 0.0f, 6.0f, 0.0f };
        const float forward[3] = { std::cos(time) * 0.97f, -0.25f, std::sin(time) * 0.97f };
        const Mat4 viewProj = viewProjection(eye, forward, float(extent.width) / float(extent.height));

        CommandList list = device.acquireCommandList(QueueType::Graphics, "frame");

        const std::array reuse{ GlobalBarrier{ .srcStage = Stage::IndirectArgs, .dstStage = Stage::Copy | Stage::ComputeShader } };
        list.barrier(reuse);

        list.beginScope("cull");
        list.fillBuffer(count, 0, sizeof(uint32_t), 0);
        const std::array toCull{ BufferBarrier{ .buffer = count,
                                                .srcStage = Stage::Copy,
                                                .srcAccess = Access::CopyWrite,
                                                .dstStage = Stage::ComputeShader,
                                                .dstAccess = Access::ShaderRead | Access::ShaderWrite } };
        list.barrier({}, toCull);

        list.setPipeline(cull);
        list.pushRoot(
            CullRoot{ .viewProj = viewProj,
                      .instances = device.address(instances),
                      .records = device.address(records),
                      .count = device.address(count),
                      .instanceCount = kInstanceCount,
                      .indexCount = uint32_t(cubeIndices.size()) }
        );
        const std::array cullAccesses{ ResourceAccess{ .buffer = records, .stage = Stage::ComputeShader, .access = Access::ShaderWrite },
                                       ResourceAccess{ .buffer = count, .stage = Stage::ComputeShader, .access = Access::ShaderRead | Access::ShaderWrite } };
        list.declareAccess(cullAccesses);
        list.dispatch((kInstanceCount + 63) / 64, 1, 1);
        list.endScope();

        const std::array toDraw{ BufferBarrier{ .buffer = records,
                                                .srcStage = Stage::ComputeShader,
                                                .srcAccess = Access::ShaderWrite,
                                                .dstStage = Stage::IndirectArgs,
                                                .dstAccess = Access::IndirectArgs },
                                 BufferBarrier{ .buffer = count,
                                                .srcStage = Stage::ComputeShader,
                                                .srcAccess = Access::ShaderWrite,
                                                .dstStage = Stage::IndirectArgs,
                                                .dstAccess = Access::IndirectArgs } };
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
        list.barrier({}, toDraw, toRender);

        const std::array attachments{ ColorAttachment{ .texture = image, .load = LoadOp::Clear, .clear = { 0.55f, 0.7f, 0.9f, 1.0f } } };
        list.beginRenderPass(
            { .colors = attachments,
              .depth = { .texture = depth, .load = LoadOp::Clear, .store = StoreOp::DontCare, .clearDepth = 0.0f },
              .area = { 0, 0, extent.width, extent.height },
              .label = "scene" }
        );
        list.setPipeline(scene);
        list.setViewport({ .width = float(extent.width), .height = float(extent.height) });
        list.setScissor({ 0, 0, extent.width, extent.height });
        list.pushRoot(DrawRoot{ .viewProj = viewProj, .instances = device.address(instances), .vertices = device.address(vertices) });
        list.drawIndexedIndirect(indices, IndexType::UInt16, device.address(records), kInstanceCount, device.address(count));
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
    device.destroy(count);
    device.destroy(records);
    device.destroy(instances);
    device.destroy(indices);
    device.destroy(vertices);
    device.destroy(scene);
    device.destroy(cull);
    device.destroy(library);
    device.destroy(swapchain);
    destroyDevice(device);
    return 0;
}
