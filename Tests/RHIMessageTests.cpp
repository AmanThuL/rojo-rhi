#include <catch2/catch_test_macros.hpp>
#include <rojoRHI/CaptureSchema.h>
#include <rojoRHI/Message.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using rojoRHI::MessageSeverity;

namespace {

// One installed sink's observations. A single file-local instance keeps the callback a plain
// function pointer, which is what the public contract accepts.
struct SinkRecord {
    std::vector<std::pair<MessageSeverity, std::string>> messages;
    void* user = nullptr;
    int calls = 0;
};

SinkRecord gRecord;

//======================================================================================================================
void recordMessage(MessageSeverity severity, std::string_view message, void* user) {
    gRecord.messages.emplace_back(severity, std::string(message));
    gRecord.user = user;
    ++gRecord.calls;
}

// Restores the stderr default when a case ends, so a later case never reaches a dangling sink.
struct SinkGuard {
    //==================================================================================================================
    ~SinkGuard() { rojoRHI::setMessageCallback(nullptr, nullptr); }
};

// The error the capture schema logs when its temporary file cannot be opened. No device is
// involved, so this is the cheapest deterministic RHI message a unit test can provoke.
constexpr std::string_view kUnwritablePath = "/nonexistent-dir/x.json";
constexpr std::string_view kExpectedText =
    "capture schema: cannot open '/nonexistent-dir/x.json.tmp' for writing";

//======================================================================================================================
void provokeErrorMessage() {
    REQUIRE_FALSE(rojoRHI::debug::CaptureSchema::instance().writeJson(kUnwritablePath));
}

//======================================================================================================================
// Counts the recorded messages that match the expected severity and text exactly.
size_t matchingMessages() {
    size_t count = 0;
    for (const auto& entry : gRecord.messages) {
        if (entry.first == MessageSeverity::Error && entry.second == kExpectedText) {
            ++count;
        }
    }
    return count;
}

} // namespace

//======================================================================================================================
TEST_CASE("the message callback receives severity and text", "[rhi][message]") {
    const SinkGuard guard;
    gRecord = SinkRecord{};
    rojoRHI::setMessageCallback(&recordMessage, nullptr);

    provokeErrorMessage();

    REQUIRE(matchingMessages() == 1);
}

//======================================================================================================================
TEST_CASE("the message callback returns its user pointer", "[rhi][message]") {
    const SinkGuard guard;
    gRecord = SinkRecord{};
    int marker = 0;
    rojoRHI::setMessageCallback(&recordMessage, &marker);

    provokeErrorMessage();

    REQUIRE(gRecord.calls > 0);
    REQUIRE(gRecord.user == &marker);
}

//======================================================================================================================
TEST_CASE("a null message callback restores the default", "[rhi][message]") {
    const SinkGuard guard;
    rojoRHI::setMessageCallback(&recordMessage, nullptr);
    rojoRHI::setMessageCallback(nullptr, nullptr);
    gRecord = SinkRecord{};

    provokeErrorMessage();

    REQUIRE(gRecord.calls == 0);
}
