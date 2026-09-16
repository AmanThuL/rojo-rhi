#include "RHIValidateTestSupport.h"

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
    STATIC_REQUIRE(CommandList::kMaxBufferBindings == 16);
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
