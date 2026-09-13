#include "RHIValidateTestSupport.h"

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
