#include "RHIValidateTestSupport.h"

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
TEST_CASE("CPU buffer writes require upload permission and a real source range",
          "[rhi][scene-tables]") {
    const FakeBuffer buffer{256};
    const uint32_t value = 42;
    REQUIRE(validateBufferWrite(buffer, true, 0, &value, sizeof(value)));
    REQUIRE(validateBufferWrite(buffer, true, 252, &value, sizeof(value)));

    const auto denied = validateBufferWrite(buffer, false, 0, &value, sizeof(value));
    REQUIRE_FALSE(denied);
    REQUIRE(denied.error().code == ErrorCode::InvalidDesc);
    REQUIRE(denied.error().message.contains("cpuWrite"));
    REQUIRE_FALSE(validateBufferWrite(buffer, true, 0, nullptr, sizeof(value)));
    REQUIRE_FALSE(validateBufferWrite(buffer, true, 0, &value, 0));
    REQUIRE_FALSE(validateBufferWrite(buffer, true, 256, &value, sizeof(value)));
    REQUIRE_FALSE(validateBufferWrite(buffer, true, 253, &value, sizeof(value)));
    REQUIRE_FALSE(
        validateBufferWrite(buffer, true, 8, &value, std::numeric_limits<uint64_t>::max()));
    REQUIRE_FALSE(validateBufferWrite(buffer, true, std::numeric_limits<uint64_t>::max(), &value,
                                      sizeof(value)));
}
