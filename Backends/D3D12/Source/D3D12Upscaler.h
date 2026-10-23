//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Upscaler.h
/// @brief Declares how the D3D12 backend loads the FidelityFX runtime, lists FSR and retires an FSR upscale context.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "Core/Records.h"
#include "D3D12Common.h"

#include <api/include/ffx_api.h>

#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace rojoRHI::d3d12 {
    /// The FidelityFX loader module, `amd_fidelityfx_loader_dx12.dll`, and the API entry points it exports. The module holds
    /// one reference from `LoadLibraryW`, which `unloadFidelityFx` releases.
    struct FidelityFx
    {
        HMODULE module = nullptr;               ///< The loader module.
        PfnFfxCreateContext create = nullptr;   ///< `ffxCreateContext`.
        PfnFfxDestroyContext destroy = nullptr; ///< `ffxDestroyContext`.
        PfnFfxConfigure configure = nullptr;    ///< `ffxConfigure`.
        PfnFfxQuery query = nullptr;            ///< `ffxQuery`.
        PfnFfxDispatch dispatch = nullptr;      ///< `ffxDispatch`.
    };

    /// What a device keeps of the FidelityFX runtime: the loader it opened at creation, empty when none loaded beside the
    /// executable.
    struct FsrRuntime
    {
        std::optional<FidelityFx> fidelityFx; ///< The loader and its entry points; empty when FSR is unavailable.
    };

    /// Returns the directory of the running executable, without a trailing separator; empty when its path cannot be read.
    std::wstring executableDirectory();

    /// Loads `amd_fidelityfx_loader_dx12.dll` from `directory`, searching its dependencies there and in the default
    /// directories, and resolves `ffxCreateContext`, `ffxDestroyContext`, `ffxConfigure`, `ffxQuery` and `ffxDispatch`.
    /// Returns nothing, and holds no module, when the file is not there, does not load or misses an entry point.
    std::optional<FidelityFx> loadFidelityFx(std::wstring_view directory);

    /// Frees the loader module of `runtime`, if one loaded, and empties it. The device calls it once, after every upscale
    /// context has retired.
    void unloadFidelityFx(FsrRuntime& runtime);

    /// Returns true when the runtime's version query, given `device`, offers at least one upscaler provider. The query runs
    /// under the process-wide FidelityFX mutex, a leaf that every query, create and destroy call into the runtime takes, since
    /// the loader is one per process and documents no thread safety.
    bool fsrAvailable(const FidelityFx& fidelityFx, ID3D12Device12& device);

    /// Returns the upscalers a device lists: `FSR`, with FidelityFX's documented input scale range, when `fidelityFx` loaded
    /// and `fsrAvailable` holds for `device`, else none. The span views static storage.
    std::span<const UpscalerSupport> listedUpscalers(const std::optional<FidelityFx>& fidelityFx, ID3D12Device12& device);

    /// Destroys the FSR context of a retiring upscaler record with `ffxDestroyContext`, under the FidelityFX mutex. `context`
    /// is the `DeviceImpl`; a null payload is skipped. It runs under the upscaler pool's lock. Logs and aborts, in every
    /// build, when the runtime refuses the destruction.
    void retireUpscaler(void* context, core::UpscalerRecord& record);
} // namespace rojoRHI::d3d12
