#include <rojoRHI/RHI.h>

#include <catch2/catch_test_macros.hpp>

//======================================================================================================================
TEST_CASE("the version macros name interface 0.1.0", "[contract]")
{
    STATIC_REQUIRE(ROJORHI_VERSION_MAJOR == 0);
    STATIC_REQUIRE(ROJORHI_VERSION_MINOR == 1);
    STATIC_REQUIRE(ROJORHI_VERSION_PATCH == 0);
}
