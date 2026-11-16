//----------------------------------------------------------------------------------------------------------------------
/// @file DrawCoreDeathTests.cpp
/// @brief Death tests for the draw rules that a device reporting every feature cannot reach through the public entries alone.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/DeviceCore.h"
#include "Support/Death.h"
#include "Support/PassKit.h"

#include <catch2/catch_test_macros.hpp>

#include <array>

using namespace rojoRHI;
using test::PassKit;

//======================================================================================================================
TEST_CASE("VU-rojo-indirect-0013 drawMeshTasksIndirect is recorded on a device without meshTasksIndirect", "[core][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-indirect-0013", [] {
        PassKit kit;
        kit.readyToDraw(true);
        const Addr args = kit.device.address(kit.buffer(BufferUsage::IndirectArgs));
        core::coreOf(kit.device).capabilities().meshTasksIndirect = false;
        kit.list.drawMeshTasksIndirect(args, 1);
    });
}
