#include "RHIValidateTestSupport.h"

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
