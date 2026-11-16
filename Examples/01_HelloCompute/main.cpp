#include "Common/Example.h"
#include "Shaders.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <vector>

using namespace rojoRHI;
using example::check;

struct SaxpyRoot
{
    Addr x;
    Addr y;
    Addr result;
    float a;
    uint32_t count;
};

int main()
{
    constexpr uint32_t kCount = 1u << 20;
    constexpr uint64_t kBytes = kCount * sizeof(float);
    constexpr float kA = 2.0f;

    Device device = check(createDevice({ .validation = example::kValidation, .label = "hello-compute" }), "createDevice");
    const Capabilities& caps = device.capabilities();
    std::printf("%.*s (%.*s)\n", int(caps.adapterName.size()), caps.adapterName.data(), int(caps.floorName.size()), caps.floorName.data());

    ShaderLibrary library = check(device.loadShaderLibrary(embedded::kHelloComputeLibrary, "hello-compute"), "loadShaderLibrary");
    Pipeline saxpy = check(device.createPipeline(ComputePipelineDesc{ .compute = { library, "saxpy" }, .label = "saxpy" }), "createPipeline");

    std::vector<float> x(kCount);
    std::vector<float> y(kCount);
    for (uint32_t i = 0; i < kCount; ++i)
    {
        x[i] = float(i);
        y[i] = 1.0f;
    }

    Buffer xs = check(device.createBuffer({ .size = kBytes, .label = "x" }, std::as_bytes(std::span(x))), "createBuffer");
    Buffer ys = check(device.createBuffer({ .size = kBytes, .label = "y" }, std::as_bytes(std::span(y))), "createBuffer");
    Buffer result = check(device.createBuffer({ .size = kBytes, .usage = BufferUsage::ShaderWrite, .label = "result" }), "createBuffer");
    Buffer readback = check(device.createBuffer({ .size = kBytes, .memory = Memory::Readback, .label = "readback" }), "createBuffer");
    Fence done = check(device.createFence(0, "done"), "createFence");

    device.beginFrame();

    CommandList list = device.acquireCommandList(QueueType::Compute, "saxpy");
    list.setPipeline(saxpy);
    list.pushRoot(SaxpyRoot{ .x = device.address(xs), .y = device.address(ys), .result = device.address(result), .a = kA, .count = kCount });
    list.dispatch((kCount + 63) / 64, 1, 1);

    const std::array toCopy{ BufferBarrier{
        .buffer = result, .srcStage = Stage::ComputeShader, .srcAccess = Access::ShaderWrite, .dstStage = Stage::Copy, .dstAccess = Access::CopyRead } };
    list.barrier({}, toCopy);
    list.copyBuffer(result, 0, readback, 0, kBytes);
    list.end();

    const std::array signal{ FenceValue{ done, 1 } };
    device.queue(QueueType::Compute).submit(std::array{ list }, {}, signal);
    device.queue(QueueType::Graphics).submit({}, signal);

    device.endFrame();

    device.wait({ done, 1 }, UINT64_MAX);

    std::vector<float> output(kCount);
    std::memcpy(output.data(), device.cpuPointer(readback), kBytes);

    uint32_t wrong = 0;
    for (uint32_t i = 0; i < kCount; ++i)
    {
        wrong += output[i] != kA * x[i] + y[i] ? 1 : 0;
    }
    std::printf("saxpy over %u floats: %u wrong, result[12345] = %.1f\n", kCount, wrong, double(output[12345]));

    device.destroy(done);
    device.destroy(readback);
    device.destroy(result);
    device.destroy(ys);
    device.destroy(xs);
    device.destroy(saxpy);
    device.destroy(library);
    destroyDevice(device);
    return wrong == 0 ? 0 : 1;
}
