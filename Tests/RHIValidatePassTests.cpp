#include "RHIValidateTestSupport.h"

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
