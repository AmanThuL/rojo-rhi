//----------------------------------------------------------------------------------------------------------------------
/// @file PrecisionModuleDiagnostics.cpp
/// @brief Retains own-process runtime mappings at the precision result assertion when explicitly requested.
//----------------------------------------------------------------------------------------------------------------------
#include <catch2/catch_assertion_info.hpp>
#include <catch2/catch_test_case_info.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef PSAPI_VERSION
#define PSAPI_VERSION 2
#endif
#include <Windows.h>
#include <psapi.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <locale>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
    constexpr std::string_view kCase = "AbiPrecise preserves separate dynamic fp32 multiply and add";
    constexpr std::string_view kSource = "PrecisionConformanceTests.cpp";
    constexpr std::string_view kExpression = "resultBits == 0u";
    constexpr size_t kLine = 75;

    struct PathResult
    {
        std::wstring text;
        DWORD capacity = 0;
    };

    struct Image
    {
        PathResult path;
        uintptr_t base = 0;
        DWORD size = 0;
    };

    struct EnumerationAttempt
    {
        bool success = false;
        DWORD error = 0;
        DWORD capacityBytes = 0;
        DWORD neededBytes = 0;
    };

    struct Snapshot
    {
        std::vector<EnumerationAttempt> attempts;
        std::vector<Image> modules;
        std::vector<std::string> errors;
    };

    //==================================================================================================================
    PathResult modulePath(HMODULE module)
    {
        for (DWORD capacity = MAX_PATH;; capacity = std::min<DWORD>(capacity * 2, 32768))
        {
            std::wstring value(capacity, L'\0');
            const DWORD length = GetModuleFileNameW(module, value.data(), capacity);
            if (length == 0)
            {
                throw std::runtime_error(std::format("GetModuleFileNameW failed: {}", GetLastError()));
            }
            if (length < capacity)
            {
                value.resize(length);
                return { std::move(value), capacity };
            }
            if (capacity == 32768)
            {
                throw std::runtime_error("GetModuleFileNameW path remains truncated at its explicit character budget");
            }
        }
    }

    //==================================================================================================================
    Snapshot enumerateModules()
    {
        Snapshot result;
        std::vector<HMODULE> handles(256);
        bool complete = false;
        for (unsigned attempt = 0; attempt < 3; ++attempt)
        {
            EnumerationAttempt observed;
            observed.capacityBytes = static_cast<DWORD>(handles.size() * sizeof(HMODULE));
            observed.success = K32EnumProcessModules(GetCurrentProcess(), handles.data(), observed.capacityBytes, &observed.neededBytes) != FALSE;
            observed.error = observed.success ? 0 : GetLastError();
            result.attempts.push_back(observed);
            if (!observed.success)
            {
                result.errors.push_back(std::format("K32EnumProcessModules failed: {}", observed.error));
                break;
            }
            if (observed.neededBytes == 0 || observed.neededBytes % sizeof(HMODULE) != 0 || observed.neededBytes / sizeof(HMODULE) > 4096)
            {
                result.errors.push_back("module enumeration returned an invalid size or exceeded its explicit handle budget");
                break;
            }
            if (observed.neededBytes <= observed.capacityBytes)
            {
                handles.resize(observed.neededBytes / sizeof(HMODULE));
                complete = true;
                break;
            }
            handles.resize(observed.neededBytes / sizeof(HMODULE));
        }
        if (!complete)
        {
            result.errors.push_back("module enumeration did not complete within three sizing observations");
            return result;
        }
        for (HMODULE handle : handles)
        {
            Image image;
            image.base = reinterpret_cast<uintptr_t>(handle);
            try
            {
                MODULEINFO info{};
                if (!K32GetModuleInformation(GetCurrentProcess(), handle, &info, static_cast<DWORD>(sizeof(info))))
                {
                    throw std::runtime_error(std::format("K32GetModuleInformation failed: {}", GetLastError()));
                }
                if (reinterpret_cast<uintptr_t>(info.lpBaseOfDll) != image.base)
                {
                    throw std::runtime_error("module handle and reported base disagree");
                }
                image.size = info.SizeOfImage;
                image.path = modulePath(handle);
            }
            catch (const std::exception& error)
            {
                result.errors.push_back(error.what());
            }
            result.modules.push_back(std::move(image));
        }
        std::ranges::sort(result.modules, {}, &Image::base);
        for (size_t i = 1; i < result.modules.size(); ++i)
        {
            if (result.modules[i - 1].base == result.modules[i].base)
            {
                result.errors.push_back("module enumeration repeated an image instance");
            }
        }
        return result;
    }

    //==================================================================================================================
    void jsonString(std::ostream& out, std::string_view text)
    {
        out << '"';
        for (const char character : text)
        {
            const auto byte = static_cast<unsigned char>(character);
            if (byte == '"' || byte == '\\')
            {
                out << '\\' << static_cast<char>(byte);
            }
            else if (byte < 0x20)
            {
                out << std::format("\\u{:04x}", byte);
            }
            else
            {
                out << static_cast<char>(byte);
            }
        }
        out << '"';
    }

    //==================================================================================================================
    void jsonString(std::ostream& out, std::wstring_view text)
    {
        out << '"';
        for (const wchar_t character : text)
        {
            const auto word = static_cast<uint16_t>(character);
            if (word == '"' || word == '\\')
            {
                out << '\\' << static_cast<char>(word);
            }
            else if (word < 0x20 || word > 0x7E)
            {
                out << std::format("\\u{:04x}", word);
            }
            else
            {
                out << static_cast<char>(word);
            }
        }
        out << '"';
    }

    //==================================================================================================================
    void writeSnapshot(std::ostream& out, const Snapshot& snapshot)
    {
        out << "{\"complete\":" << snapshot.errors.empty() << ",\"errors\":[";
        for (size_t i = 0; i < snapshot.errors.size(); ++i)
        {
            if (i != 0)
                out << ',';
            jsonString(out, snapshot.errors[i]);
        }
        out << "],\"attempts\":[";
        for (size_t i = 0; i < snapshot.attempts.size(); ++i)
        {
            if (i != 0)
                out << ',';
            const auto& attempt = snapshot.attempts[i];
            out << "{\"success\":" << attempt.success << ",\"error\":" << attempt.error << ",\"capacityBytes\":" << attempt.capacityBytes
                << ",\"neededBytes\":" << attempt.neededBytes << '}';
        }
        out << "],\"modules\":[";
        for (size_t i = 0; i < snapshot.modules.size(); ++i)
        {
            if (i != 0)
                out << ',';
            const auto& image = snapshot.modules[i];
            out << "{\"path\":";
            jsonString(out, image.path.text);
            out << ",\"base\":\"" << std::format("0x{:x}", image.base) << "\",\"size\":" << image.size << ",\"pathCharacters\":" << image.path.text.size()
                << ",\"pathCapacity\":" << image.path.capacity << '}';
        }
        out << "]}";
    }

    //==================================================================================================================
    bool sameImages(const Snapshot& a, const Snapshot& b)
    {
        return std::ranges::equal(a.modules, b.modules, [](const Image& left, const Image& right) {
            return left.base == right.base && left.size == right.size && left.path.text == right.path.text;
        });
    }

    //==================================================================================================================
    void recordModules(const std::filesystem::path& output, const Catch::AssertionInfo& assertion)
    {
        const PathResult executable = modulePath(nullptr);
        const Snapshot first = enumerateModules();
        const Snapshot second = enumerateModules();
        const bool stable = sameImages(first, second);
        std::ofstream out(output, std::ios::binary | std::ios::noreplace);
        if (!out)
        {
            throw std::runtime_error("cannot exclusively create mapped-image output");
        }
        out.imbue(std::locale::classic());
        out << std::boolalpha << "{\"schemaVersion\":1,\"complete\":" << (stable && first.errors.empty() && second.errors.empty()) << ",\"errors\":[";
        if (!stable)
            jsonString(out, "module instances changed between two complete observations");
        out << "],\"case\":";
        jsonString(out, kCase);
        out << ",\"sourceFile\":";
        jsonString(out, std::string_view{ assertion.lineInfo.file });
        out << ",\"sourceLine\":" << assertion.lineInfo.line << ",\"expression\":";
        jsonString(out, std::string_view{ assertion.capturedExpression.data(), assertion.capturedExpression.size() });
        out << ",\"phase\":\"assertionStarting\",\"processId\":" << GetCurrentProcessId() << ",\"pointerBytes\":" << sizeof(HMODULE) << ",\"executablePath\":";
        jsonString(out, executable.text);
        out << ",\"snapshots\":[";
        writeSnapshot(out, first);
        out << ',';
        writeSnapshot(out, second);
        out << "]}\n";
        out.flush();
        if (!out)
        {
            throw std::runtime_error("mapped-image output write failed");
        }
        out.close();
        if (!out)
        {
            throw std::runtime_error("mapped-image output close failed");
        }
    }

    class PrecisionModules final : public Catch::EventListenerBase
    {
      public:
        using Catch::EventListenerBase::EventListenerBase;
        void testCaseStarting(const Catch::TestCaseInfo& info) override;
        void assertionStarting(const Catch::AssertionInfo& info) override;

      private:
        bool m_selected = false;
        bool m_attempted = false;
    };

    //==================================================================================================================
    void PrecisionModules::testCaseStarting(const Catch::TestCaseInfo& info)
    {
        m_selected = info.name == kCase;
        m_attempted = false;
    }

    //==================================================================================================================
    void PrecisionModules::assertionStarting(const Catch::AssertionInfo& info)
    {
        const std::string_view source{ info.lineInfo.file };
        const std::string_view expression{ info.capturedExpression.data(), info.capturedExpression.size() };
        const size_t slash = source.find_last_of("\\/");
        if (!m_selected || m_attempted || info.lineInfo.line != kLine || source.substr(slash == source.npos ? 0 : slash + 1) != kSource ||
            expression != kExpression)
        {
            return;
        }
        const DWORD length = GetEnvironmentVariableW(L"ROJORHI_PRECISION_MODULES_PATH", nullptr, 0);
        if (length == 0)
        {
            return;
        }
        m_attempted = true;
        try
        {
            if (length > 32768)
                throw std::runtime_error("mapped-image output path exceeds its explicit budget");
            std::wstring value(length, L'\0');
            const DWORD copied = GetEnvironmentVariableW(L"ROJORHI_PRECISION_MODULES_PATH", value.data(), length);
            if (copied == 0 || copied >= length)
                throw std::runtime_error("mapped-image output environment changed or is incomplete");
            value.resize(copied);
            const std::filesystem::path output{ value };
            if (!output.is_absolute() || output.filename() != L"mapped-modules.json")
                throw std::runtime_error("mapped-image output path differs");
            recordModules(output, info);
        }
        catch (const std::exception& error)
        {
            std::fprintf(stderr, "precision mapped-image observation: %s\n", error.what());
        }
        catch (...)
        {
            std::fputs("precision mapped-image observation: unknown failure\n", stderr);
        }
    }
} // namespace

CATCH_REGISTER_LISTENER(PrecisionModules)
