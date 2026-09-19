#pragma once

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "RHI/RHI.h"

#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace {

struct Vertex {
    float position[2];
    float color[3];
};
static_assert(sizeof(Vertex) == 20, "must match Slang's packed Vertex_natural_0 layout");

constexpr std::array<Vertex, 3> kTriangle = {{
    {{0.0f, 0.5f}, {1.0f, 0.0f, 0.0f}},
    {{-0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}},
    {{0.5f, -0.5f}, {0.0f, 0.0f, 1.0f}},
}};

constexpr uint32_t kVertexBufferSlot = 0;

// Small enough that the whole test is a few milliseconds, large enough that each probe below
// sits far enough inside its region to be deterministically interior without MSAA; see the
// per-probe margins noted at each REQUIRE below.
constexpr uint32_t kSize = 64;

// One BGRA8Unorm texel, in the channel order readback() produces.
struct Pixel {
    uint8_t b = 0, g = 0, r = 0, a = 0;
};

//======================================================================================================================
// [[maybe_unused]]: a header-defined helper is unused in whichever translation units declare no
// pixel probes of their own -- a buffer-only GPU test, for instance -- and that is not a warning.
[[maybe_unused]] Pixel pixelAt(const std::vector<uint8_t>& bgra, uint32_t x, uint32_t y) {
    const size_t offset = (size_t{y} * kSize + x) * 4;
    return {bgra[offset], bgra[offset + 1], bgra[offset + 2], bgra[offset + 3]};
}

//======================================================================================================================
// Catch2 stringifies the comparison operands, but not the pixel they came from; this goes
// through INFO so a failure says which colour was actually there.
[[maybe_unused]] std::string describe(const char* what, uint32_t x, uint32_t y, const Pixel& p) {
    return std::string(what) + " (" + std::to_string(x) + "," + std::to_string(y) +
           "): B=" + std::to_string(p.b) + " G=" + std::to_string(p.g) +
           " R=" + std::to_string(p.r) + " A=" + std::to_string(p.a);
}

// REQUIRE(result.has_value()) on its own reports "false != true"; the backend's message is
// the only thing that says *why*, so it is pulled out for INFO before the assertion.
template <typename T>

//======================================================================================================================
std::string errorOf(const lmx::rhi::Result<T>& result) {
    return result ? std::string{} : result.error().message;
}

//======================================================================================================================
// For probes whose expected value is an arithmetic result rather than an on/off channel.
inline bool channelNear(uint8_t actual, int expected, int tolerance) {
    return std::abs(int{actual} - expected) <= tolerance;
}

inline lmx::rhi::Result<std::unique_ptr<lmx::rhi::Texture>>
makeProbeTarget(lmx::rhi::Device& device, const char* label) {
    return device.createTexture({.width = kSize,
                                 .height = kSize,
                                 .format = lmx::rhi::Format::BGRA8Unorm,
                                 .renderTarget = true,
                                 .cpuReadback = true,
                                 .label = label});
}

inline std::vector<uint8_t>
renderSampledImage(lmx::rhi::Device& device, lmx::rhi::GraphicsPipeline& pipeline,
                   uint32_t textureSlot, lmx::rhi::Texture& source, lmx::rhi::Sampler& sampler,
                   lmx::rhi::Texture& destination, const lmx::rhi::TextureViewDesc& view = {}) {
    lmx::rhi::CommandList& commands = device.beginFrame();
    commands.beginRenderPass({.colorTarget = &destination,
                              .clearColor = {1.0f, 0.0f, 1.0f, 1.0f},
                              .clear = true,
                              .label = "lmx.test.textureUpload.probe"});
    commands.bindPipeline(pipeline);
    commands.bindTexture(textureSlot, source, view);
    commands.bindSampler(0, sampler);
    commands.draw(3);
    commands.endRenderPass();
    device.endFrame(nullptr);
    device.waitIdle();

    std::vector<uint8_t> pixels(size_t{kSize} * kSize * 4);
    destination.readback(pixels.data(), pixels.size());
    return pixels;
}

} // namespace
