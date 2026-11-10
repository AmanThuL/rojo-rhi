#include "Common/Example.h"
#include "Shaders.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

using namespace rojoRHI;
using example::check;

struct Particle
{
    float position[2];
    float velocity[2];
};

struct SimulateRoot
{
    Addr source;
    Addr target;
    float time;
    float dt;
    uint32_t count;
    uint32_t pad;
};

struct DrawRoot
{
    Addr particles;
    float aspect;
    float size;
};

constexpr uint32_t kParticleCount = 1u << 17;

std::vector<Particle> seed()
{
    std::vector<Particle> particles(kParticleCount);
    uint32_t state = 1;
    const auto random = [&] {
        state = state * 1664525u + 1013904223u;
        return float(state >> 8) / float(1u << 24);
    };
    for (Particle& particle : particles)
    {
        const float angle = random() * 6.2831853f;
        const float radius = 0.2f + 0.7f * std::sqrt(random());
        particle = { { radius * std::cos(angle), radius * std::sin(angle) }, { -0.3f * std::sin(angle), 0.3f * std::cos(angle) } };
    }
    return particles;
}

int main()
{
    example::Window window("RojoRHI - Async Compute", 1280, 720);

    Device device = check(createDevice({ .validation = example::kValidation, .label = "async-compute" }), "createDevice");
    Queue graphics = device.queue(QueueType::Graphics);
    Queue compute = device.queue(QueueType::Compute);
    Swapchain swapchain = check(device.createSwapchain({ .nativeSurface = window.surface(), .label = "async-compute" }), "createSwapchain");

    ShaderLibrary library = check(device.loadShaderLibrary(embedded::kAsyncComputeLibrary, "async-compute"), "loadShaderLibrary");
    Pipeline simulate = check(device.createPipeline(ComputePipelineDesc{ .compute = { library, "simulate" }, .label = "simulate" }), "createPipeline");
    const std::array colors{ ColorTarget{ .format = Format::BGRA8Unorm,
                                          .blend = { .enable = true, .srcColor = BlendFactor::One, .dstColor = BlendFactor::One } } };
    Pipeline particles = check(
        device.createPipeline(
            GraphicsPipelineDesc{ .vertex = { library, "particleVertex" }, .fragment = { library, "particleFragment" }, .colors = colors, .label = "particles" }
        ),
        "createPipeline"
    );

    const std::vector<Particle> initial = seed();
    const BufferDesc particleDesc{ .size = kParticleCount * sizeof(Particle), .usage = BufferUsage::ShaderWrite, .label = "particles" };
    const std::array buffers{ check(device.createBuffer(particleDesc, std::as_bytes(std::span(initial))), "createBuffer"),
                              check(device.createBuffer(particleDesc), "createBuffer") };

    Fence simulated = check(device.createFence(0, "simulated"), "createFence");
    Fence drawn = check(device.createFence(0, "drawn"), "createFence");

    Extent2D extent = window.extent();
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
            extent = size;
        }

        device.beginFrame();
        const Texture image = swapchain.acquire();
        if (image == Texture{})
        {
            device.endFrame();
            continue;
        }

        ++frame;
        const Buffer previous = buffers[(frame - 1) % 2];
        const Buffer next = buffers[frame % 2];

        CommandList step = device.acquireCommandList(QueueType::Compute, "simulate");
        const std::array afterLastStep{ GlobalBarrier{ .srcStage = Stage::ComputeShader,
                                                       .srcAccess = Access::ShaderWrite,
                                                       .dstStage = Stage::ComputeShader,
                                                       .dstAccess = Access::ShaderRead | Access::ShaderWrite } };
        step.barrier(afterLastStep);
        step.setPipeline(simulate);
        step.pushRoot(
            SimulateRoot{
                .source = device.address(previous), .target = device.address(next), .time = float(window.time()), .dt = 1.0f / 60.0f, .count = kParticleCount }
        );
        const std::array stepAccesses{ ResourceAccess{ .buffer = previous, .stage = Stage::ComputeShader, .access = Access::ShaderRead },
                                       ResourceAccess{ .buffer = next, .stage = Stage::ComputeShader, .access = Access::ShaderWrite } };
        step.declareAccess(stepAccesses);
        step.dispatch(kParticleCount / 256, 1, 1);
        step.end();

        compute.submit(std::array{ step }, std::array{ FenceValue{ drawn, frame - 1 } }, std::array{ FenceValue{ simulated, frame } });

        CommandList draw = device.acquireCommandList(QueueType::Graphics, "draw");
        const std::array toRender{ TextureBarrier{ .texture = image,
                                                   .dstStage = Stage::ColorAttachment,
                                                   .dstAccess = Access::ColorAttachmentRead | Access::ColorAttachmentWrite,
                                                   .oldLayout = Layout::Undefined,
                                                   .newLayout = Layout::ColorAttachment } };
        draw.barrier({}, {}, toRender);

        const std::array attachments{ ColorAttachment{ .texture = image, .load = LoadOp::Clear, .clear = { 0.0f, 0.0f, 0.0f, 1.0f } } };
        draw.beginRenderPass({ .colors = attachments, .area = { 0, 0, extent.width, extent.height }, .label = "particles" });
        draw.setPipeline(particles);
        draw.setViewport({ .width = float(extent.width), .height = float(extent.height) });
        draw.setScissor({ 0, 0, extent.width, extent.height });
        draw.pushRoot(DrawRoot{ .particles = device.address(previous), .aspect = float(extent.width) / float(extent.height), .size = 0.006f });
        const std::array drawAccesses{ ResourceAccess{ .buffer = previous, .stage = Stage::VertexShader, .access = Access::ShaderRead } };
        draw.declareAccess(drawAccesses);
        draw.draw(6, kParticleCount);
        draw.endRenderPass();

        const std::array toPresent{ TextureBarrier{ .texture = image,
                                                    .srcStage = Stage::ColorAttachment,
                                                    .srcAccess = Access::ColorAttachmentWrite,
                                                    .oldLayout = Layout::ColorAttachment,
                                                    .newLayout = Layout::Present } };
        draw.barrier({}, {}, toPresent);
        draw.end();

        graphics.submit(std::array{ draw }, std::array{ FenceValue{ simulated, frame - 1 } }, std::array{ FenceValue{ drawn, frame } });
        graphics.present(swapchain);
        graphics.submit({}, std::array{ FenceValue{ simulated, frame } });
        device.endFrame();
    }

    device.destroy(drawn);
    device.destroy(simulated);
    device.destroy(buffers[1]);
    device.destroy(buffers[0]);
    device.destroy(particles);
    device.destroy(simulate);
    device.destroy(library);
    device.destroy(swapchain);
    destroyDevice(device);
    return 0;
}
