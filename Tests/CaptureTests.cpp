#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <span>
#include <sstream>
#include <vector>

#include <rojoRHI/Metal4/Metal4Capture.h>
#include <rojoRHI/RHI.h>

namespace {
const bool kCaptureEnv = [] {
    ::setenv("MTL_CAPTURE_ENABLED", "1", 0);
    return true;
}();
} // namespace

//======================================================================================================================
TEST_CASE("beginCapture rejects bad paths without touching the filesystem", "[gpu]") {
    auto device = rojoRHI::createDevice();
    REQUIRE(device.has_value());
    REQUIRE_FALSE(rojoRHI::metal4::beginCapture(**device, ""));
    if (rojoRHI::metal4::captureAvailable()) {
        REQUIRE(rojoRHI::metal4::captureFailureReason().find("empty") != std::string_view::npos);
    } else {
        REQUIRE(rojoRHI::metal4::captureFailureReason().find("MTL_CAPTURE_ENABLED=1") !=
                std::string_view::npos);
    }
    REQUIRE_FALSE(rojoRHI::metal4::beginCapture(**device, "frame.trace"));
    if (rojoRHI::metal4::captureAvailable()) {
        REQUIRE(rojoRHI::metal4::captureFailureReason().find(".gputrace") !=
                std::string_view::npos);
    }
    REQUIRE_FALSE(std::filesystem::exists("frame.trace"));
}

//======================================================================================================================
TEST_CASE("capture write failure retains a recoverable reason and leaves other files intact",
          "[gpu]") {
    auto device = rojoRHI::createDevice();
    REQUIRE(device.has_value());
    if (!rojoRHI::metal4::captureAvailable()) {
        SKIP("write failure requires process capture capability");
    }
    const std::filesystem::path blocker = "lmx-capture-output-blocker.txt";
    {
        std::ofstream file(blocker);
        REQUIRE(file.good());
        file << "preserve this file";
    }
    REQUIRE_FALSE(rojoRHI::metal4::beginCapture(**device, (blocker / "frame.gputrace").string()));
    REQUIRE_FALSE(rojoRHI::metal4::captureFailureReason().empty());
    std::ifstream file(blocker);
    std::stringstream contents;
    contents << file.rdbuf();
    REQUIRE(contents.str() == "preserve this file");
    std::filesystem::remove(blocker);
    REQUIRE_FALSE(rojoRHI::metal4::beginCapture(**device, "frame.trace"));
    REQUIRE(rojoRHI::metal4::captureFailureReason().find("must end in .gputrace") !=
            std::string_view::npos);
}

//======================================================================================================================
TEST_CASE("begin/endCapture writes a .gputrace document", "[gpu]") {
    auto device = rojoRHI::createDevice();
    REQUIRE(device.has_value());
    const std::filesystem::path path = "lmx-capture-test.gputrace";
    std::filesystem::remove_all(path);
    if (!rojoRHI::metal4::captureAvailable()) {
        REQUIRE_FALSE(rojoRHI::metal4::beginCapture(**device, path.string()));
        REQUIRE(rojoRHI::metal4::captureFailureReason().find("MTL_CAPTURE_ENABLED=1") !=
                std::string_view::npos);
        REQUIRE_FALSE(std::filesystem::exists(path));
        return;
    }
    REQUIRE(rojoRHI::metal4::beginCapture(**device, path.string()));
    REQUIRE(rojoRHI::metal4::captureFailureReason().empty());
    REQUIRE_FALSE(rojoRHI::metal4::beginCapture(**device, path.string()));
    REQUIRE(rojoRHI::metal4::captureFailureReason().find("already in progress") !=
            std::string_view::npos);
    (*device)->beginFrame();
    (*device)->endFrame(nullptr);
    (*device)->waitIdle();
    rojoRHI::metal4::endCapture();
    REQUIRE(std::filesystem::exists(path));
    std::filesystem::remove_all(path);
}

//======================================================================================================================
TEST_CASE("endCapture writes the schema sidecar next to the bundle", "[gpu]") {
    auto device = rojoRHI::createDevice();
    REQUIRE(device.has_value());
    const std::filesystem::path bundle = "lmx-sidecar-test.gputrace";
    std::filesystem::remove_all(bundle);
    std::filesystem::remove(bundle.string() + ".schema.json");
    if (!rojoRHI::metal4::captureAvailable()) {
        SKIP("programmatic capture unavailable in this environment");
    }
    REQUIRE(rojoRHI::metal4::beginCapture(**device, bundle.string()));
    (*device)->beginFrame();
    (*device)->endFrame(nullptr);
    (*device)->waitIdle();
    rojoRHI::metal4::endCapture();
    const std::filesystem::path sidecar = bundle.string() + ".schema.json";
    REQUIRE(std::filesystem::exists(sidecar));
    // The device's own frame-data arena pages must be in the registry.
    std::ifstream in(sidecar);
    std::stringstream text;
    text << in.rdbuf();
    REQUIRE(text.str().find("rojorhi.device.frameData.0.page.0") != std::string::npos);
    std::filesystem::remove_all(bundle);
    std::filesystem::remove(sidecar);
}

namespace {

//======================================================================================================================
// The spike's known pattern: each byte is a pure function of its index, so the bundle blob can
// be verified byte-for-byte by regenerating the pattern rather than carrying a fixture.
std::vector<uint8_t> spikePatternBytes(uint32_t width, uint32_t height) {
    std::vector<uint8_t> bytes(size_t{width} * height * 4);
    for (size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<uint8_t>((i * 31u + (i >> 8)) & 0xFFu);
    }
    return bytes;
}
} // namespace

//======================================================================================================================
TEST_CASE("capture bundle carries a labeled texture's bytes", "[gpu][spike]") {
    using namespace rojoRHI;
    auto device = createDevice();
    REQUIRE(device.has_value());
    // 300x299, a size nothing engine-internal shares (the frame-data arena's normal pages are
    // exactly 256 KiB): the blob stays findable by size alone even if label recovery fails.
    constexpr uint32_t kW = 300, kH = 299;
    const std::vector<uint8_t> pattern = spikePatternBytes(kW, kH);
    const TextureMip mip{.data = pattern.data(), .bytesPerRow = kW * 4};
    auto source = (*device)->createTexture({.width = kW,
                                            .height = kH,
                                            .format = Format::RGBA8Unorm,
                                            .sampled = true,
                                            .label = "rojorhi.test.spikePattern"},
                                           std::span<const TextureMip>{&mip, 1});
    REQUIRE(source.has_value());
    auto target = (*device)->createTexture({.width = kW,
                                            .height = kH,
                                            .format = Format::BGRA8Unorm,
                                            .renderTarget = true,
                                            .label = "rojorhi.test.spikeTarget"});
    REQUIRE(target.has_value());
    auto library = (*device)->loadShaderLibrary("Shaders/FullscreenSample");
    REQUIRE(library.has_value());
    auto pipeline = (*device)->createGraphicsPipeline({.library = library->get(),
                                                       .vertexEntry = "vertexMain",
                                                       .fragmentEntry = "fragmentMain",
                                                       .colorFormat = Format::BGRA8Unorm,
                                                       .label = "rojorhi.test.spikePipeline"});
    REQUIRE(pipeline.has_value());

    const std::filesystem::path bundle = "lmx-spike.gputrace";
    std::filesystem::remove_all(bundle);
    if (!rojoRHI::metal4::captureAvailable()) {
        SKIP("programmatic capture unavailable in this environment");
    }
    REQUIRE(rojoRHI::metal4::beginCapture(**device, bundle.string()));
    CommandList& commands = (*device)->beginFrame();
    commands.beginRenderPass({.colorTarget = target->get(),
                              .clearColor = {0.0f, 0.0f, 0.0f, 1.0f},
                              .clear = true,
                              .label = "rojorhi.test.capture"});
    commands.bindPipeline(**pipeline);
    commands.bindTexture(0, **source); // FullscreenSample's gSource
    commands.draw(3);
    commands.endRenderPass();
    (*device)->endFrame(nullptr);
    (*device)->waitIdle();
    rojoRHI::metal4::endCapture();
    REQUIRE(std::filesystem::exists(bundle));
    // The bundle is left on disk deliberately: spike_inventory.py inspects it after the run.
}
