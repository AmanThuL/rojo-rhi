#include <rojoRHI/RHI.h>

#include "Base/Assert.h"
#include "Base/Log.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <string>
#include <string_view>
#include <type_traits>

#ifdef _WIN32
#include <io.h>
#define ROJORHI_TEST_DUP _dup
#define ROJORHI_TEST_DUP2 _dup2
#define ROJORHI_TEST_CLOSE _close
#define ROJORHI_TEST_FILENO _fileno
#else
#include <unistd.h>
#define ROJORHI_TEST_DUP dup
#define ROJORHI_TEST_DUP2 dup2
#define ROJORHI_TEST_CLOSE close
#define ROJORHI_TEST_FILENO fileno
#endif

namespace {

    struct Received
    {
        int count = 0;
        rojoRHI::MessageSeverity severity = rojoRHI::MessageSeverity::Info;
        std::string text;
        void* user = nullptr;
    };

    Received gReceived;
    int gMarker = 0;

    //==================================================================================================================
    void recordMessage(rojoRHI::MessageSeverity severity, std::string_view message, void* user)
    {
        ++gReceived.count;
        gReceived.severity = severity;
        gReceived.text = message;
        gReceived.user = user;
    }

    //==================================================================================================================
    /// Runs `emit` with standard error redirected to a temporary file and returns what it wrote.
    template<class F> std::string captureStderr(F&& emit)
    {
        std::fflush(stderr);
        std::FILE* file = std::tmpfile();
        REQUIRE(file != nullptr);
        const int saved = ROJORHI_TEST_DUP(ROJORHI_TEST_FILENO(stderr));
        REQUIRE(saved >= 0);
        ROJORHI_TEST_DUP2(ROJORHI_TEST_FILENO(file), ROJORHI_TEST_FILENO(stderr));

        emit();

        std::fflush(stderr);
        ROJORHI_TEST_DUP2(saved, ROJORHI_TEST_FILENO(stderr));
        ROJORHI_TEST_CLOSE(saved);

        std::string text;
        std::rewind(file);
        char buffer[256];
        while (std::size_t n = std::fread(buffer, 1, sizeof buffer, file))
            text.append(buffer, n);
        std::fclose(file);
        return text;
    }

    template<class H>
    constexpr bool isHandleShaped = sizeof(H) == 8 && std::is_trivially_copyable_v<H> && std::is_standard_layout_v<H> && H{}.bits == 0 && H{} == H{};

} // namespace

//======================================================================================================================
TEST_CASE("a Result holds either a value or an Error", "[contract]")
{
    rojoRHI::Result<int> ok = 7;
    rojoRHI::Result<int> bad = std::unexpected(rojoRHI::Error{ rojoRHI::ErrorCategory::OutOfMemory, "heap is full" });
    rojoRHI::Result<void> none;

    REQUIRE(ok.has_value());
    CHECK(*ok == 7);
    REQUIRE_FALSE(bad.has_value());
    CHECK(bad.error().category == rojoRHI::ErrorCategory::OutOfMemory);
    CHECK(bad.error().message == "heap is full");
    CHECK(none.has_value());
}

//======================================================================================================================
TEST_CASE("every ErrorCategory enumerator exists and keeps its order", "[contract]")
{
    using rojoRHI::ErrorCategory;
    STATIC_REQUIRE(std::is_same_v<std::underlying_type_t<ErrorCategory>, uint8_t>);
    STATIC_REQUIRE(static_cast<uint8_t>(ErrorCategory::Unsupported) == 0);
    STATIC_REQUIRE(static_cast<uint8_t>(ErrorCategory::OutOfMemory) == 1);
    STATIC_REQUIRE(static_cast<uint8_t>(ErrorCategory::DriverFailure) == 2);
    STATIC_REQUIRE(static_cast<uint8_t>(ErrorCategory::ShaderAbiMismatch) == 3);
}

//======================================================================================================================
TEST_CASE("an Error owns a string and is not trivially copyable", "[contract]")
{
    STATIC_REQUIRE_FALSE(std::is_trivially_copyable_v<rojoRHI::Error>);
}

//======================================================================================================================
TEST_CASE("resource handles are eight trivially copyable zero-initialised comparable bytes", "[contract]")
{
    using namespace rojoRHI;
    STATIC_REQUIRE(isHandleShaped<Buffer>);
    STATIC_REQUIRE(isHandleShaped<Texture>);
    STATIC_REQUIRE(isHandleShaped<Pipeline>);
    STATIC_REQUIRE(isHandleShaped<ShaderLibrary>);
    STATIC_REQUIRE(isHandleShaped<Heap>);
    STATIC_REQUIRE(isHandleShaped<Fence>);
    STATIC_REQUIRE(isHandleShaped<Upscaler>);
}

//======================================================================================================================
TEST_CASE("resource handles with different bits compare unequal", "[contract]")
{
    rojoRHI::Buffer a{ 1 };
    rojoRHI::Buffer b{ 2 };
    CHECK(a == a);
    CHECK_FALSE(a == b);
    CHECK_FALSE(a == rojoRHI::Buffer{});
}

//======================================================================================================================
TEST_CASE("geometry types default as chapter 01 writes them", "[contract]")
{
    using namespace rojoRHI;
    const Extent2D e2;
    CHECK((e2.width == 0 && e2.height == 0));
    const Extent3D e3;
    CHECK((e3.width == 1 && e3.height == 1 && e3.depth == 1));
    const Offset3D o;
    CHECK((o.x == 0 && o.y == 0 && o.z == 0));
    const Rect2D r;
    CHECK((r.x == 0 && r.y == 0 && r.width == 0 && r.height == 0));
    STATIC_REQUIRE(std::is_trivially_copyable_v<Extent2D>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<Extent3D>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<Offset3D>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<Rect2D>);
}

//======================================================================================================================
TEST_CASE("an installed message callback receives severity, text and user", "[contract]")
{
    gReceived = {};
    rojoRHI::setMessageCallback(recordMessage, &gMarker);

    rojoRHI::base::emitMessage(rojoRHI::MessageSeverity::Warning, "careful");

    rojoRHI::setMessageCallback(nullptr, nullptr);
    CHECK(gReceived.count == 1);
    CHECK(gReceived.severity == rojoRHI::MessageSeverity::Warning);
    CHECK(gReceived.text == "careful");
    CHECK(gReceived.user == &gMarker);
}

//======================================================================================================================
TEST_CASE("the default message sink writes to standard error", "[contract]")
{
    rojoRHI::setMessageCallback(nullptr, nullptr);
    const std::string text = captureStderr([] { rojoRHI::base::emitMessage(rojoRHI::MessageSeverity::Info, "default sink line"); });
    CHECK(text.find("default sink line") != std::string::npos);
}

//======================================================================================================================
TEST_CASE("setMessageCallback with nullptr restores the default sink", "[contract]")
{
    gReceived = {};
    rojoRHI::setMessageCallback(recordMessage, nullptr);
    rojoRHI::setMessageCallback(nullptr, nullptr);

    const std::string text = captureStderr([] { rojoRHI::base::emitMessage(rojoRHI::MessageSeverity::Error, "after restore"); });

    CHECK(gReceived.count == 0);
    CHECK(text.find("after restore") != std::string::npos);
}

//======================================================================================================================
TEST_CASE("the inside-callback flag is set only while a callback runs", "[contract]")
{
    static bool seenInside = false;
    CHECK_FALSE(rojoRHI::base::insideMessageCallback());
    rojoRHI::setMessageCallback([](rojoRHI::MessageSeverity, std::string_view, void*) { seenInside = rojoRHI::base::insideMessageCallback(); }, nullptr);
    rojoRHI::base::emitMessage(rojoRHI::MessageSeverity::Info, "probe");
    rojoRHI::setMessageCallback(nullptr, nullptr);
    CHECK(seenInside);
    CHECK_FALSE(rojoRHI::base::insideMessageCallback());
}

//======================================================================================================================
TEST_CASE("a satisfied ROJORHI_VU does not terminate", "[contract]")
{
    // A volatile operand keeps the condition from folding to a constant, which would make the macro's failure path
    // statically dead and the code after it unreachable to the compiler.
    volatile int one = 1;
    ROJORHI_VU(one + one == 2, "VU-rojo-test-0000", "arithmetic holds {}", 2);
    SUCCEED();
}

//======================================================================================================================
TEST_CASE("ROJORHI_VU evaluates its condition once in Debug and never in Release", "[contract]")
{
    int evaluations = 0;
    ROJORHI_VU(++evaluations > 0, "VU-rojo-test-0000", "counter is positive");
#ifdef NDEBUG
    CHECK(evaluations == 0);
#else
    CHECK(evaluations == 1);
#endif
}
