//----------------------------------------------------------------------------------------------------------------------
/// @file FormatDeathTests.cpp
/// @brief Death tests for the valid-usage rules of the formats chapter that need no texture to exercise.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Support/Death.h"

#include <catch2/catch_test_macros.hpp>

using namespace rojoRHI;

//======================================================================================================================
TEST_CASE("VU-rojo-format-0001 a Format value is not an enumerator", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-format-0001", [] {
        Device device = *createDevice();
        (void)device.formatCapabilities(static_cast<Format>(14));
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-format-0005 formatCapabilities is asked about Undefined", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-format-0005", [] {
        Device device = *createDevice();
        (void)device.formatCapabilities(Format::Undefined);
    });
}
