#include "AbiExamples.h"
#include "ShaderLayout.h"

#include <catch2/catch_test_macros.hpp>

//======================================================================================================================
// The layout asserts in ShaderLayout.h fail the build on drift; this case pins how many structs they cover.
TEST_CASE("the layout probe covers four records and three roots", "[abi]")
{
    CHECK(kShaderLayoutStructCount == 7);
}
