#include <rojoRHI/RHI.h>

#include "Base/Log.h"
#include "Support/Death.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace {
    //==================================================================================================================
    void callSetMessageCallbackFromCallback(rojoRHI::MessageSeverity, std::string_view, void*)
    {
        rojoRHI::setMessageCallback(nullptr, nullptr);
    }

    //==================================================================================================================
    void keepEveryMessage(rojoRHI::MessageSeverity, std::string_view, void*)
    {
    }
} // namespace

//======================================================================================================================
TEST_CASE("VU-rojo-core-0008 a message callback calls setMessageCallback", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-core-0008", [] {
        rojoRHI::setMessageCallback(callSetMessageCallbackFromCallback, nullptr);
        rojoRHI::base::emitMessage(rojoRHI::MessageSeverity::Info, "trigger");
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0002 reaches stderr while a callback keeps every message", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0002", [] {
        rojoRHI::setMessageCallback(keepEveryMessage, nullptr);
        rojoRHI::Device device = *rojoRHI::createDevice();
        device.beginFrame();
        rojoRHI::CommandList list = device.acquireCommandList(rojoRHI::QueueType::Graphics, "test.list");
        list.end();
        list.end();
    });
}

// The two cases below check the harness itself; Catch2 reports each as passing only when the case fails.
//======================================================================================================================
TEST_CASE("the death harness fails a body that returns", "[contract][death][!shouldfail]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-core-0008", [] {});
}

//======================================================================================================================
TEST_CASE("the death harness fails a body that violates another rule", "[contract][death][!shouldfail]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-core-0008", [] {
        rojoRHI::base::emitMessage(rojoRHI::MessageSeverity::Error, "VU-rojo-core-9999: another rule");
        std::abort();
    });
}

//======================================================================================================================
TEST_CASE("quoteWindowsArgument follows the command-line quoting rule", "[contract]")
{
    CHECK(rojoRHI::test::quoteWindowsArgument("plain") == "\"plain\"");
    CHECK(rojoRHI::test::quoteWindowsArgument("two words") == "\"two words\"");
    CHECK(rojoRHI::test::quoteWindowsArgument("a\"b") == "\"a\\\"b\"");
    CHECK(rojoRHI::test::quoteWindowsArgument("end\\") == "\"end\\\\\"");
    CHECK(rojoRHI::test::quoteWindowsArgument("a\\\"b") == "\"a\\\\\\\"b\"");
    CHECK(rojoRHI::test::quoteWindowsArgument("a\\, b \\[c\\]") == "\"a\\, b \\[c\\]\"");
}

//======================================================================================================================
TEST_CASE("the child environment carries exactly one marker whatever the parent held", "[contract]")
{
    using rojoRHI::test::withDeathChildMarker;
    const std::vector<std::string> parent{ "PATH=/bin", "ROJORHI_DEATH_CHILD=0", "ROJORHI_DEATH_CHILD=", "ROJORHI_DEATH_CHILD_X=2", "HOME=/h" };
    const std::vector<std::string> expected{ "PATH=/bin", "ROJORHI_DEATH_CHILD_X=2", "HOME=/h", "ROJORHI_DEATH_CHILD=1" };
    CHECK(withDeathChildMarker(parent) == expected);
    CHECK(withDeathChildMarker(std::vector<std::string>{}) == std::vector<std::string>{ "ROJORHI_DEATH_CHILD=1" });
    CHECK(withDeathChildMarker(std::vector<std::string>{ "ROJORHI_DEATH_CHILD=1" }) == std::vector<std::string>{ "ROJORHI_DEATH_CHILD=1" });
}

//======================================================================================================================
TEST_CASE("the wide child environment drops the marker in any casing", "[contract]")
{
    using rojoRHI::test::withDeathChildMarker;
    const std::vector<std::wstring> parent{ L"Path=C:\\", L"rojorhi_death_child=7", L"RojoRHI_Death_Child=" };
    const std::vector<std::wstring> expected{ L"Path=C:\\", L"ROJORHI_DEATH_CHILD=1" };
    CHECK(withDeathChildMarker(parent) == expected);
}

//======================================================================================================================
TEST_CASE("only an unset marker is a parent and only 1 is a child", "[contract]")
{
    using rojoRHI::test::DeathRole;
    using rojoRHI::test::deathRoleOf;
    CHECK(deathRoleOf(nullptr) == DeathRole::Parent);
    CHECK(deathRoleOf("1") == DeathRole::Child);
    CHECK(deathRoleOf("0") == DeathRole::Invalid);
    CHECK(deathRoleOf("") == DeathRole::Invalid);
    CHECK(deathRoleOf("11") == DeathRole::Invalid);
}
