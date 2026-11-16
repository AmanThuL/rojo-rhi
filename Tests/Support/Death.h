//----------------------------------------------------------------------------------------------------------------------
/// @file Death.h
/// @brief Declares the death-test helper that re-runs one test case in a child process.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace rojoRHI::test {
    /// Runs `body` in a child process and requires it to terminate with a non-zero status after writing a violation report
    /// to standard error. The report is the first occurrence of `VU-rojo-<name>:` in the captured text, not the first line:
    /// the default sink prefixes each line with "[RHI] [error] " and a platform layer may print earlier. It must be `id`
    /// followed by a colon. In the child (`ROJORHI_DEATH_CHILD=1`) it runs `body` and exits 0 when `body` returns. Release
    /// builds skip the case, because valid-usage checks do not exist there.
    void expectDeath(std::string_view id, const std::function<void()>& body);

    /// Runs `body` in a child process and requires it to terminate with a non-zero status after writing `marker` to standard
    /// error, anywhere in the captured text and without a `VU-rojo-` prefix: for an abort that no valid-usage rule raises, such
    /// as one a native layer's error message causes. Release builds skip the case.
    void expectAbort(std::string_view marker, const std::function<void()>& body);

    /// What the `ROJORHI_DEATH_CHILD` variable makes the current process: no variable is a parent, exactly "1" is a child
    /// and any other value is invalid, because a child spawned from such a parent could not tell itself apart.
    enum class DeathRole
    {
        Parent,
        Child,
        Invalid
    };

    /// Classifies the value of `ROJORHI_DEATH_CHILD`; null means the variable is not set.
    DeathRole deathRoleOf(const char* value);

    /// Returns `environment`, a list of `NAME=value` entries, without any `ROJORHI_DEATH_CHILD` entry and with the child
    /// marker `ROJORHI_DEATH_CHILD=1` appended, so the child sees exactly one such entry. Windows names are case-insensitive,
    /// so the wide form drops every casing.
    template<typename String> std::vector<String> withDeathChildMarker(const std::vector<String>& environment)
    {
        constexpr std::string_view name = "ROJORHI_DEATH_CHILD=";
        const auto isMarker = [&](const String& entry) {
            if (entry.size() < name.size())
            {
                return false;
            }
            for (std::size_t i = 0; i < name.size(); ++i)
            {
                auto c = entry[i];
                if constexpr (!std::is_same_v<String, std::string>)
                {
                    c = (c >= 'a' && c <= 'z') ? static_cast<decltype(c)>(c - 'a' + 'A') : c;
                }
                if (c != name[i])
                {
                    return false;
                }
            }
            return true;
        };
        std::vector<String> out;
        for (const String& entry : environment)
        {
            if (!isMarker(entry))
            {
                out.push_back(entry);
            }
        }
        String marker(name.begin(), name.end());
        marker.push_back('1');
        out.push_back(std::move(marker));
        return out;
    }

    /// Quotes `argument` as one Windows command-line argument: backslashes are literal except a run before a quote or the
    /// closing quote, which is doubled, and each embedded quote gets one backslash.
    std::string quoteWindowsArgument(std::string_view argument);
} // namespace rojoRHI::test

/// Asserts that the body violates the valid-usage rule `id` and aborts the process. The body is variadic so a comma outside
/// parentheses, in a braced initializer or a template argument list, does not split it.
#define ROJORHI_EXPECT_DEATH(id, ...) ::rojoRHI::test::expectDeath(id, __VA_ARGS__)

/// Asserts that the body aborts the process after writing `marker` to standard error. The body is variadic for the same reason.
#define ROJORHI_EXPECT_ABORT(marker, ...) ::rojoRHI::test::expectAbort(marker, __VA_ARGS__)
