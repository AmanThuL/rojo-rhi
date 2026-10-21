//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Capture.cpp
/// @brief Implements programmatic GPU capture through the PIX GPU capturer for the D3D12 backend.
//----------------------------------------------------------------------------------------------------------------------

#include "D3D12Capture.h"

#include <rojoRHI/RHI.h>

#include "Base/Log.h"
#include "Core/Captures.h"
#include "Core/Counters.h"
#include "Core/DeviceCore.h"
#include "D3D12Common.h"
#include "D3D12Device.h"

#include <KnownFolders.h>
#include <shlobj.h>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace rojoRHI {
    namespace {
        /// The capturer's module name, which also identifies it once loaded.
        constexpr const wchar_t* kCapturerModule = L"WinPixGpuCapturer.dll";

        /// What `BeginProgrammaticGpuCapture` reads: the first member of PIX's `PIXCaptureParameters` union, padded to the size
        /// of the union's largest member so the capturer never reads past it.
        struct CaptureParameters
        {
            PCWSTR fileName = nullptr;
            std::byte reserved[96] = {};
        };

        // pix3.h declares the union as GpuCaptureParameters { PCWSTR FileName; } beside a TimingCaptureParameters of a file name
        // and thirteen 4-byte members, so the file name sits at offset 0 and the largest member is under 96 bytes after it.

        using BeginCaptureFn = HRESULT(WINAPI*)(const CaptureParameters*);
        using EndCaptureFn = HRESULT(WINAPI*)();

        //==============================================================================================================
        /// Returns the capturer's export `name` as a `Function`, or null while the capturer is not loaded or lacks it.
        template<class Function> Function capturerExport(const char* name)
        {
            const HMODULE module = GetModuleHandleW(kCapturerModule);
            return module != nullptr ? reinterpret_cast<Function>(GetProcAddress(module, name)) : nullptr;
        }

        //==============================================================================================================
        /// Returns the numbers in the runs of digits of `name`, in order.
        std::vector<uint64_t> versionParts(const std::wstring& name)
        {
            std::vector<uint64_t> parts;
            bool inRun = false;
            for (const wchar_t character : name)
            {
                if (character >= L'0' && character <= L'9')
                {
                    if (!inRun)
                    {
                        parts.push_back(0);
                        inRun = true;
                    }
                    parts.back() = parts.back() * 10 + static_cast<uint64_t>(character - L'0');
                }
                else
                {
                    inRun = false;
                }
            }
            return parts;
        }
    } // namespace

    namespace d3d12 {
        //==============================================================================================================
        std::optional<size_t> newestVersion(std::span<const std::wstring> names)
        {
            std::optional<size_t> newest;
            std::vector<uint64_t> newestParts;
            for (size_t index = 0; index < names.size(); ++index)
            {
                std::vector<uint64_t> parts = versionParts(names[index]);
                if (!newest.has_value() || parts > newestParts)
                {
                    newest = index;
                    newestParts = std::move(parts);
                }
            }
            return newest;
        }

        //==============================================================================================================
        std::wstring wide(std::string_view text)
        {
            if (text.empty())
            {
                return {};
            }
            const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
            if (length <= 0)
            {
                return {};
            }
            std::wstring converted(static_cast<size_t>(length), L'\0');
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), converted.data(), length);
            return converted;
        }

        //==============================================================================================================
        bool loadCapturer()
        {
            if (GetModuleHandleW(kCapturerModule) != nullptr)
            {
                return true;
            }
            PWSTR programFiles = nullptr;
            const HRESULT known = SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &programFiles);
            if (FAILED(known))
            {
                CoTaskMemFree(programFiles);
                return false;
            }
            const std::filesystem::path installs = std::filesystem::path(programFiles) / L"Microsoft PIX";
            CoTaskMemFree(programFiles);

            std::vector<std::wstring> versions;
            std::error_code listError;
            for (std::filesystem::directory_iterator it(installs, listError), end; !listError && it != end; it.increment(listError))
            {
                std::error_code entryError;
                if (it->is_directory(entryError) && std::filesystem::is_regular_file(it->path() / kCapturerModule, entryError))
                {
                    versions.push_back(it->path().filename().wstring());
                }
            }
            const std::optional<size_t> newest = newestVersion(versions);
            if (!newest.has_value())
            {
                return false;
            }
            const std::filesystem::path capturer = installs / versions[*newest] / kCapturerModule;
            if (LoadLibraryExW(capturer.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR) == nullptr)
            {
                ROJORHI_LOG_WARN("the PIX GPU capturer {} failed to load with error {}", utf8(capturer.wstring()), GetLastError());
                return false;
            }
            return true;
        }

        //==============================================================================================================
        Result<void> startCapture(std::string_view path)
        {
            const auto begin = capturerExport<BeginCaptureFn>("BeginProgrammaticGpuCapture");
            if (begin == nullptr)
            {
                return std::unexpected(
                    Error{
                        ErrorCategory::Unsupported,
                        "the PIX GPU capturer is not loaded; install PIX and create the device with DeviceDesc::enableCapture",
                    }
                );
            }
            const std::wstring name = wide(path);
            if (name.empty())
            {
                // The core refuses an empty path, so an empty conversion means the bytes are not UTF-8. The error enum has no
                // invalid-argument category, so the capture is refused as unsupported.
                return std::unexpected(Error{ ErrorCategory::Unsupported, std::format("the capture path \"{}\" is not valid UTF-8", path) });
            }
            // The capturer writes the file where the process runs it from, so the path names an absolute location.
            std::error_code pathError;
            const std::filesystem::path absolute = std::filesystem::absolute(std::filesystem::path(name), pathError);
            if (pathError)
            {
                return std::unexpected(
                    Error{ ErrorCategory::Unsupported, std::format("cannot resolve the capture path \"{}\": {}", path, pathError.message()) }
                );
            }
            CaptureParameters parameters;
            parameters.fileName = absolute.c_str();
            if (const HRESULT started = begin(&parameters); FAILED(started))
            {
                return std::unexpected(hresultError(started, std::format("the PIX GPU capturer's capture to \"{}\"", utf8(absolute.wstring()))));
            }
            return {};
        }

        //==============================================================================================================
        Result<void> refuseCapture(std::string_view)
        {
            return std::unexpected(
                Error{ ErrorCategory::Unsupported, "the device was created without DeviceDesc::enableCapture, which loads the PIX GPU capturer" }
            );
        }
    } // namespace d3d12

    //==================================================================================================================
    Result<void> Device::beginCapture(std::string_view path)
    {
        DeviceImpl& impl = DeviceImpl::of(*this);
        Result<void> started = core::beginCapture(impl.core, path);
        if (started.has_value())
        {
            core::DeviceCounts::add(impl.core.counts().nativeHostCalls);
        }
        return started;
    }

    //==================================================================================================================
    void Device::endCapture()
    {
        DeviceImpl& impl = DeviceImpl::of(*this);
        core::endCapture(impl.core);
        // The capturer is process-wide: in Release, where VU-rojo-diag-0010 is unchecked, an unmatched call may stop another
        // device's capture, which the caller is responsible for avoiding.
        const auto end = capturerExport<EndCaptureFn>("EndProgrammaticGpuCapture");
        if (end == nullptr)
        {
            return;
        }
        core::DeviceCounts::add(impl.core.counts().nativeHostCalls);
        if (const HRESULT ended = end(); FAILED(ended))
        {
            ROJORHI_LOG_ERROR("the PIX GPU capturer failed to finish the capture with HRESULT 0x{:08X}", d3d12::hresultBits(ended));
        }
    }
} // namespace rojoRHI
