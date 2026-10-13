//----------------------------------------------------------------------------------------------------------------------
/// @file CommandListCoreDeathTests.cpp
/// @brief Death tests for the rules of `CommandList::end` that no public entry reaches yet, driven through the core directly.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/CommandLists.h"
#include "Support/Death.h"

#include <catch2/catch_test_macros.hpp>

using namespace rojoRHI;

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0003 a list with a render pass open ends", "[core][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0003", [] {
        core::CommandListCore list;
        list.passOpen = true;
        core::endList(list);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0004 a list with a scope open ends", "[core][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0004", [] {
        core::CommandListCore list;
        list.openScopes = 1;
        core::endList(list);
    });
}
