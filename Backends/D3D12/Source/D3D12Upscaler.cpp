//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Upscaler.cpp
/// @brief Implements the FidelityFX runtime's loading, the FSR capability, upscaler creation and the upscaler retire hook.
//----------------------------------------------------------------------------------------------------------------------

#include "D3D12Upscaler.h"

#include <rojoRHI/RHI.h>

#include "Base/Assert.h"
#include "Base/Log.h"
#include "Core/DeviceCore.h"
#include "Core/HandlePool.h"
#include "Core/Records.h"
#include "Core/Upscalers.h"
#include "D3D12Common.h"
#include "D3D12Device.h"

#include <api/include/dx12/ffx_api_dx12.h>
#include <api/include/ffx_api.h>
#include <api/include/ffx_api_types.h>
#include <upscalers/include/ffx_upscale.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace rojoRHI {
    namespace {
        /// The file name of the FidelityFX loader, which loads the upscaler module beside it.
        constexpr const wchar_t* kLoaderName = L"amd_fidelityfx_loader_dx12.dll";

        /// What an `Auto` or `FSR` refusal adds when no runtime loaded, so the caller learns which file is missing.
        constexpr std::string_view kMissingRuntime =
            "; FSR needs the FidelityFX runtime, amd_fidelityfx_loader_dx12.dll beside the executable, and none loaded";

        /// The return code of a FidelityFX call that succeeded, as the type the calls return.
        constexpr ffxReturnCode_t kOk = FFX_API_RETURN_OK;

        //==============================================================================================================
        /// Returns the process-wide mutex held around every FidelityFX query, context creation and context destruction: the
        /// loader is one per process, whichever device loaded it, and documents no thread safety. It is a leaf.
        std::mutex& fidelityFxCalls()
        {
            static std::mutex mutex;
            return mutex;
        }

        //==============================================================================================================
        /// Returns the name of the FidelityFX return code `code`.
        const char* returnCodeName(ffxReturnCode_t code)
        {
            switch (code)
            {
            case FFX_API_RETURN_OK: return "FFX_API_RETURN_OK";
            case FFX_API_RETURN_ERROR: return "FFX_API_RETURN_ERROR";
            case FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE: return "FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE";
            case FFX_API_RETURN_ERROR_RUNTIME_ERROR: return "FFX_API_RETURN_ERROR_RUNTIME_ERROR";
            case FFX_API_RETURN_NO_PROVIDER: return "FFX_API_RETURN_NO_PROVIDER";
            case FFX_API_RETURN_ERROR_MEMORY: return "FFX_API_RETURN_ERROR_MEMORY";
            case FFX_API_RETURN_ERROR_PARAMETER: return "FFX_API_RETURN_ERROR_PARAMETER";
            case FFX_API_RETURN_PROVIDER_NO_SUPPORT_NEW_DESCTYPE: return "FFX_API_RETURN_PROVIDER_NO_SUPPORT_NEW_DESCTYPE";
            }
            return "an unknown return code";
        }

        //==============================================================================================================
        /// Resolves the export `name` of `module` into `entry`; returns false, and logs a warning, when it is missing.
        template<typename T> bool resolve(HMODULE module, const char* name, T& entry)
        {
            entry = reinterpret_cast<T>(GetProcAddress(module, name));
            if (entry == nullptr)
            {
                ROJORHI_LOG_WARN("the FidelityFX loader exports no {} (error {}), so FSR is unavailable", name, GetLastError());
                return false;
            }
            return true;
        }

        //==============================================================================================================
        /// Returns the `ffxCreateContextDescUpscale` flags of `flags`. Every upscaler takes a per-frame content extent, so
        /// dynamic resolution is always enabled.
        uint32_t createFlags(UpscalerFlags flags)
        {
            uint32_t native = static_cast<uint32_t>(FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION);
            if ((flags & UpscalerFlags::HDR) != UpscalerFlags::None)
            {
                native |= static_cast<uint32_t>(FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE);
            }
            if ((flags & UpscalerFlags::ReversedDepth) != UpscalerFlags::None)
            {
                native |= static_cast<uint32_t>(FFX_UPSCALE_ENABLE_DEPTH_INVERTED);
            }
            if ((flags & UpscalerFlags::AutoExposure) != UpscalerFlags::None)
            {
                native |= static_cast<uint32_t>(FFX_UPSCALE_ENABLE_AUTO_EXPOSURE);
            }
            return native;
        }

        //==============================================================================================================
        /// Returns the error of a creation of `record` that `ffxCreateContext` refused with `code`: `OutOfMemory` for a failed
        /// allocation, `Unsupported` when no provider takes the description, `DriverFailure` otherwise.
        Error refusal(const core::UpscalerRecord& record, ffxReturnCode_t code)
        {
            const ffxReturnCode_t memory = FFX_API_RETURN_ERROR_MEMORY;
            const ffxReturnCode_t noProvider = FFX_API_RETURN_NO_PROVIDER;
            const ErrorCategory category = code == memory       ? ErrorCategory::OutOfMemory
                                           : code == noProvider ? ErrorCategory::Unsupported
                                                                : ErrorCategory::DriverFailure;
            return Error{ category,
                          std::format(
                              "ffxCreateContext refused FSR upscaler \"{}\" of {}x{} to {}x{} with {} ({})",
                              record.label,
                              record.input.width,
                              record.input.height,
                              record.output.width,
                              record.output.height,
                              returnCodeName(code),
                              code
                          ) };
        }

        //==============================================================================================================
        /// Creates the FSR upscale context of `record` on `impl`'s device: `ffxCreateContextDescUpscale` with the input as the
        /// largest render size and the output as the upscale size, chained to the version the headers declare and the DX12
        /// backend description. Creation is synchronous and takes the FidelityFX mutex. A FidelityFX context is no
        /// `ID3D12Object`, so it takes no name.
        Result<ffxContext> createContext(DeviceImpl& impl, const core::UpscalerRecord& record)
        {
            ffxCreateBackendDX12Desc backend = {};
            backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
            backend.device = impl.device();
            ffxCreateContextDescUpscaleVersion version = {};
            version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
            version.header.pNext = &backend.header;
            version.version = FFX_UPSCALER_VERSION;
            ffxCreateContextDescUpscale upscale = {};
            upscale.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
            upscale.header.pNext = &version.header;
            upscale.flags = createFlags(record.flags);
            upscale.maxRenderSize = { record.input.width, record.input.height };
            upscale.maxUpscaleSize = { record.output.width, record.output.height };
            upscale.fpMessage = nullptr;
            ffxContext context = nullptr;
            std::unique_lock oneAtATime(fidelityFxCalls());
            const ffxReturnCode_t code = impl.fsr.fidelityFx->create(&context, &upscale.header, nullptr);
            oneAtATime.unlock();
            if (code != kOk)
            {
                return std::unexpected(refusal(record, code));
            }
            return context;
        }

        //==============================================================================================================
        /// Destroys `context`, the FSR context of the upscaler `label`, under the FidelityFX mutex. Logs and aborts, in every
        /// build, when the runtime refuses.
        void destroyContext(d3d12::FsrRuntime& runtime, ffxContext context, std::string_view label)
        {
            ROJORHI_INTERNAL_ASSERT(runtime.fidelityFx.has_value(), "an FSR context outlived the FidelityFX runtime that made it");
            std::unique_lock oneAtATime(fidelityFxCalls());
            const ffxReturnCode_t code = runtime.fidelityFx->destroy(&context, nullptr);
            oneAtATime.unlock();
            if (code != kOk)
            {
                ROJORHI_LOG_ERROR("ffxDestroyContext for upscaler \"{}\" returned {} ({})", label, returnCodeName(code), code);
                std::abort();
            }
        }
    } // namespace

    namespace d3d12 {
        //==============================================================================================================
        std::wstring executableDirectory()
        {
            std::wstring path(MAX_PATH, L'\0');
            for (;;)
            {
                const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
                if (length == 0)
                {
                    return {};
                }
                if (length < path.size())
                {
                    path.resize(length);
                    break;
                }
                path.resize(path.size() * 2);
            }
            const size_t slash = path.find_last_of(L"\\/");
            if (slash == std::wstring::npos)
            {
                return {};
            }
            path.resize(slash);
            return path;
        }

        //==============================================================================================================
        std::optional<FidelityFx> loadFidelityFx(std::wstring_view directory)
        {
            if (directory.empty())
            {
                return std::nullopt;
            }
            std::wstring path(directory);
            path += L'\\';
            path += kLoaderName;
            // An absent loader is a supported state: the executable does not ship FSR. Only a file that is there is loaded, so
            // a loader of the same name elsewhere is never picked up.
            if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
            {
                return std::nullopt;
            }
            // The loader's own imports are searched in its directory and the default directories, never the current
            // directory or the PATH.
            const HMODULE module = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
            if (module == nullptr)
            {
                ROJORHI_LOG_WARN("LoadLibraryExW of {} failed with error {}, so FSR is unavailable", utf8(path), GetLastError());
                return std::nullopt;
            }
            FidelityFx fidelityFx;
            fidelityFx.module = module;
            const bool resolved = resolve(module, "ffxCreateContext", fidelityFx.create) && resolve(module, "ffxDestroyContext", fidelityFx.destroy) &&
                                  resolve(module, "ffxConfigure", fidelityFx.configure) && resolve(module, "ffxQuery", fidelityFx.query) &&
                                  resolve(module, "ffxDispatch", fidelityFx.dispatch);
            if (!resolved)
            {
                FreeLibrary(module);
                return std::nullopt;
            }
            return fidelityFx;
        }

        //==============================================================================================================
        void unloadFidelityFx(FsrRuntime& runtime)
        {
            if (runtime.fidelityFx.has_value())
            {
                FreeLibrary(runtime.fidelityFx->module);
                runtime.fidelityFx.reset();
            }
        }

        //==============================================================================================================
        bool fsrAvailable(const FidelityFx& fidelityFx, ID3D12Device12& device)
        {
            // A zero count with no output arrays asks for the number of versions the device can create.
            uint64_t count = 0;
            ffxQueryDescGetVersions query = {};
            query.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
            query.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
            query.device = static_cast<ID3D12Device*>(&device);
            query.outputCount = &count;
            std::unique_lock oneAtATime(fidelityFxCalls());
            const ffxReturnCode_t code = fidelityFx.query(nullptr, &query.header);
            oneAtATime.unlock();
            if (code != kOk)
            {
                ROJORHI_LOG_WARN("the FidelityFX version query returned {} ({}), so FSR is unavailable", returnCodeName(code), code);
                return false;
            }
            return count > 0;
        }

        //==============================================================================================================
        std::span<const UpscalerKind> listedUpscalers(const std::optional<FidelityFx>& fidelityFx, ID3D12Device12& device)
        {
            static constexpr std::array<UpscalerKind, 1> kFsr = { UpscalerKind::FSR };
            if (fidelityFx.has_value() && fsrAvailable(*fidelityFx, device))
            {
                return kFsr;
            }
            return {};
        }

        //==============================================================================================================
        void retireUpscaler(void* context, core::UpscalerRecord& record)
        {
            if (record.native == nullptr)
            {
                return;
            }
            DeviceImpl& impl = *static_cast<DeviceImpl*>(context);
            destroyContext(impl.fsr, record.native, record.label);
            record.native = nullptr;
        }
    } // namespace d3d12

    //==================================================================================================================
    Result<Upscaler> Device::createUpscaler(const UpscalerDesc& desc)
    {
        DeviceImpl& impl = DeviceImpl::of(*this);
        // The descriptor is checked and the kind resolved first; a kind the device does not list never reaches FidelityFX.
        Result<core::UpscalerRecord> prepared = core::prepareUpscaler(impl.core, desc);
        if (!prepared)
        {
            Error error = std::move(prepared.error());
            const bool asksForFsr = desc.kind == UpscalerKind::Auto || desc.kind == UpscalerKind::FSR;
            if (asksForFsr && error.category == ErrorCategory::Unsupported && !impl.fsr.fidelityFx.has_value())
            {
                error.message += kMissingRuntime;
            }
            return std::unexpected(std::move(error));
        }
        core::UpscalerRecord record = std::move(*prepared);
        // The device lists FSR alone, and only when the runtime loaded and offers a provider.
        ROJORHI_INTERNAL_ASSERT(
            record.kind == UpscalerKind::FSR && impl.fsr.fidelityFx.has_value(), "the D3D12 device listed an upscaler kind it has no runtime for"
        );
        Result<ffxContext> context = createContext(impl, record);
        if (!context)
        {
            return std::unexpected(std::move(context.error()));
        }
        record.native = *context;
        const std::string label = record.label;
        const Upscaler upscaler = impl.core.pool<Upscaler>().allocate(std::move(record));
        if (upscaler.bits == 0)
        {
            destroyContext(impl.fsr, *context, label);
            return std::unexpected(core::tableFull("upscaler", impl.core.pool<Upscaler>().capacity()));
        }
        return upscaler;
    }
} // namespace rojoRHI
