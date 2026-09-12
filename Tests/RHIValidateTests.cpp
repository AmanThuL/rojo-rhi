#include <catch2/catch_test_macros.hpp>

#include "RHI/CommandList.h"
#include "RHI/Indirect.h"
#include "RHI/ShaderLibrary.h"
#include "RHI/Validate.h"

#include <limits>

#include <limits>

using namespace lmx::rhi;

namespace {
// ShaderLibrary is an interface; pipeline validation only inspects the pointer,
// so a trivial concrete subclass is enough to exercise the happy path.
struct DummyShaderLibrary : ShaderLibrary {};

// Stands in for the CAMetalLayer* the windowing layer supplies; validation only
// checks it against nullptr.
int dummyNativeLayer = 0;

// Texture is an interface and the helpers under test read nothing but its reported shape, so a
// texture of a given extent, format, and subresource count is expressible without a device.
// readback() is never reached -- no helper calls it -- so it is left empty rather than faked.
struct FakeTexture final : Texture {

    //==================================================================================================================
    FakeTexture(uint32_t width, uint32_t height, Format format = Format::BGRA8Unorm,
                uint32_t mipLevels = 1, uint32_t arrayLayers = 1)
        : m_width(width), m_height(height), m_format(format), m_mipLevels(mipLevels),
          m_arrayLayers(arrayLayers) {}

    //==================================================================================================================
    uint32_t width() const override { return m_width; }

    //==================================================================================================================
    uint32_t height() const override { return m_height; }

    //==================================================================================================================
    Format format() const override { return m_format; }

    //==================================================================================================================
    uint32_t mipLevels() const override { return m_mipLevels; }

    //==================================================================================================================
    uint32_t arrayLayers() const override { return m_arrayLayers; }

    //==================================================================================================================
    void readback(void*, uint64_t) override {}

private:
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    Format m_format = Format::BGRA8Unorm;
    uint32_t m_mipLevels = 1;
    uint32_t m_arrayLayers = 1;
};

// Heap is an interface and validatePlacement reads nothing but its size, so the bound a placement
// is checked against is expressible without a device.
struct FakeHeap final : Heap {

    //==================================================================================================================
    explicit FakeHeap(uint64_t size) : m_size(size) {}

    //==================================================================================================================
    uint64_t size() const override { return m_size; }

private:
    uint64_t m_size = 0;
};

// The same for buffers: the copy, fill, and barrier helpers read nothing but the allocation size,
// and identity comparisons between two of these stand in for "the same buffer twice".
struct FakeBuffer final : Buffer {

    //==================================================================================================================
    explicit FakeBuffer(uint64_t size) : m_size(size) {}

    //==================================================================================================================
    uint64_t size() const override { return m_size; }

    //==================================================================================================================
    void readback(void*, uint64_t) override {}

private:
    uint64_t m_size = 0;
};

//======================================================================================================================
// A region covering the whole of one mip level of a square texture, which is what most of the copy
// cases below start from before perturbing one field.
TextureCopyRegion wholeLevel(uint32_t extent, uint32_t mipLevel = 0, uint32_t arrayLayer = 0) {
    const uint32_t levelExtent = mipExtent(extent, mipLevel);
    return {.mipLevel = mipLevel,
            .arrayLayer = arrayLayer,
            .width = levelExtent,
            .height = levelExtent};
}
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
// Binding capacities are part of the public contract because slot indices are caller supplied.
TEST_CASE("argument table binding capacities are public", "[rhi]") {
    STATIC_REQUIRE(CommandList::kMaxBufferBindings == 8);
    STATIC_REQUIRE(CommandList::kMaxTextureBindings == 16);
    STATIC_REQUIRE(CommandList::kMaxSamplerBindings == 8);
}

//======================================================================================================================
TEST_CASE("frame-data validation rejects size plus alignment overflow", "[rhi]") {
    const uint32_t value = 1;
    const auto result = validateFrameData(0, &value, std::numeric_limits<uint64_t>::max(), 256);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().code == ErrorCode::InvalidDesc);
    REQUIRE(result.error().message.contains("overflows"));
}

//======================================================================================================================
TEST_CASE("resource alias barrier visibility composes with ordinary options", "[rhi]") {
    constexpr BarrierOptions options = BarrierOptions::None | BarrierOptions::ResourceAlias;
    STATIC_REQUIRE(hasBarrierOption(options, BarrierOptions::ResourceAlias));
    STATIC_REQUIRE(!hasBarrierOption(BarrierOptions::None, BarrierOptions::ResourceAlias));
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
TEST_CASE("ComputePipelineDesc with a null library is rejected", "[rhi]") {
    ComputePipelineDesc desc{};
    desc.computeEntry = "computeMain";

    const auto r = validate(desc);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ErrorCode::InvalidDesc);
    REQUIRE(r.error().message.contains("library"));
}

//======================================================================================================================
TEST_CASE("ComputePipelineDesc with an empty computeEntry is rejected", "[rhi]") {
    DummyShaderLibrary library;
    ComputePipelineDesc desc{};
    desc.library = &library;

    const auto r = validate(desc);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ErrorCode::InvalidDesc);
    REQUIRE(r.error().message.contains("computeEntry"));
}

//======================================================================================================================
// A zero here reads as "this dimension is unused", which is what 1 means; taken literally it is a
// grid of no threads at all.
TEST_CASE("ComputePipelineDesc with a zero threadgroup dimension is rejected", "[rhi]") {
    DummyShaderLibrary library;
    ComputePipelineDesc desc{};
    desc.library = &library;
    desc.computeEntry = "computeMain";
    desc.threadsPerThreadgroup[0] = 64;
    desc.threadsPerThreadgroup[1] = 0;
    desc.threadsPerThreadgroup[2] = 1;

    const auto r = validate(desc);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ErrorCode::InvalidDesc);
    REQUIRE(r.error().message.contains("threadsPerThreadgroup"));
}

//======================================================================================================================
TEST_CASE("ComputePipelineDesc with a library, an entry, and threads is accepted", "[rhi]") {
    DummyShaderLibrary library;
    ComputePipelineDesc desc{};
    desc.library = &library;
    desc.computeEntry = "computeMain";
    desc.threadsPerThreadgroup[0] = 64;
    desc.label = "histogram";

    REQUIRE(validate(desc).has_value());
}

//======================================================================================================================
TEST_CASE("TextureDesc storage usage with an sRGB format is rejected", "[rhi]") {
    TextureDesc desc{};
    desc.width = 64;
    desc.height = 64;
    desc.format = Format::RGBA8Unorm_sRGB;
    desc.storageWrite = true;

    const auto r = validate(desc);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ErrorCode::InvalidDesc);
    REQUIRE(r.error().message.contains("storage"));
}

//======================================================================================================================
TEST_CASE("TextureDesc storage usage with a storage format is accepted", "[rhi]") {
    TextureDesc desc{};
    desc.width = 64;
    desc.height = 64;
    desc.format = Format::RGBA16Float;
    desc.storageRead = true;
    desc.storageWrite = true;
    desc.label = "bloomChain";

    REQUIRE(validate(desc).has_value());
}

//======================================================================================================================
// The default range is the whole resource, which is what every whole-resource declaration passes.
TEST_CASE("a default subresource range covers the whole texture", "[rhi]") {
    const FakeTexture texture{64, 64, Format::RGBA8Unorm, /*mipLevels=*/4};

    REQUIRE(validateSubresourceRange(texture, {}).has_value());
}

//======================================================================================================================
TEST_CASE("a subresource range starting past the mip chain is rejected", "[rhi]") {
    const FakeTexture texture{64, 64, Format::RGBA8Unorm, /*mipLevels=*/4};
    TextureSubresourceRange range{};
    range.baseMipLevel = 4;

    const auto r = validateSubresourceRange(texture, range);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ErrorCode::InvalidDesc);
    REQUIRE(r.error().message.contains("baseMipLevel"));
}

//======================================================================================================================
TEST_CASE("a subresource range running past the mip chain is rejected", "[rhi]") {
    const FakeTexture texture{64, 64, Format::RGBA8Unorm, /*mipLevels=*/4};
    TextureSubresourceRange range{};
    range.baseMipLevel = 2;
    range.mipLevelCount = 3;

    const auto r = validateSubresourceRange(texture, range);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ErrorCode::InvalidDesc);
    REQUIRE(r.error().message.contains("3 mip level"));
}

//======================================================================================================================
TEST_CASE("a subresource range covering no mip level is rejected", "[rhi]") {
    const FakeTexture texture{64, 64, Format::RGBA8Unorm, /*mipLevels=*/4};
    TextureSubresourceRange range{};
    range.mipLevelCount = 0;

    const auto r = validateSubresourceRange(texture, range);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ErrorCode::InvalidDesc);
    REQUIRE(r.error().message.contains("at least one mip level"));
}

//======================================================================================================================
TEST_CASE("a single-mip subresource range of a chain is accepted", "[rhi]") {
    const FakeTexture texture{64, 64, Format::RGBA8Unorm, /*mipLevels=*/4};
    TextureSubresourceRange range{};
    range.baseMipLevel = 3;
    range.mipLevelCount = 1;

    REQUIRE(validateSubresourceRange(texture, range).has_value());
}

//======================================================================================================================
TEST_CASE("a subresource range past a cubemap's faces is rejected", "[rhi]") {
    const FakeTexture texture{64, 64, Format::RGBA8Unorm, /*mipLevels=*/1, /*arrayLayers=*/6};
    TextureSubresourceRange range{};
    range.baseArrayLayer = 4;
    range.arrayLayerCount = 4;

    const auto r = validateSubresourceRange(texture, range);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ErrorCode::InvalidDesc);
    REQUIRE(r.error().message.contains("layer"));
}

//======================================================================================================================
// The sRGB sibling describes the same bits under a different transfer function, which is the only
// reinterpretation a view may perform.
TEST_CASE("a texture view may reinterpret a format's sRGB sibling", "[rhi]") {
    const FakeTexture texture{64, 64, Format::RGBA8Unorm};
    TextureViewDesc view{};
    view.format = Format::RGBA8Unorm_sRGB;

    REQUIRE(validateTextureView(texture, view).has_value());
}

//======================================================================================================================
TEST_CASE("a texture view may not reinterpret a different format family", "[rhi]") {
    const FakeTexture texture{64, 64, Format::RGBA8Unorm};
    TextureViewDesc view{};
    view.format = Format::RGBA16Float;

    const auto r = validateTextureView(texture, view);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ErrorCode::InvalidDesc);
    REQUIRE(r.error().message.contains("format family"));
}

//======================================================================================================================
TEST_CASE("a texture view may select a cubemap face subset", "[rhi]") {
    const FakeTexture texture{64, 64, Format::RGBA8Unorm, /*mipLevels=*/1, /*arrayLayers=*/6};
    TextureViewDesc view{};
    view.range.baseArrayLayer = 2;
    view.range.arrayLayerCount = 1;

    REQUIRE(validateTextureView(texture, view).has_value());
}

//======================================================================================================================
TEST_CASE("a texture view reports the range problem before the format one", "[rhi]") {
    const FakeTexture texture{64, 64, Format::RGBA8Unorm, /*mipLevels=*/2};
    TextureViewDesc view{};
    view.range.baseMipLevel = 5;
    view.format = Format::RGBA16Float;

    const auto r = validateTextureView(texture, view);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().message.contains("baseMipLevel"));
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

    REQUIRE(bytesPerPixel(Format::RG16Float) == 4);
    REQUIRE(bytesPerPixel(Format::R8Unorm) == 1);

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
// The DFG lookup table is generated on the CPU and only ever sampled.
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
// The motion target the temporal path renders into: an extra colour attachment written by a
// fragment stage and read back by the tests that assert on the vectors it holds.
TEST_CASE("TextureDesc RG16Float renders and reads back", "[rhi]") {
    TextureDesc desc{};
    desc.width = 64;
    desc.height = 64;
    desc.format = Format::RG16Float;
    desc.renderTarget = true;
    desc.sampled = true;
    desc.cpuReadback = true;
    desc.label = "motion";

    REQUIRE(validate(desc).has_value());
}

//======================================================================================================================
// The reactive-mask target the temporal path renders into: a single-channel attachment a fragment
// stage writes, a later pass samples, and a readback reads one byte per texel from.
TEST_CASE("TextureDesc R8Unorm renders, samples, and reads back", "[rhi][validate]") {
    TextureDesc desc{};
    desc.width = 64;
    desc.height = 64;
    desc.format = Format::R8Unorm;
    desc.renderTarget = true;
    desc.sampled = true;
    desc.cpuReadback = true;
    desc.label = "reactive";

    REQUIRE(validate(desc).has_value());
}

//======================================================================================================================
TEST_CASE("TextureDesc temporal packing formats support storage", "[rhi][validate]") {
    for (const auto format : {Format::RG16Float, Format::R8Unorm, Format::R16Float}) {
        TextureDesc desc{};
        desc.width = 64;
        desc.height = 64;
        desc.format = format;
        desc.storageWrite = true;
        desc.storageRead = true;
        desc.label = "temporal packing storage";
        REQUIRE(validate(desc).has_value());
    }
    REQUIRE(bytesPerPixel(Format::R16Float) == 2);
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
TEST_CASE("GraphicsPipelineDesc with an RG16Float color format is accepted", "[rhi]") {
    DummyShaderLibrary library;
    GraphicsPipelineDesc desc{};
    desc.library = &library;
    desc.vertexEntry = "vertexMain";
    desc.fragmentEntry = "fragmentMain";
    desc.colorFormat = Format::RG16Float;
    desc.label = "motion pipeline";

    REQUIRE(validate(desc).has_value());
}

//======================================================================================================================
TEST_CASE("GraphicsPipelineDesc extra color formats need a primary color format",
          "[rhi][validate]") {
    DummyShaderLibrary library;
    GraphicsPipelineDesc desc{};
    desc.library = &library;
    desc.vertexEntry = "vertexMain";
    desc.fragmentEntry = "fragmentMain";
    desc.colorFormat = Format::Unknown;
    desc.depthFormat = Format::D32Float;
    desc.extraColorFormats[0] = Format::RG16Float;
    desc.extraColorCount = 1;
    desc.label = "depth-only with extras";

    const auto r = validate(desc);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ErrorCode::InvalidDesc);
    REQUIRE(r.error().message.contains("extraColorFormats"));
    REQUIRE(r.error().message.contains("colorFormat"));
}

//======================================================================================================================
TEST_CASE("GraphicsPipelineDesc extraColorCount past the limit is rejected", "[rhi][validate]") {
    DummyShaderLibrary library;
    GraphicsPipelineDesc desc{};
    desc.library = &library;
    desc.vertexEntry = "vertexMain";
    desc.fragmentEntry = "fragmentMain";
    desc.extraColorCount = kMaxExtraColorTargets + 1;
    desc.label = "too many extras";

    const auto r = validate(desc);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ErrorCode::InvalidDesc);
    REQUIRE(r.error().message.contains("extraColorCount"));
}

//======================================================================================================================
// Unknown is the "no attachment" spelling for the primary format, and a counted extra is by
// definition an attachment, so the same value cannot mean both things there.
TEST_CASE("GraphicsPipelineDesc with an Unknown extra color format is rejected",
          "[rhi][validate]") {
    DummyShaderLibrary library;
    GraphicsPipelineDesc desc{};
    desc.library = &library;
    desc.vertexEntry = "vertexMain";
    desc.fragmentEntry = "fragmentMain";
    desc.extraColorFormats[0] = Format::Unknown;
    desc.extraColorCount = 1;
    desc.label = "unknown extra";

    const auto r = validate(desc);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ErrorCode::InvalidDesc);
    REQUIRE(r.error().message.contains("extraColorFormats[0]"));
}

//======================================================================================================================
TEST_CASE("GraphicsPipelineDesc with a non-renderable extra color format is rejected",
          "[rhi][validate]") {
    DummyShaderLibrary library;
    GraphicsPipelineDesc desc{};
    desc.library = &library;
    desc.vertexEntry = "vertexMain";
    desc.fragmentEntry = "fragmentMain";
    desc.extraColorFormats[0] = Format::RG16Float;
    desc.extraColorFormats[1] = Format::BC1Unorm;
    desc.extraColorCount = 2;
    desc.label = "block-compressed extra";

    const auto r = validate(desc);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ErrorCode::InvalidDesc);
    REQUIRE(r.error().message.contains("extraColorFormats[1]"));
    REQUIRE(r.error().message.contains("color-renderable"));
}

//======================================================================================================================
// The pipeline the motion pass needs: scene colour at attachment zero, motion vectors after it.
TEST_CASE("GraphicsPipelineDesc with a full set of extra color formats is accepted",
          "[rhi][validate]") {
    DummyShaderLibrary library;
    GraphicsPipelineDesc desc{};
    desc.library = &library;
    desc.vertexEntry = "vertexMain";
    desc.fragmentEntry = "fragmentMain";
    desc.colorFormat = Format::RGBA16Float;
    desc.extraColorFormats[0] = Format::RG16Float;
    desc.extraColorFormats[1] = Format::RGBA8Unorm;
    desc.extraColorFormats[2] = Format::BGRA8Unorm;
    desc.extraColorCount = kMaxExtraColorTargets;
    desc.label = "scene with motion";

    REQUIRE(validate(desc).has_value());
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

//======================================================================================================================
TEST_CASE("a render pass with no extra color attachments is accepted", "[rhi][validate]") {
    const FakeTexture color{64, 64};
    REQUIRE(validateExtraColorTargets(&color, nullptr, 0).has_value());
}

//======================================================================================================================
// Attachment zero is the primary target, so an extra with nothing in front of it would be a pass
// whose attachments start at index one.
TEST_CASE("extra color attachments without a primary color target are rejected",
          "[rhi][validate]") {
    FakeTexture motion{64, 64, Format::RG16Float};
    const ExtraColorTarget extras[1] = {{.target = &motion}};

    const auto r = validateExtraColorTargets(nullptr, extras, 1);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ErrorCode::InvalidDesc);
    REQUIRE(r.error().message.contains("extraColor"));
    REQUIRE(r.error().message.contains("colorTarget"));
}

//======================================================================================================================
TEST_CASE("more extra color attachments than the limit are rejected", "[rhi][validate]") {
    const FakeTexture color{64, 64};

    const auto r = validateExtraColorTargets(&color, nullptr, kMaxExtraColorTargets + 1);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ErrorCode::InvalidDesc);
    REQUIRE(r.error().message.contains("extraColorCount"));
}

//======================================================================================================================
TEST_CASE("a null extra color attachment is rejected", "[rhi][validate]") {
    const FakeTexture color{64, 64};
    FakeTexture motion{64, 64, Format::RG16Float};
    const ExtraColorTarget extras[2] = {{.target = &motion}, {.target = nullptr}};

    const auto r = validateExtraColorTargets(&color, extras, 2);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ErrorCode::InvalidDesc);
    REQUIRE(r.error().message.contains("extraColor[1]"));
}

//======================================================================================================================
TEST_CASE("an extra color attachment of a different extent is rejected", "[rhi][validate]") {
    const FakeTexture color{64, 64};
    FakeTexture motion{64, 32, Format::RG16Float};
    const ExtraColorTarget extras[1] = {{.target = &motion}};

    const auto r = validateExtraColorTargets(&color, extras, 1);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ErrorCode::InvalidDesc);
    REQUIRE(r.error().message.contains("extent"));
    REQUIRE(r.error().message.contains("64x64"));
    REQUIRE(r.error().message.contains("64x32"));
}

//======================================================================================================================
TEST_CASE("an extra color attachment with a non-renderable format is rejected", "[rhi][validate]") {
    const FakeTexture color{64, 64};
    FakeTexture lut{64, 64, Format::BC1Unorm};
    const ExtraColorTarget extras[1] = {{.target = &lut}};

    const auto r = validateExtraColorTargets(&color, extras, 1);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ErrorCode::InvalidDesc);
    REQUIRE(r.error().message.contains("extraColor[0]"));
    REQUIRE(r.error().message.contains("color-renderable"));
}

//======================================================================================================================
TEST_CASE("extra color attachments matching the primary are accepted", "[rhi][validate]") {
    const FakeTexture color{64, 64};
    FakeTexture motion{64, 64, Format::RG16Float};
    FakeTexture normal{64, 64, Format::RGBA8Unorm};
    const ExtraColorTarget extras[2] = {{.target = &motion}, {.target = &normal}};

    REQUIRE(validateExtraColorTargets(&color, extras, 2).has_value());
}

//======================================================================================================================
// Zero is the "whole attachment" spelling, which is what a pass that never asked for a sub-region
// carries. It stays valid whatever the attachment's extent is.
TEST_CASE("an unset render area is accepted", "[rhi][validate]") {
    const FakeTexture color{64, 64};

    REQUIRE(validateRenderArea(&color, nullptr, 0, 0).has_value());
}

//======================================================================================================================
// One zero is the dangerous half-set state: a width with no height would otherwise reach Metal as
// a zero-height viewport that rasterises nothing.
TEST_CASE("a render area with only one dimension set is rejected", "[rhi][validate]") {
    const FakeTexture color{64, 64};

    const auto r = validateRenderArea(&color, nullptr, 32, 0);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ErrorCode::InvalidDesc);
    REQUIRE(r.error().message.contains("renderAreaWidth"));
    REQUIRE(r.error().message.contains("renderAreaHeight"));
}

//======================================================================================================================
// Width alone, so a helper that only ever compared heights would not pass.
TEST_CASE("a render area wider than the color attachment is rejected", "[rhi][validate]") {
    const FakeTexture color{64, 64};

    const auto r = validateRenderArea(&color, nullptr, 65, 16);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ErrorCode::InvalidDesc);
    // Both extents belong in the message: which one was too large is the actionable part.
    REQUIRE(r.error().message.contains("65x16"));
    REQUIRE(r.error().message.contains("64x64"));
}

//======================================================================================================================
// A depth-only pass has no colour attachment, so depth is the only extent the area can be measured
// against.
TEST_CASE("a render area taller than a depth-only pass's attachment is rejected",
          "[rhi][validate]") {
    const FakeTexture depth{64, 64};

    const auto r = validateRenderArea(nullptr, &depth, 16, 65);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ErrorCode::InvalidDesc);
    REQUIRE(r.error().message.contains("16x65"));
    REQUIRE(r.error().message.contains("64x64"));
}

//======================================================================================================================
// The whole attachment expressed the long way: a full-extent area is a legal sub-rectangle, not an
// off-by-one past the last texel.
TEST_CASE("a render area matching the attachment extent is accepted", "[rhi][validate]") {
    const FakeTexture color{64, 64};

    REQUIRE(validateRenderArea(&color, nullptr, 64, 64).has_value());
}

//======================================================================================================================
// The default range is the whole allocation, which is what a barrier with no byte detail means.
TEST_CASE("a default buffer range covers the whole buffer", "[rhi]") {
    const FakeBuffer buffer{256};

    REQUIRE(validateBufferRange(buffer, {}).has_value());
}

//======================================================================================================================
TEST_CASE("a buffer range starting past the allocation is rejected", "[rhi]") {
    const FakeBuffer buffer{256};

    const auto r = validateBufferRange(buffer, {.offset = 256});
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ErrorCode::InvalidDesc);
    REQUIRE(r.error().message.contains("256"));
}

//======================================================================================================================
TEST_CASE("a buffer range running past the allocation is rejected", "[rhi]") {
    const FakeBuffer buffer{256};

    const auto r = validateBufferRange(buffer, {.offset = 128, .size = 200});
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().message.contains("past the buffer's"));
}

//======================================================================================================================
TEST_CASE("a buffer range covering no bytes is rejected", "[rhi]") {
    const FakeBuffer buffer{256};

    const auto r = validateBufferRange(buffer, {.offset = 0, .size = 0});
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().message.contains("at least one byte"));
}

//======================================================================================================================
// The whole-buffer sentinel resolves against this buffer's own size, so the tail of a large buffer
// is expressible without the caller doing the subtraction.
TEST_CASE("a buffer range from an offset to the end is accepted", "[rhi]") {
    const FakeBuffer buffer{256};

    REQUIRE(validateBufferRange(buffer, {.offset = 192}).has_value());
}

//======================================================================================================================
TEST_CASE("a buffer copy between disjoint ranges of one buffer is accepted", "[rhi]") {
    const FakeBuffer buffer{256};

    REQUIRE(validateBufferCopy(buffer, 0, buffer, 128, 128).has_value());
}

//======================================================================================================================
// Metal leaves the direction of an overlapping self-copy undefined, so the RHI refuses it rather
// than shipping a result that depends on the hardware.
TEST_CASE("a buffer copy between overlapping ranges of one buffer is rejected", "[rhi]") {
    const FakeBuffer buffer{256};

    const auto r = validateBufferCopy(buffer, 0, buffer, 64, 128);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().message.contains("overlap"));
}

//======================================================================================================================
// The same offsets in two different allocations are not an overlap; only identity makes them one.
TEST_CASE("a buffer copy between two buffers at the same offsets is accepted", "[rhi]") {
    const FakeBuffer source{256};
    const FakeBuffer destination{256};

    REQUIRE(validateBufferCopy(source, 0, destination, 0, 256).has_value());
}

//======================================================================================================================
TEST_CASE("a buffer copy running past the source is rejected", "[rhi]") {
    const FakeBuffer source{128};
    const FakeBuffer destination{256};

    const auto r = validateBufferCopy(source, 64, destination, 0, 128);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().message.contains("past the buffer's"));
}

//======================================================================================================================
// The region's extent is in texels of its own mip level, so the whole of level two of a 64x64 chain
// is 16x16 -- not 64x64 clamped.
TEST_CASE("a copy region covering a whole mip level is accepted", "[rhi]") {
    const FakeTexture texture{64, 64, Format::RGBA8Unorm, /*mipLevels=*/4};

    REQUIRE(validateTextureCopyRegion(texture, wholeLevel(64, /*mipLevel=*/2)).has_value());
}

//======================================================================================================================
TEST_CASE("a copy region sized for level zero is rejected on a smaller mip level", "[rhi]") {
    const FakeTexture texture{64, 64, Format::RGBA8Unorm, /*mipLevels=*/4};
    TextureCopyRegion region = wholeLevel(64);
    region.mipLevel = 2;

    const auto r = validateTextureCopyRegion(texture, region);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ErrorCode::InvalidDesc);
    REQUIRE(r.error().message.contains("16x16"));
}

//======================================================================================================================
TEST_CASE("a copy region on a mip level past the chain is rejected", "[rhi]") {
    const FakeTexture texture{64, 64, Format::RGBA8Unorm, /*mipLevels=*/2};

    const auto r = validateTextureCopyRegion(texture, wholeLevel(64, /*mipLevel=*/2));
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().message.contains("mipLevel"));
}

//======================================================================================================================
TEST_CASE("a copy region on a layer past a cubemap's faces is rejected", "[rhi]") {
    const FakeTexture texture{64, 64, Format::RGBA8Unorm, /*mipLevels=*/1, /*arrayLayers=*/6};

    const auto r = validateTextureCopyRegion(texture, wholeLevel(64, 0, /*arrayLayer=*/6));
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().message.contains("arrayLayer"));
}

//======================================================================================================================
TEST_CASE("a copy region on one face of a cubemap is accepted", "[rhi]") {
    const FakeTexture texture{64, 64, Format::RGBA8Unorm, /*mipLevels=*/3, /*arrayLayers=*/6};

    REQUIRE(validateTextureCopyRegion(texture, wholeLevel(64, /*mipLevel=*/1, /*arrayLayer=*/4))
                .has_value());
}

//======================================================================================================================
// The third dimension exists in the vocabulary but has no texture kind behind it yet, so a caller
// reaching for it gets told that rather than a silently ignored field.
TEST_CASE("a copy region with a depth other than one is rejected", "[rhi]") {
    const FakeTexture texture{64, 64, Format::RGBA8Unorm};
    TextureCopyRegion region = wholeLevel(64);
    region.depth = 2;

    const auto r = validateTextureCopyRegion(texture, region);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().message.contains("3D texture"));
}

//======================================================================================================================
TEST_CASE("a copy region running past its mip level is rejected", "[rhi]") {
    const FakeTexture texture{64, 64, Format::RGBA8Unorm};
    TextureCopyRegion region = wholeLevel(64);
    region.x = 1;

    const auto r = validateTextureCopyRegion(texture, region);
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().message.contains("past mip level"));
}

//======================================================================================================================
// A tightly packed destination for a whole 32x32 level of RGBA8: 4096 bytes at a 128-byte stride.
TEST_CASE("a tightly packed buffer/texture copy is accepted", "[rhi]") {
    const FakeTexture texture{64, 64, Format::RGBA8Unorm, /*mipLevels=*/2};
    const FakeBuffer buffer{32 * 32 * 4};

    REQUIRE(validateBufferTextureCopy(buffer, {.bytesPerRow = 32 * 4}, texture,
                                      wholeLevel(64, /*mipLevel=*/1))
                .has_value());
}

//======================================================================================================================
TEST_CASE("a buffer/texture copy with too narrow a row stride is rejected", "[rhi]") {
    const FakeTexture texture{64, 64, Format::RGBA8Unorm};
    const FakeBuffer buffer{64 * 64 * 4};

    const auto r =
        validateBufferTextureCopy(buffer, {.bytesPerRow = 32 * 4}, texture, wholeLevel(64));
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().message.contains("bytesPerRow"));
}

//======================================================================================================================
TEST_CASE("a buffer/texture copy running past the buffer is rejected", "[rhi]") {
    const FakeTexture texture{64, 64, Format::RGBA8Unorm};
    const FakeBuffer buffer{64 * 64 * 4};

    const auto r = validateBufferTextureCopy(buffer, {.offset = 4, .bytesPerRow = 64 * 4}, texture,
                                             wholeLevel(64));
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().message.contains("past the buffer's"));
}

//======================================================================================================================
// Row padding after the final row is not read or written. A buffer ending at the final texel is
// therefore large enough even when earlier rows use a wider stride.
TEST_CASE("a padded buffer texture layout needs no padding after its final row", "[rhi]") {
    const FakeTexture texture{2, 2, Format::RGBA8Unorm};
    const FakeBuffer buffer{24}; // 16-byte first-row stride, then 8 bytes of final-row texels.

    REQUIRE(
        validateBufferTextureCopy(buffer, {.bytesPerRow = 16}, texture, {.width = 2, .height = 2})
            .has_value());
}

//======================================================================================================================
// A stride is caller-controlled uint64 data. It must not wrap the footprint arithmetic into a
// small in-bounds value before the native copy encoder sees it.
TEST_CASE("a buffer texture layout whose row footprint overflows is rejected", "[rhi]") {
    const FakeTexture texture{1, 3, Format::RGBA8Unorm};
    const FakeBuffer buffer{64};
    constexpr uint64_t kHugeTexelAlignedStride = std::numeric_limits<uint64_t>::max() - 3;

    const auto r = validateBufferTextureCopy(buffer, {.bytesPerRow = kHugeTexelAlignedStride},
                                             texture, {.width = 1, .height = 3});
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().message.contains("overflow"));
}

//======================================================================================================================
// A row stride that is not a whole number of texels is an addressing bug the hardware cannot
// express, so it is rejected before Metal sees it.
TEST_CASE("a buffer/texture copy with a partial-texel row stride is rejected", "[rhi]") {
    const FakeTexture texture{64, 64, Format::RGBA8Unorm};
    const FakeBuffer buffer{64 * 64 * 8};

    const auto r =
        validateBufferTextureCopy(buffer, {.bytesPerRow = 64 * 4 + 2}, texture, wholeLevel(64));
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().message.contains("multiples of the format's texel size"));
}

//======================================================================================================================
TEST_CASE("a buffer/texture copy of a block-compressed format is rejected", "[rhi]") {
    const FakeTexture texture{64, 64, Format::BC1Unorm};
    const FakeBuffer buffer{64 * 64};

    const auto r = validateBufferTextureCopy(buffer, {.bytesPerRow = 32}, texture, wholeLevel(64));
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().message.contains("packed texel size"));
}

//======================================================================================================================
// Level one's top-left 16x16 corner into the whole of level two, which is the shape a downsample
// chain's copies have: the extents match, and each region is in bounds of its own level.
TEST_CASE("a texture copy between two mip levels of one texture is accepted", "[rhi]") {
    const FakeTexture texture{64, 64, Format::RGBA8Unorm, /*mipLevels=*/3};

    REQUIRE(validateTextureCopy(texture, {.mipLevel = 1, .width = 16, .height = 16}, texture,
                                wholeLevel(64, /*mipLevel=*/2))
                .has_value());
}

//======================================================================================================================
TEST_CASE("a texture copy between regions of different extents is rejected", "[rhi]") {
    const FakeTexture source{64, 64, Format::RGBA8Unorm};
    const FakeTexture destination{32, 32, Format::RGBA8Unorm};

    const auto r = validateTextureCopy(source, wholeLevel(64), destination, wholeLevel(32));
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().message.contains("same extent"));
    REQUIRE(r.error().message.contains("64x64"));
    REQUIRE(r.error().message.contains("32x32"));
}

//======================================================================================================================
TEST_CASE("a texture copy between different formats is rejected", "[rhi]") {
    const FakeTexture source{64, 64, Format::RGBA8Unorm};
    const FakeTexture destination{64, 64, Format::RGBA16Float};

    const auto r = validateTextureCopy(source, wholeLevel(64), destination, wholeLevel(64));
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().message.contains("same format"));
}

//======================================================================================================================
TEST_CASE("a texture copy between overlapping regions of one subresource is rejected", "[rhi]") {
    const FakeTexture texture{64, 64, Format::RGBA8Unorm};

    const auto r = validateTextureCopy(texture, {.width = 32, .height = 32}, texture,
                                       {.x = 16, .y = 16, .width = 32, .height = 32});
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().message.contains("overlap"));
}

//======================================================================================================================
// Two rectangles of one subresource that share neither a column nor a row have no hazard, so the
// disjointness check must not fire on them.
TEST_CASE("a texture copy between disjoint regions of one subresource is accepted", "[rhi]") {
    const FakeTexture texture{64, 64, Format::RGBA8Unorm};

    REQUIRE(validateTextureCopy(texture, {.width = 32, .height = 32}, texture,
                                {.x = 32, .y = 32, .width = 32, .height = 32})
                .has_value());
}

//======================================================================================================================
TEST_CASE("indirect arguments at an aligned in-bounds offset are accepted", "[rhi]") {
    const FakeBuffer buffer{256};

    REQUIRE(validateIndirectArgs(buffer, 64, sizeof(DrawIndexedIndirectArgs)).has_value());
}

//======================================================================================================================
TEST_CASE("indirect arguments at an unaligned offset are rejected", "[rhi]") {
    const FakeBuffer buffer{256};

    const auto r = validateIndirectArgs(buffer, 6, sizeof(DispatchIndirectArgs));
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ErrorCode::InvalidDesc);
    REQUIRE(r.error().message.contains("multiple of 4"));
}

//======================================================================================================================
// The whole struct has to fit: an offset four bytes short of the end is in bounds by itself and
// still reads past the allocation.
TEST_CASE("indirect arguments running past the buffer are rejected", "[rhi]") {
    const FakeBuffer buffer{16};

    const auto r = validateIndirectArgs(buffer, 8, sizeof(DrawIndirectArgs));
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().message.contains("past the buffer's"));
}

//======================================================================================================================
// The shape a caller must get right before any frame-owned memory is touched. Capacity is
// deliberately absent: the arena grows, so running out of it is the backend's failure to report,
// not a request the caller could have made smaller.
TEST_CASE("a frame-data request is checked for slot, block, and alignment", "[rhi][validate]") {
    int block = 0;

    REQUIRE(validateFrameData(0, &block, sizeof(block), kFrameDataAlignment).has_value());
    REQUIRE(validateFrameData(CommandList::kMaxBufferBindings - 1, &block, 1, 4096).has_value());

    const auto slot = validateFrameData(CommandList::kMaxBufferBindings, &block, 4, 256);
    REQUIRE_FALSE(slot.has_value());
    REQUIRE(slot.error().code == ErrorCode::InvalidDesc);
    REQUIRE(slot.error().message.contains("buffer binding count"));

    REQUIRE_FALSE(validateFrameData(0, nullptr, 4, 256).has_value());
    REQUIRE_FALSE(validateFrameData(0, &block, 0, 256).has_value());
}

//======================================================================================================================
// Both halves of the alignment rule, each with its own diagnostic: too small breaks the placement
// contract a constant buffer is read through, and not a power of two is not an alignment at all.
TEST_CASE("a frame-data alignment must be a power of two of at least 256", "[rhi][validate]") {
    int block = 0;

    REQUIRE(validateFrameData(0, &block, 4, 256).has_value());
    REQUIRE(validateFrameData(0, &block, 4, 512).has_value());
    REQUIRE(validateFrameData(0, &block, 4, 65536).has_value());

    const auto small = validateFrameData(0, &block, 4, 128);
    REQUIRE_FALSE(small.has_value());
    REQUIRE(small.error().message.contains("at least 256"));

    const auto odd = validateFrameData(0, &block, 4, 384);
    REQUIRE_FALSE(odd.has_value());
    REQUIRE(odd.error().message.contains("power of two"));

    REQUIRE_FALSE(validateFrameData(0, &block, 4, 0).has_value());
}

//======================================================================================================================
// A heap with no bytes is a heap nothing can be placed in, which is a caller error rather than an
// empty success.
TEST_CASE("HeapDesc requires a size", "[rhi][validate]") {
    REQUIRE_FALSE(validate(HeapDesc{.size = 0}).has_value());
    REQUIRE(validate(HeapDesc{.size = 4096, .label = "lmx.test.heap"}).has_value());
}

//======================================================================================================================
// The three ways a placement can be wrong, each caught before the driver sees it: a resource the
// backend declined to size, an offset the resource's alignment does not divide, and a footprint
// running off the end of the heap. The last is checked at the exact boundary from both sides,
// because that is where an off-by-one lives.
TEST_CASE("a placement is checked against its heap and its alignment", "[rhi][validate]") {
    const FakeHeap heap{4096};

    REQUIRE_FALSE(validatePlacement(heap, 0, {.size = 0, .alignment = 256}).has_value());
    REQUIRE_FALSE(validatePlacement(heap, 0, {.size = 256, .alignment = 0}).has_value());

    REQUIRE_FALSE(validatePlacement(heap, 128, {.size = 256, .alignment = 256}).has_value());
    REQUIRE(validatePlacement(heap, 256, {.size = 256, .alignment = 256}).has_value());

    // Exactly filling the heap is legal; one byte of size past it is not.
    REQUIRE(validatePlacement(heap, 3840, {.size = 256, .alignment = 256}).has_value());
    REQUIRE_FALSE(validatePlacement(heap, 3840, {.size = 512, .alignment = 256}).has_value());
    REQUIRE_FALSE(validatePlacement(heap, 4096, {.size = 256, .alignment = 256}).has_value());
}

//======================================================================================================================
TEST_CASE("temporal scaler descriptor validates capacity format and support", "[rhi][validate]") {
    const TemporalScalerSupport support{true, 0.5f, 1.0f, "test"};
    TemporalScalerDesc desc{.inputWidth = 320,
                            .inputHeight = 180,
                            .outputWidth = 320,
                            .outputHeight = 180,
                            .minInputScale = 0.5f,
                            .maxInputScale = 1.0f};
    REQUIRE(validate(desc, support));
    SECTION("unavailable") {
        REQUIRE_FALSE(validate(desc, {}));
    }
    SECTION("empty extent") {
        desc.inputWidth = 0;
        REQUIRE_FALSE(validate(desc, support));
    }
    SECTION("unsupported format") {
        desc.motionFormat = Format::RGBA16Float;
        REQUIRE_FALSE(validate(desc, support));
    }
    SECTION("low scale") {
        desc.minInputScale = 0.25f;
        REQUIRE_FALSE(validate(desc, support));
    }
    SECTION("high scale") {
        desc.maxInputScale = 1.5f;
        REQUIRE_FALSE(validate(desc, support));
    }
    SECTION("reversed range") {
        desc.minInputScale = 1.0f;
        desc.maxInputScale = 0.5f;
        REQUIRE_FALSE(validate(desc, support));
    }
    SECTION("nan scale") {
        desc.minInputScale = std::numeric_limits<float>::quiet_NaN();
        REQUIRE_FALSE(validate(desc, support));
    }
    SECTION("insufficient capacity") {
        desc.inputWidth = 160;
        REQUIRE_FALSE(validate(desc, support));
    }
}

//======================================================================================================================
TEST_CASE("temporal scaler frame validates every input and content rectangle", "[rhi][validate]") {
    TemporalScalerDesc desc{.inputWidth = 320,
                            .inputHeight = 180,
                            .outputWidth = 320,
                            .outputHeight = 180,
                            .minInputScale = 0.5f,
                            .maxInputScale = 1.0f};
    FakeTexture color{320, 180, Format::RGBA16Float};
    FakeTexture depth{320, 180, Format::D32Float};
    FakeTexture motion{320, 180, Format::RG16Float};
    FakeTexture reactive{320, 180, Format::R8Unorm};
    FakeTexture exposure{1, 1, Format::R16Float};
    FakeTexture output{320, 180, Format::RGBA16Float};
    TemporalScaleParams params{.color = &color,
                               .depth = &depth,
                               .motion = &motion,
                               .reactive = &reactive,
                               .exposure = &exposure,
                               .output = &output,
                               .inputContentWidth = 160,
                               .inputContentHeight = 90};
    REQUIRE(validateTemporalScale(desc, params));
    SECTION("null inputs") {
        for (Texture** field : {&params.color, &params.depth, &params.motion, &params.reactive,
                                &params.exposure, &params.output}) {
            auto* saved = *field;
            *field = nullptr;
            REQUIRE_FALSE(validateTemporalScale(desc, params));
            *field = saved;
        }
    }
    SECTION("wrong format") {
        params.motion = &color;
        REQUIRE_FALSE(validateTemporalScale(desc, params));
    }
    SECTION("wrong extent") {
        FakeTexture wrong{160, 90, Format::RGBA16Float};
        params.output = &wrong;
        REQUIRE_FALSE(validateTemporalScale(desc, params));
    }
    SECTION("wrong exposure") {
        params.exposure = &reactive;
        REQUIRE_FALSE(validateTemporalScale(desc, params));
    }
    SECTION("too many mips") {
        FakeTexture wrong{320, 180, Format::RGBA16Float, 2};
        params.color = &wrong;
        REQUIRE_FALSE(validateTemporalScale(desc, params));
    }
    SECTION("output alias") {
        params.output = params.color;
        REQUIRE_FALSE(validateTemporalScale(desc, params));
    }
    SECTION("empty content") {
        params.inputContentHeight = 0;
        REQUIRE_FALSE(validateTemporalScale(desc, params));
    }
    SECTION("content below range") {
        params.inputContentWidth = 159;
        REQUIRE_FALSE(validateTemporalScale(desc, params));
    }
    SECTION("content beyond capacity") {
        params.inputContentHeight = 181;
        REQUIRE_FALSE(validateTemporalScale(desc, params));
    }
    SECTION("infinite motion scale") {
        params.motionScaleX = std::numeric_limits<float>::infinity();
        REQUIRE_FALSE(validateTemporalScale(desc, params));
    }
    SECTION("nan jitter") {
        params.jitterOffsetY = std::numeric_limits<float>::quiet_NaN();
        REQUIRE_FALSE(validateTemporalScale(desc, params));
    }
    SECTION("invalid exposure") {
        params.preExposure = 0;
        REQUIRE_FALSE(validateTemporalScale(desc, params));
    }
}
