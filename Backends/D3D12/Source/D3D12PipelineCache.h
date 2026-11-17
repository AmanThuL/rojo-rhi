//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12PipelineCache.h
/// @brief Declares the D3D12 pipeline cache: a pipeline library over the bytes of the caller's blob, and its lock.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "D3D12Common.h"

#include <cstddef>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rojoRHI {
    struct DeviceImpl;
}

namespace rojoRHI::d3d12 {
    class PipelineStream;

    // An `ID3D12PipelineLibrary1` made at device creation from the library bytes of the caller's blob, which the device
    // copies and keeps for its life because the library reads them in place, or an empty library. A device whose
    // runtime refuses even an empty library has none, and every creation is a miss. The mutex is a leaf: held only
    // around the library's calls, never while a pool lock is held, and never while recording.
    class PipelineCache
    {
      public:
        PipelineCache() = default;
        PipelineCache(const PipelineCache&) = delete;
        PipelineCache& operator=(const PipelineCache&) = delete;

        // An empty blob makes an empty library and reports nothing. A blob `libraryOf` refuses, or library bytes
        // `CreatePipelineLibrary` refuses (another adapter or driver, or bytes that are no library), is one warning and
        // leaves an empty library. A runtime that refuses an empty library leaves none; that is one warning too.
        void open(DeviceImpl& impl, std::span<const std::byte> blob);

        // The caller owns the returned reference; `hit` is set when the library served it. A name another creation
        // stored first stays as stored; any other refused store is one warning and an uncached pipeline. Free-threaded:
        // the creation runs outside the cache's lock, so creations compile side by side.
        Result<ID3D12PipelineState*> create(DeviceImpl& impl, const PipelineStream& stream, const std::wstring& name, std::string_view label, bool& hit);

        // Serialized under the cache's lock; without a library, the header alone with `library 0`.
        Result<std::vector<std::byte>> data(DeviceImpl& impl);

        // Only once every pipeline of the device has retired.
        void release();

      private:
        std::vector<std::byte> m_bytes; // `m_library` reads these bytes in place.
        Com<ID3D12PipelineLibrary1> m_library;
        std::mutex m_mutex;
    };
} // namespace rojoRHI::d3d12
