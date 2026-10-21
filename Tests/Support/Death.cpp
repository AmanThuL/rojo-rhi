//----------------------------------------------------------------------------------------------------------------------
/// @file Death.cpp
/// @brief Implements the death-test helper with posix_spawn on macOS and CreateProcessW on Windows.
//----------------------------------------------------------------------------------------------------------------------

#include "Support/Death.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/interfaces/catch_interfaces_capture.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <crtdbg.h>
#include <stdlib.h>
#include <windows.h>
#elif defined(__APPLE__)
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#else
#error "The death-test helper supports macOS and Windows only; add the child spawn for this platform"
#endif

namespace rojoRHI::test {
    namespace {
        //==============================================================================================================
        /// Makes an abort, a failed CRT check or a crash end the process with its message on standard error, never with a
        /// modal dialog, which would hang a runner until its step times out. Only Windows shows such dialogs; elsewhere it
        /// does nothing.
        void quietAborts()
        {
#if defined(_WIN32)
            _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
            for ([[maybe_unused]] const int kind : { _CRT_WARN, _CRT_ERROR, _CRT_ASSERT })
            {
                _CrtSetReportMode(kind, _CRTDBG_MODE_FILE);
                _CrtSetReportFile(kind, _CRTDBG_FILE_STDERR);
            }
            SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
        }

        /// Quiets aborts once the test run starts, so an abort outside a death test fails the run at once, its message in
        /// the log; a death child quiets them again before it runs its body. On Windows it also leaves standard output and
        /// standard error unbuffered: an abort flushes neither, and a pipe would hold back the report of every case before it.
        class QuietAborts final : public Catch::EventListenerBase
        {
          public:
            using Catch::EventListenerBase::EventListenerBase;

            void testRunStarting(const Catch::TestRunInfo&) override;
        };

        //==============================================================================================================
        void QuietAborts::testRunStarting(const Catch::TestRunInfo&)
        {
            quietAborts();
#if defined(_WIN32)
            for (std::FILE* stream : { stdout, stderr })
            {
                std::fflush(stream);
                std::setvbuf(stream, nullptr, _IONBF, 0);
            }
#endif
        }

        struct ChildResult
        {
            bool launched = false;
            bool died = false;
            std::string status;
        };

        //==============================================================================================================
        /// Escapes the characters Catch2 treats specially in a test spec so the name selects exactly one case.
        std::string escapeTestSpec(const std::string& name)
        {
            std::string out;
            for (std::size_t i = 0; i < name.size(); ++i)
            {
                const char c = name[i];
                const bool special = c == ',' || c == '[' || c == ']' || c == '"' || c == '\\' || c == '*' || (i == 0 && c == '~');
                if (special)
                {
                    out += '\\';
                }
                out += c;
            }
            return out;
        }

        //==============================================================================================================
        std::string readFile(const std::filesystem::path& path)
        {
            std::ifstream in(path, std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }

        //==============================================================================================================
        /// The first violation report in `text` (a `VU-rojo-` identifier directly followed by a colon) through the end of its
        /// line, or an empty string. The default sink prefixes each line with "[RHI] [severity] ", and Catch2 echoes the test
        /// name, which may itself begin with an identifier, so the colon is what marks a report.
        std::string firstViolationLine(const std::string& text)
        {
            static const std::regex report("VU-rojo-[A-Za-z0-9-]+:[^\\n]*");
            std::smatch match;
            return std::regex_search(text, match, report) ? match.str() : std::string();
        }

        //==============================================================================================================
        /// Creates the empty capture file and returns its path, or an empty path with `reason` set.
        std::filesystem::path makeCapturePath(std::string& reason)
        {
            std::error_code error;
            const std::filesystem::path directory = std::filesystem::temp_directory_path(error);
            if (error)
            {
                reason = "temp_directory_path failed: " + error.message();
                return {};
            }
#if defined(_WIN32)
            wchar_t name[MAX_PATH]{};
            if (GetTempFileNameW(directory.c_str(), L"rrd", 0, name) == 0)
            {
                reason = "GetTempFileNameW failed with error " + std::to_string(GetLastError());
                return {};
            }
            return name;
#else
            std::string pattern = (directory / "rojorhi-death-XXXXXX").string();
            const int fd = mkstemp(pattern.data());
            if (fd < 0)
            {
                reason = "mkstemp failed with errno " + std::to_string(errno);
                return {};
            }
            close(fd);
            return pattern;
#endif
        }

        //==============================================================================================================
        /// Runs the current executable on `spec` with ROJORHI_DEATH_CHILD=1, standard error going to `capture` and standard
        /// output to the null device, and waits for it.
        ChildResult runChild(const std::string& spec, const std::filesystem::path& capture)
        {
            ChildResult result;
#if defined(_WIN32)
            const auto widen = [](const std::string& utf8) {
                if (utf8.empty())
                {
                    return std::wstring();
                }
                const int count = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
                std::wstring wide(static_cast<std::size_t>(count), L'\0');
                MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), count);
                return wide;
            };
            std::wstring exe(32768, L'\0');
            exe.resize(GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size())));

            SECURITY_ATTRIBUTES attributes{ sizeof(attributes), nullptr, TRUE };
            const HANDLE file = CreateFileW(capture.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &attributes, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            const HANDLE null =
                CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE || null == INVALID_HANDLE_VALUE)
            {
                result.status = "could not open the capture or null handle";
                if (file != INVALID_HANDLE_VALUE)
                {
                    CloseHandle(file);
                }
                if (null != INVALID_HANDLE_VALUE)
                {
                    CloseHandle(null);
                }
                return result;
            }

            const std::wstring command = std::wstring(L"\"") + exe + L"\" " + widen(quoteWindowsArgument(spec)) + L" --reporter compact";
            std::wstring mutableCommand = command;

            // The environment block is the parent's variables minus any stale marker, plus the child marker, double-NUL terminated.
            std::vector<std::wstring> parentEnvironment;
            if (wchar_t* block = GetEnvironmentStringsW(); block != nullptr)
            {
                for (const wchar_t* entry = block; *entry != L'\0'; entry += wcslen(entry) + 1)
                {
                    parentEnvironment.emplace_back(entry);
                }
                FreeEnvironmentStringsW(block);
            }
            std::wstring environment;
            for (const std::wstring& entry : withDeathChildMarker(parentEnvironment))
            {
                environment.append(entry);
                environment.push_back(L'\0');
            }
            environment.push_back(L'\0');

            // Limit inheritance to the two handles the child uses.
            HANDLE inherited[2] = { file, null };
            SIZE_T attributeSize = 0;
            InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
            std::vector<char> attributeStorage(attributeSize);
            auto* attributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
            const bool listed =
                InitializeProcThreadAttributeList(attributeList, 1, 0, &attributeSize) != FALSE &&
                UpdateProcThreadAttribute(attributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr) != FALSE;

            STARTUPINFOEXW startup{};
            startup.StartupInfo.cb = sizeof(startup);
            startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
            startup.StartupInfo.hStdInput = null;
            startup.StartupInfo.hStdOutput = null;
            startup.StartupInfo.hStdError = file;
            startup.lpAttributeList = listed ? attributeList : nullptr;

            PROCESS_INFORMATION process{};
            const BOOL created = CreateProcessW(
                exe.c_str(),
                mutableCommand.data(),
                nullptr,
                nullptr,
                TRUE,
                EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
                environment.data(),
                nullptr,
                &startup.StartupInfo,
                &process
            );
            if (listed)
            {
                DeleteProcThreadAttributeList(attributeList);
            }
            if (!created)
            {
                CloseHandle(file);
                CloseHandle(null);
                result.status = "CreateProcessW failed with error " + std::to_string(GetLastError());
                return result;
            }

            WaitForSingleObject(process.hProcess, INFINITE);
            DWORD code = 0;
            GetExitCodeProcess(process.hProcess, &code);
            CloseHandle(process.hProcess);
            CloseHandle(process.hThread);
            CloseHandle(file);
            CloseHandle(null);

            result.launched = true;
            result.died = code != 0;
            result.status = "exit code " + std::to_string(code);
            return result;
#else
            std::string exe(4096, '\0');
            std::uint32_t size = static_cast<std::uint32_t>(exe.size());
            if (_NSGetExecutablePath(exe.data(), &size) != 0)
            {
                result.status = "could not resolve the executable path";
                return result;
            }
            exe.resize(std::char_traits<char>::length(exe.c_str()));

            std::vector<std::string> parentEnvironment;
            for (char** entry = environ; *entry != nullptr; ++entry)
            {
                parentEnvironment.emplace_back(*entry);
            }
            std::vector<std::string> childEnvironment = withDeathChildMarker(parentEnvironment);
            std::vector<char*> env;
            for (std::string& entry : childEnvironment)
            {
                env.push_back(entry.data());
            }
            env.push_back(nullptr);

            std::string specArg = spec;
            std::string reporterFlag = "--reporter";
            std::string reporterName = "compact";
            std::vector<char*> args{ exe.data(), specArg.data(), reporterFlag.data(), reporterName.data(), nullptr };

            posix_spawn_file_actions_t actions;
            posix_spawn_file_actions_init(&actions);
            posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
            posix_spawn_file_actions_addopen(&actions, 2, capture.c_str(), O_WRONLY | O_TRUNC, 0600);

            pid_t pid = 0;
            const int spawned = posix_spawn(&pid, exe.c_str(), &actions, nullptr, args.data(), env.data());
            posix_spawn_file_actions_destroy(&actions);
            if (spawned != 0)
            {
                result.status = "posix_spawn failed with error " + std::to_string(spawned);
                return result;
            }

            int status = 0;
            pid_t waited = 0;
            do
            {
                waited = waitpid(pid, &status, 0);
            } while (waited < 0 && errno == EINTR);
            if (waited < 0)
            {
                result.status = "waitpid failed with errno " + std::to_string(errno);
                return result;
            }

            result.launched = true;
            if (WIFSIGNALED(status))
            {
                result.died = true;
                result.status = "terminated by signal " + std::to_string(WTERMSIG(status));
            }
            else
            {
                result.died = WEXITSTATUS(status) != 0;
                result.status = "exit code " + std::to_string(WEXITSTATUS(status));
            }
            return result;
#endif
        }
    } // namespace

    //==================================================================================================================
    std::string quoteWindowsArgument(std::string_view argument)
    {
        std::string out = "\"";
        std::size_t backslashes = 0;
        for (const char c : argument)
        {
            if (c == '\\')
            {
                ++backslashes;
                continue;
            }
            if (c == '"')
            {
                out.append(backslashes * 2 + 1, '\\');
            }
            else
            {
                out.append(backslashes, '\\');
            }
            backslashes = 0;
            out += c;
        }
        out.append(backslashes * 2, '\\');
        out += '"';
        return out;
    }

    //==================================================================================================================
    DeathRole deathRoleOf(const char* value)
    {
        if (value == nullptr)
        {
            return DeathRole::Parent;
        }
        return std::string_view(value) == "1" ? DeathRole::Child : DeathRole::Invalid;
    }

    namespace {
        //==============================================================================================================
        /// Runs the current case again in a child process that runs `body`; in the child itself it runs `body` and exits 0 when
        /// `body` returns. In the parent it requires the child to die with a non-zero status and `matches` to
        /// accept its captured standard error.
        void runDeathCase(const std::function<void()>& body, const std::function<bool(const std::string&)>& matches)
        {
#ifdef NDEBUG
            SKIP("death tests need a Debug build");
#endif
            const char* marker = std::getenv("ROJORHI_DEATH_CHILD");
            const DeathRole role = deathRoleOf(marker);
            if (role == DeathRole::Invalid)
            {
                FAIL(
                    "ROJORHI_DEATH_CHILD is set to \"" << marker
                                                       << "\" in the environment; it must be unset or 1, and a death test spawns nothing until it is unset"
                );
            }
            if (role == DeathRole::Child)
            {
                // A crash must end the child without a modal dialog that would hang a runner.
                quietAborts();
                body();
                std::exit(0);
            }

            const std::string name = Catch::getResultCapture().getCurrentTestName();
            std::string reason;
            const std::filesystem::path capture = makeCapturePath(reason);
            if (capture.empty())
            {
                FAIL("could not create the capture file: " << reason);
            }
            const ChildResult child = runChild(escapeTestSpec(name), capture);
            const std::string text = readFile(capture);
            std::error_code ignored;
            std::filesystem::remove(capture, ignored);

            INFO("child: " << child.status << "\ncaptured:\n" << text);
            REQUIRE(child.launched);
            REQUIRE(child.died);
            REQUIRE(matches(text));
        }
    } // namespace

    //==================================================================================================================
    void expectDeath(std::string_view id, const std::function<void()>& body)
    {
        const std::string expected = std::string(id) + ":";
        runDeathCase(body, [&](const std::string& text) { return firstViolationLine(text).starts_with(expected); });
    }

    //==================================================================================================================
    void expectAbort(std::string_view marker, const std::function<void()>& body)
    {
        runDeathCase(body, [&](const std::string& text) { return text.find(marker) != std::string::npos; });
    }
} // namespace rojoRHI::test

CATCH_REGISTER_LISTENER(rojoRHI::test::QuietAborts)
