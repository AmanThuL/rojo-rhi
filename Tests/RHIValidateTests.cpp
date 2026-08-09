#include <catch2/catch_test_macros.hpp>

#include "RHI/Validate.h"

using namespace lmx::rhi;

namespace {
// ShaderLibrary is an interface; pipeline validation only inspects the pointer,
// so a trivial concrete subclass is enough to exercise the happy path.
struct DummyShaderLibrary : ShaderLibrary {};

// Stands in for the CAMetalLayer* the windowing layer supplies; validation only
// checks it against nullptr.
int dummyNativeLayer = 0;

// Texture is an interface and validateRenderPassTargets reads nothing but width()/height(), so an
// attachment of a given extent is expressible without a device. readback() is never reached --
// the helper does not call it -- so it is left empty rather than faked.
struct FakeTexture final : Texture {

    //==================================================================================================================
    FakeTexture(uint32_t width, uint32_t height) : m_width(width), m_height(height) {}

    //==================================================================================================================
    uint32_t width() const override { return m_width; }

    //==================================================================================================================
    uint32_t height() const override { return m_height; }

    //==================================================================================================================
    void readback(void*, uint64_t) override {}

private:
    uint32_t m_width = 0;
    uint32_t m_height = 0;
};
} // namespace

//======================================================================================================================
TEST_CASE("BufferDesc with zero size is rejected", "[rhi]") {
    BufferDesc desc{};
    desc.size = 0;
    desc.label = "zero";

    const auto r = validate(desc);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ErrorCode::InvalidDesc);
    REQUIRE(r.error().message.contains("size"));
}

//======================================================================================================================
TEST_CASE("BufferDesc with a non-zero size is accepted", "[rhi]") {
    BufferDesc desc{};
    desc.size = 256;
    desc.label = "vertices";

    REQUIRE(validate(desc).has_value());
}

//======================================================================================================================
TEST_CASE("TextureDesc with zero width is rejected", "[rhi]") {
    TextureDesc desc{};
    desc.width = 0;
    desc.height = 64;
    desc.label = "empty";

    const auto r = validate(desc);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ErrorCode::InvalidDesc);
    REQUIRE(r.error().message.contains("width"));
}

//======================================================================================================================
TEST_CASE("TextureDesc with zero height is rejected", "[rhi]") {
    TextureDesc desc{};
    desc.width = 64;
    desc.height = 0;
    desc.label = "flat";

    const auto r = validate(desc);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ErrorCode::InvalidDesc);
    REQUIRE(r.error().message.contains("height"));
}

//======================================================================================================================
// Metal 4 / Apple7+ caps a 2D texture at 16384 per side (Validate.cpp); one pixel over that on
// either axis must be rejected before it ever reaches the backend, since MTLTextureDescriptor
// validation aborts the process on an oversized texture rather than returning a diagnosable
// error. Pure validation -- no device or GPU needed to exercise this.
TEST_CASE("TextureDesc exceeding the max 2D dimension is rejected", "[rhi]") {
    TextureDesc desc{};
    desc.width = 16385;
    desc.height = 16385;
    desc.label = "oversized";

    const auto r = validate(desc);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ErrorCode::InvalidDesc);
    REQUIRE(r.error().message.contains("width"));
}

//======================================================================================================================
TEST_CASE("TextureDesc with Format::Unknown is rejected", "[rhi]") {
    TextureDesc desc{};
    desc.width = 64;
    desc.height = 64;
    desc.format = Format::Unknown;
    desc.label = "unknown";

    const auto r = validate(desc);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ErrorCode::InvalidDesc);
    REQUIRE(r.error().message.contains("format"));
    REQUIRE(r.error().message.contains("Unknown"));
}

//======================================================================================================================
TEST_CASE("TextureDesc cpuReadback with a format readback does not support is rejected", "[rhi]") {
    TextureDesc desc{};
    desc.width = 64;
    desc.height = 64;
    desc.format = Format::D32Float;
    desc.cpuReadback = true;
    desc.label = "depth readback";

    const auto r = validate(desc);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ErrorCode::InvalidDesc);
    REQUIRE(r.error().message.contains("cpuReadback"));
    REQUIRE(r.error().message.contains("format"));
}

//======================================================================================================================
TEST_CASE("TextureDesc with a render target and readback is accepted", "[rhi]") {
    TextureDesc desc{};
    desc.width = 64;
    desc.height = 64;
    desc.format = Format::BGRA8Unorm;
    desc.renderTarget = true;
    desc.cpuReadback = true;
    desc.label = "offscreen";

    REQUIRE(validate(desc).has_value());
}

//======================================================================================================================
TEST_CASE("GraphicsPipelineDesc with a null library is rejected", "[rhi]") {
    GraphicsPipelineDesc desc{};
    desc.library = nullptr;
    desc.vertexEntry = "vertexMain";
    desc.fragmentEntry = "fragmentMain";
    desc.label = "no library";

    const auto r = validate(desc);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ErrorCode::InvalidDesc);
    REQUIRE(r.error().message.contains("library"));
}

//======================================================================================================================
TEST_CASE("GraphicsPipelineDesc with an empty vertexEntry is rejected", "[rhi]") {
    DummyShaderLibrary library;
    GraphicsPipelineDesc desc{};
    desc.library = &library;
    desc.vertexEntry = "";
    desc.fragmentEntry = "fragmentMain";
    desc.label = "no vertex entry";

    const auto r = validate(desc);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ErrorCode::InvalidDesc);
    REQUIRE(r.error().message.contains("vertexEntry"));
}

//======================================================================================================================
TEST_CASE("GraphicsPipelineDesc with an empty fragmentEntry is rejected", "[rhi]") {
    DummyShaderLibrary library;
    GraphicsPipelineDesc desc{};
    desc.library = &library;
    desc.vertexEntry = "vertexMain";
    desc.fragmentEntry = "";
    desc.label = "no fragment entry";

    const auto r = validate(desc);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ErrorCode::InvalidDesc);
    REQUIRE(r.error().message.contains("fragmentEntry"));
}

//======================================================================================================================
TEST_CASE("GraphicsPipelineDesc with Format::Unknown color is rejected", "[rhi]") {
    DummyShaderLibrary library;
    GraphicsPipelineDesc desc{};
    desc.library = &library;
    desc.vertexEntry = "vertexMain";
    desc.fragmentEntry = "fragmentMain";
    desc.colorFormat = Format::Unknown;
    desc.label = "unknown color";

    const auto r = validate(desc);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ErrorCode::InvalidDesc);
    REQUIRE(r.error().message.contains("colorFormat"));
    REQUIRE(r.error().message.contains("Unknown"));
}

//======================================================================================================================
// Depth in a color slot is not a recoverable driver error: Metal's render-pipeline
// descriptor validator aborts the process on it, so validation has to catch it first.
TEST_CASE("GraphicsPipelineDesc with a depth colorFormat is rejected", "[rhi]") {
    DummyShaderLibrary library;
    GraphicsPipelineDesc desc{};
    desc.library = &library;
    desc.vertexEntry = "vertexMain";
    desc.fragmentEntry = "fragmentMain";
    desc.colorFormat = Format::D32Float;
    desc.label = "depth as color";

    const auto r = validate(desc);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ErrorCode::InvalidDesc);
    REQUIRE(r.error().message.contains("colorFormat"));
    REQUIRE(r.error().message.contains("color-renderable"));
}

//======================================================================================================================
TEST_CASE("GraphicsPipelineDesc with a library and both entries is accepted", "[rhi]") {
    DummyShaderLibrary library;
    GraphicsPipelineDesc desc{};
    desc.library = &library;
    desc.vertexEntry = "vertexMain";
    desc.fragmentEntry = "fragmentMain";
    desc.colorFormat = Format::BGRA8Unorm;
    desc.label = "triangle";

    REQUIRE(validate(desc).has_value());
}

//======================================================================================================================
TEST_CASE("SwapchainDesc with a null nativeLayer is rejected", "[rhi]") {
    SwapchainDesc desc{};
    desc.nativeLayer = nullptr;
    desc.width = 1280;
    desc.height = 720;

    const auto r = validate(desc);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ErrorCode::InvalidDesc);
    REQUIRE(r.error().message.contains("nativeLayer"));
}

//======================================================================================================================
TEST_CASE("SwapchainDesc with a zero extent is rejected", "[rhi]") {
    SwapchainDesc desc{};
    desc.nativeLayer = &dummyNativeLayer;
    desc.width = 0;
    desc.height = 720;

    const auto r = validate(desc);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ErrorCode::InvalidDesc);
    REQUIRE(r.error().message.contains("width"));
}

//======================================================================================================================
TEST_CASE("SwapchainDesc with a zero height is rejected", "[rhi]") {
    SwapchainDesc desc{};
    desc.nativeLayer = &dummyNativeLayer;
    desc.width = 1280;
    desc.height = 0;

    const auto r = validate(desc);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ErrorCode::InvalidDesc);
    REQUIRE(r.error().message.contains("height"));
}

//======================================================================================================================
TEST_CASE("SwapchainDesc with Format::Unknown is rejected", "[rhi]") {
    SwapchainDesc desc{};
    desc.nativeLayer = &dummyNativeLayer;
    desc.width = 1280;
    desc.height = 720;
    desc.format = Format::Unknown;

    const auto r = validate(desc);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ErrorCode::InvalidDesc);
    REQUIRE(r.error().message.contains("format"));
    REQUIRE(r.error().message.contains("Unknown"));
}

//======================================================================================================================
// CAMetalLayer rejects a depth pixel format outright, so this must not reach the backend.
TEST_CASE("SwapchainDesc with a depth format is rejected", "[rhi]") {
    SwapchainDesc desc{};
    desc.nativeLayer = &dummyNativeLayer;
    desc.width = 1280;
    desc.height = 720;
    desc.format = Format::D32Float;

    const auto r = validate(desc);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ErrorCode::InvalidDesc);
    REQUIRE(r.error().message.contains("format"));
    REQUIRE(r.error().message.contains("color-renderable"));
}

//======================================================================================================================
// RGBA16Float is the renderer's offscreen scene-color format. The swapchain remains SDR until its
// CAMetalLayer path explicitly configures and tests extended-range presentation.
TEST_CASE("SwapchainDesc with the offscreen HDR format is rejected", "[rhi]") {
    SwapchainDesc desc{};
    desc.nativeLayer = reinterpret_cast<void*>(0x1);
    desc.width = 1280;
    desc.height = 720;
    desc.format = Format::RGBA16Float;

    const auto r = validate(desc);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ErrorCode::InvalidDesc);
    REQUIRE(r.error().message.contains("SDR"));
}

//======================================================================================================================
TEST_CASE("SwapchainDesc with a layer and non-zero extent is accepted", "[rhi]") {
    SwapchainDesc desc{};
    desc.nativeLayer = &dummyNativeLayer;
    desc.width = 1280;
    desc.height = 720;
    desc.format = Format::BGRA8Unorm;

    REQUIRE(validate(desc).has_value());
}

//======================================================================================================================
// Named for what it pins rather than for the renderTarget-format rule: that rule is unreachable
// with today's Format enum (see Validate.cpp), so what is actually observable here is that a depth
// format is legal in both of its real roles.
TEST_CASE("TextureDesc D32Float is valid sampled and as a depth render target", "[rhi]") {
    TextureDesc desc{};
    desc.width = 64;
    desc.height = 64;
    desc.format = Format::D32Float;
    desc.sampled = true; // a sampled depth texture (a shadow map) is fine...
    REQUIRE(validate(desc).has_value());

    desc.sampled = false;
    desc.renderTarget = true; // ...and a D32 render target is a *depth* target: also fine.
    REQUIRE(validate(desc).has_value());
}

//======================================================================================================================
TEST_CASE("TextureDesc with no usage flags is rejected", "[rhi]") {
    TextureDesc desc{};
    desc.width = 64;
    desc.height = 64;
    desc.format = Format::BGRA8Unorm;
    desc.label = "unreachable";

    const auto r = validate(desc);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ErrorCode::InvalidDesc);
    REQUIRE(r.error().message.contains("usage"));
    REQUIRE(r.error().message.contains("sampled"));
}

//======================================================================================================================
TEST_CASE("TextureDesc with sampled as its only usage is accepted", "[rhi]") {
    TextureDesc desc{};
    desc.width = 64;
    desc.height = 64;
    desc.format = Format::BGRA8Unorm;
    desc.sampled = true;
    desc.label = "sampled only";

    REQUIRE(validate(desc).has_value());
}

//======================================================================================================================
TEST_CASE("GraphicsPipelineDesc depth flags without a depth format are rejected", "[rhi]") {
    DummyShaderLibrary library;
    GraphicsPipelineDesc desc{};
    desc.library = &library;
    desc.vertexEntry = "vertexMain";
    desc.fragmentEntry = "fragmentMain";
    desc.colorFormat = Format::BGRA8Unorm;
    desc.depthTestEnable = true; // but depthFormat left Unknown

    const auto r = validate(desc);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ErrorCode::InvalidDesc);
    REQUIRE(r.error().message.contains("depthFormat"));
}

//======================================================================================================================
TEST_CASE("GraphicsPipelineDesc with a color format as depth format is rejected", "[rhi]") {
    DummyShaderLibrary library;
    GraphicsPipelineDesc desc{};
    desc.library = &library;
    desc.vertexEntry = "vertexMain";
    desc.fragmentEntry = "fragmentMain";
    desc.colorFormat = Format::BGRA8Unorm;
    desc.depthFormat = Format::RGBA8Unorm;

    const auto r = validate(desc);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ErrorCode::InvalidDesc);
    REQUIRE(r.error().message.contains("depth"));
}

//======================================================================================================================
TEST_CASE("TextureDesc D32Float with cpuReadback is rejected", "[rhi]") {
    TextureDesc desc{};
    desc.width = 64;
    desc.height = 64;
    desc.format = Format::D32Float;
    desc.cpuReadback = true;

    const auto r = validate(desc);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ErrorCode::InvalidDesc);
}

//======================================================================================================================
// Metal's sampler descriptor validator aborts the process on an anisotropy outside 1..16 rather
// than returning an error, and newSamplerState has no NS::Error out-parameter at all -- so both
// ends of the range have to be caught here, before a SamplerDesc reaches the backend.
TEST_CASE("SamplerDesc with zero maxAnisotropy is rejected", "[rhi]") {
    SamplerDesc desc{};
    desc.maxAnisotropy = 0;
    desc.label = "no anisotropy";

    const auto r = validate(desc);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ErrorCode::InvalidDesc);
    REQUIRE(r.error().message.contains("anisotropy"));
}

//======================================================================================================================
TEST_CASE("SamplerDesc above the maximum anisotropy is rejected", "[rhi]") {
    SamplerDesc desc{};
    desc.maxAnisotropy = 17;
    desc.label = "too anisotropic";

    const auto r = validate(desc);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ErrorCode::InvalidDesc);
    REQUIRE(r.error().message.contains("anisotropy"));
}

//======================================================================================================================
TEST_CASE("SamplerDesc defaults are accepted", "[rhi]") {
    SamplerDesc desc{};
    desc.label = "linear wrap";

    REQUIRE(validate(desc).has_value());
}

//======================================================================================================================
TEST_CASE("SamplerDesc with a comparison function is accepted", "[rhi]") {
    SamplerDesc desc{};
    desc.compare = CompareFunc::LessEqual;
    desc.label = "shadow compare";

    REQUIRE(validate(desc).has_value());
}

//======================================================================================================================
// The comparison a reversed-Z shadow map needs, where a larger stored depth is the nearer surface.
TEST_CASE("SamplerDesc with a reversed-Z comparison function is accepted", "[rhi]") {
    SamplerDesc desc{};
    desc.compare = CompareFunc::GreaterEqual;
    desc.label = "reversed shadow compare";

    REQUIRE(validate(desc).has_value());
}

//======================================================================================================================
// A cube's six faces are all one square size in Metal (MTLTextureDescriptor takes a single
// width/height for the whole cube and ignores height for TypeCube), so a non-square desc is a
// caller mistake that would otherwise turn into six silently-resized faces.
TEST_CASE("TextureDesc Cube with unequal width and height is rejected", "[rhi]") {
    TextureDesc desc{};
    desc.width = 64;
    desc.height = 32;
    desc.format = Format::RGBA8Unorm;
    desc.kind = TextureKind::Cube;
    desc.sampled = true;
    desc.label = "oblong cube";

    const auto r = validate(desc);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ErrorCode::InvalidDesc);
    REQUIRE(r.error().message.contains("Cube"));
    REQUIRE(r.error().message.contains("width"));
}

//======================================================================================================================
TEST_CASE("TextureDesc Cube with equal width and height is accepted", "[rhi]") {
    TextureDesc desc{};
    desc.width = 64;
    desc.height = 64;
    desc.format = Format::RGBA8Unorm;
    desc.kind = TextureKind::Cube;
    desc.sampled = true;
    desc.label = "skybox";

    REQUIRE(validate(desc).has_value());
}

//======================================================================================================================
// Zero is not "no mips" -- level 0 always exists -- and MTLTextureDescriptor's
// mipmapLevelCount defaults to 1 for exactly that reason. A zero here would reach Metal as a
// descriptor whose validator aborts the process.
TEST_CASE("TextureDesc with zero mipLevels is rejected", "[rhi]") {
    TextureDesc desc{};
    desc.width = 64;
    desc.height = 64;
    desc.format = Format::RGBA8Unorm;
    desc.mipLevels = 0;
    desc.sampled = true;
    desc.label = "no levels";

    const auto r = validate(desc);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ErrorCode::InvalidDesc);
    REQUIRE(r.error().message.contains("mipLevels"));
}

//======================================================================================================================
// 64x64 has floor(log2(64)) + 1 == 7 levels (64, 32, 16, 8, 4, 2, 1); an eighth would have to
// be smaller than one texel.
TEST_CASE("TextureDesc with more mipLevels than the extent allows is rejected", "[rhi]") {
    TextureDesc desc{};
    desc.width = 64;
    desc.height = 64;
    desc.format = Format::RGBA8Unorm;
    desc.mipLevels = 8;
    desc.sampled = true;
    desc.label = "one level too many";

    const auto r = validate(desc);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ErrorCode::InvalidDesc);
    REQUIRE(r.error().message.contains("mipLevels"));
}

//======================================================================================================================
// The chain length is set by the *longer* side, not by both: a 64x1 texture still has 7 levels,
// with the short axis pinned at 1 from level 0 onward.
TEST_CASE("TextureDesc with the full mip chain for its longer side is accepted", "[rhi]") {
    TextureDesc desc{};
    desc.width = 64;
    desc.height = 1;
    desc.format = Format::RGBA8Unorm;
    desc.mipLevels = 7;
    desc.sampled = true;
    desc.label = "full chain";

    REQUIRE(validate(desc).has_value());
}

//======================================================================================================================
// This is what makes the renderable-format rule reachable: BC1 is a sampled-only compressed
// format, and Metal's render-pipeline validator aborts the process rather than returning an
// error when a non-renderable format reaches a color attachment.
TEST_CASE("TextureDesc renderTarget with a BC1 format is rejected", "[rhi]") {
    TextureDesc desc{};
    desc.width = 64;
    desc.height = 64;
    desc.format = Format::BC1Unorm;
    desc.renderTarget = true;
    desc.label = "compressed target";

    const auto r = validate(desc);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ErrorCode::InvalidDesc);
    REQUIRE(r.error().message.contains("renderTarget"));
    REQUIRE(r.error().message.contains("color-renderable"));
}

//======================================================================================================================
// The shape every compressed glTF texture arrives in: a block-compressed image with a
// precomputed mip chain, sampled and nothing else. 4x4 is one BC1 block, and its three levels
// (4x4, 2x2, 1x1) are each still that one block.
TEST_CASE("TextureDesc BC1 4x4 with three mip levels is accepted", "[rhi]") {
    TextureDesc desc{};
    desc.width = 4;
    desc.height = 4;
    desc.format = Format::BC1Unorm;
    desc.mipLevels = 3;
    desc.sampled = true;
    desc.label = "bc1 chain";

    REQUIRE(validate(desc).has_value());
}

//======================================================================================================================
// sRGB differs from its linear twin only in how the sampler decodes the bytes -- the bytes
// themselves are still four per texel, which is the whole of what readback needs to know.
TEST_CASE("TextureDesc sRGB with cpuReadback is accepted", "[rhi]") {
    TextureDesc desc{};
    desc.width = 64;
    desc.height = 64;
    desc.format = Format::RGBA8Unorm_sRGB;
    desc.cpuReadback = true;
    desc.label = "srgb readback";

    REQUIRE(validate(desc).has_value());
}

//======================================================================================================================
// readback() takes one flat destination buffer and no face index, so the six faces have nowhere to
// go; the desc has to be refused rather than quietly answering with one.
TEST_CASE("TextureDesc cpuReadback on a Cube is rejected", "[rhi]") {
    TextureDesc desc{};
    desc.width = 64;
    desc.height = 64;
    desc.format = Format::RGBA8Unorm;
    desc.kind = TextureKind::Cube;
    desc.cpuReadback = true;
    desc.label = "cube readback";

    const auto r = validate(desc);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ErrorCode::InvalidDesc);
    REQUIRE(r.error().message.contains("cpuReadback"));
    REQUIRE(r.error().message.contains("Cube"));
}

//======================================================================================================================
TEST_CASE("TextureDesc cpuReadback with a BC1 format is rejected", "[rhi]") {
    TextureDesc desc{};
    desc.width = 4;
    desc.height = 4;
    desc.format = Format::BC1Unorm;
    desc.cpuReadback = true;
    desc.label = "bc1 readback";

    const auto r = validate(desc);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ErrorCode::InvalidDesc);
    REQUIRE(r.error().message.contains("cpuReadback"));
}

//======================================================================================================================
// The one place a format becomes readback-capable. Zero is the answer for every format readback
// cannot express: block-compressed ones have no per-texel size at all, and the rest simply have no
// readback caller yet.
TEST_CASE("bytesPerPixel sizes the formats readback supports and zeroes the rest", "[rhi]") {
    REQUIRE(bytesPerPixel(Format::BGRA8Unorm) == 4);
    REQUIRE(bytesPerPixel(Format::RGBA8Unorm) == 4);
    REQUIRE(bytesPerPixel(Format::RGBA8Unorm_sRGB) == 4);
    REQUIRE(bytesPerPixel(Format::RGBA16Float) == 8);

    REQUIRE(bytesPerPixel(Format::RG16Float) == 0);
    REQUIRE(bytesPerPixel(Format::D32Float) == 0);
    REQUIRE(bytesPerPixel(Format::BC1Unorm) == 0);
    REQUIRE(bytesPerPixel(Format::BC1Unorm_sRGB) == 0);
    REQUIRE(bytesPerPixel(Format::Unknown) == 0);
}

//======================================================================================================================
// The scene-linear color format carries all three usages at once: a pass renders into it, a later
// pass samples it, and an offscreen test reads it back to assert on the values it holds.
TEST_CASE("TextureDesc RGBA16Float renders, samples, and reads back", "[rhi]") {
    TextureDesc desc{};
    desc.width = 64;
    desc.height = 64;
    desc.format = Format::RGBA16Float;
    desc.renderTarget = true;
    desc.sampled = true;
    desc.cpuReadback = true;
    desc.label = "hdr scene color";

    REQUIRE(validate(desc).has_value());
}

//======================================================================================================================
// The DFG lookup table is generated on the CPU and only ever sampled, so RG16Float stops there:
// rendering into one and reading one back have no caller, and an untested capability is worse than
// an absent one.
TEST_CASE("TextureDesc RG16Float sampled is accepted", "[rhi]") {
    TextureDesc desc{};
    desc.width = 64;
    desc.height = 64;
    desc.format = Format::RG16Float;
    desc.sampled = true;
    desc.label = "dfg lut";

    REQUIRE(validate(desc).has_value());
}

//======================================================================================================================
TEST_CASE("TextureDesc RG16Float as a render target is rejected", "[rhi]") {
    TextureDesc desc{};
    desc.width = 64;
    desc.height = 64;
    desc.format = Format::RG16Float;
    desc.renderTarget = true;
    desc.label = "two-channel target";

    const auto r = validate(desc);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ErrorCode::InvalidDesc);
    REQUIRE(r.error().message.contains("renderTarget"));
    REQUIRE(r.error().message.contains("color-renderable"));
}

//======================================================================================================================
TEST_CASE("TextureDesc RG16Float with cpuReadback is rejected", "[rhi]") {
    TextureDesc desc{};
    desc.width = 64;
    desc.height = 64;
    desc.format = Format::RG16Float;
    desc.cpuReadback = true;
    desc.label = "two-channel readback";

    const auto r = validate(desc);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ErrorCode::InvalidDesc);
    REQUIRE(r.error().message.contains("cpuReadback"));
}

//======================================================================================================================
TEST_CASE("GraphicsPipelineDesc with an RGBA16Float color format is accepted", "[rhi]") {
    DummyShaderLibrary library;
    GraphicsPipelineDesc desc{};
    desc.library = &library;
    desc.vertexEntry = "vertexMain";
    desc.fragmentEntry = "fragmentMain";
    desc.colorFormat = Format::RGBA16Float;
    desc.label = "hdr scene pipeline";

    REQUIRE(validate(desc).has_value());
}

//======================================================================================================================
TEST_CASE("GraphicsPipelineDesc with an RG16Float color format is rejected", "[rhi]") {
    DummyShaderLibrary library;
    GraphicsPipelineDesc desc{};
    desc.library = &library;
    desc.vertexEntry = "vertexMain";
    desc.fragmentEntry = "fragmentMain";
    desc.colorFormat = Format::RG16Float;
    desc.label = "lut pipeline";

    const auto r = validate(desc);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ErrorCode::InvalidDesc);
    REQUIRE(r.error().message.contains("colorFormat"));
}

//======================================================================================================================
// The depth-only pipeline the shadow pass needs: no color attachment, so no color format. Metal
// rejects a pipeline whose fragment stage writes a color the pass has nowhere to put, which is why
// this pairing has to be expressible at all.
TEST_CASE("GraphicsPipelineDesc with no color format but a depth format is accepted", "[rhi]") {
    DummyShaderLibrary library;
    GraphicsPipelineDesc desc{};
    desc.library = &library;
    desc.vertexEntry = "vertexMain";
    desc.fragmentEntry = "fragmentDepthOnly";
    desc.colorFormat = Format::Unknown;
    desc.depthFormat = Format::D32Float;
    desc.depthTestEnable = true;
    desc.depthWriteEnable = true;
    desc.label = "shadow";

    REQUIRE(validate(desc).has_value());
}

//======================================================================================================================
// The other half of that relaxation: Unknown on *both* is a pipeline that rasterises into nothing.
TEST_CASE("GraphicsPipelineDesc with neither a color nor a depth format is rejected", "[rhi]") {
    DummyShaderLibrary library;
    GraphicsPipelineDesc desc{};
    desc.library = &library;
    desc.vertexEntry = "vertexMain";
    desc.fragmentEntry = "fragmentMain";
    desc.colorFormat = Format::Unknown;
    desc.depthFormat = Format::Unknown;
    desc.label = "no attachments";

    const auto r = validate(desc);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ErrorCode::InvalidDesc);
    REQUIRE(r.error().message.contains("colorFormat"));
    REQUIRE(r.error().message.contains("depthFormat"));
}

//======================================================================================================================
// The raster fields added with the depth-only passes carry no rules of their own -- every
// enumerator maps to a legal backend value and a bias is any three floats -- so what this pins is
// exactly that: a fully-populated desc is not accidentally rejected by a rule meant for something
// else.
TEST_CASE("GraphicsPipelineDesc with non-default raster state is accepted", "[rhi]") {
    DummyShaderLibrary library;
    GraphicsPipelineDesc desc{};
    desc.library = &library;
    desc.vertexEntry = "vertexMain";
    desc.fragmentEntry = "fragmentMain";
    desc.colorFormat = Format::BGRA8Unorm;
    desc.depthFormat = Format::D32Float;
    desc.depthTestEnable = true;
    desc.fillMode = FillMode::Wireframe;
    desc.cullMode = CullMode::None;
    desc.depthCompare = DepthCompare::LessEqual;
    desc.depthBias = {.constant = 4.0f, .slopeScale = 1.0f, .clamp = 0.0f};
    desc.label = "wireframe sky";

    REQUIRE(validate(desc).has_value());
}

//
// Unlike every case above, this one is not about a *creation* desc: it is the render pass's
// attachment pairing, which the backend asserts through rather than returns. Testing it here is
// what keeps it a pure function with a message instead of a bare condition buried in
// beginRenderPass -- the five cases below need no device and no GPU.

//======================================================================================================================
TEST_CASE("a render pass with no attachments at all is rejected", "[rhi]") {
    const auto r = validateRenderPassTargets(nullptr, nullptr);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ErrorCode::InvalidDesc);
    REQUIRE(r.error().message.contains("colorTarget"));
    REQUIRE(r.error().message.contains("depthTarget"));
}

//======================================================================================================================
TEST_CASE("a color-only render pass is accepted", "[rhi]") {
    const FakeTexture color{64, 64};
    REQUIRE(validateRenderPassTargets(&color, nullptr).has_value());
}

//======================================================================================================================
TEST_CASE("a depth-only render pass is accepted", "[rhi]") {
    const FakeTexture depth{2048, 2048};
    REQUIRE(validateRenderPassTargets(nullptr, &depth).has_value());
}

//======================================================================================================================
TEST_CASE("color and depth attachments of the same extent are accepted", "[rhi]") {
    const FakeTexture color{64, 64};
    const FakeTexture depth{64, 64};
    REQUIRE(validateRenderPassTargets(&color, &depth).has_value());
}

//======================================================================================================================
TEST_CASE("color and depth attachments of different extents are rejected", "[rhi]") {
    const FakeTexture color{64, 64};
    const FakeTexture depth{32, 32};

    const auto r = validateRenderPassTargets(&color, &depth);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ErrorCode::InvalidDesc);
    REQUIRE(r.error().message.contains("extent"));
    // The two extents belong in the message: "they differ" is not actionable, "64x64 vs 32x32" is.
    REQUIRE(r.error().message.contains("64x64"));
    REQUIRE(r.error().message.contains("32x32"));
}

//======================================================================================================================
// Height alone, because a square-vs-square comparison would pass a helper that only ever looked
// at width.
TEST_CASE("color and depth attachments differing only in height are rejected", "[rhi]") {
    const FakeTexture color{64, 64};
    const FakeTexture depth{64, 32};

    const auto r = validateRenderPassTargets(&color, &depth);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().message.contains("extent"));
}
