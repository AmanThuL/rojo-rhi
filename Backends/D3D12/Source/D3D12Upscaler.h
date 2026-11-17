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
    // Holds one reference from `LoadLibraryExW`, which `unloadFidelityFx` releases.
    struct FidelityFx
    {
        HMODULE module = nullptr;
        PfnFfxCreateContext create = nullptr;
        PfnFfxDestroyContext destroy = nullptr;
        PfnFfxConfigure configure = nullptr;
        PfnFfxQuery query = nullptr;
        PfnFfxDispatch dispatch = nullptr;
    };

    // Empty when no FidelityFX runtime loaded beside the executable.
    struct FsrRuntime
    {
        std::optional<FidelityFx> fidelityFx;
    };

    // Loads the FidelityFX loader from `directory` and resolves its five entry points; its dependencies are searched
    // there and in the default directories. Returns nothing, and holds no module, when the file is not there, does not
    // load or misses an entry point.
    std::optional<FidelityFx> loadFidelityFx(std::wstring_view directory);

    // Once, after every upscale context has retired.
    void unloadFidelityFx(FsrRuntime& runtime);

    // Under the process-wide FidelityFX mutex, a leaf every query, create and destroy call into the runtime takes: the
    // loader is one per process and documents no thread safety.
    bool fsrAvailable(const FidelityFx& fidelityFx, ID3D12Device12& device);

    // `FSR`, with FidelityFX's documented input scale range, when it loaded and `fsrAvailable` holds, else none. The
    // span views static storage.
    std::span<const UpscalerSupport> listedUpscalers(const std::optional<FidelityFx>& fidelityFx, ID3D12Device12& device);

    // Under the FidelityFX mutex. `context` is the DeviceImpl; a null payload is skipped. Runs under the upscaler
    // pool's lock. Logs and aborts, in every build, when the runtime refuses the destruction.
    void retireUpscaler(void* context, core::UpscalerRecord& record);
} // namespace rojoRHI::d3d12
