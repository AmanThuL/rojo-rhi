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

    /// A device's pipeline cache: an `ID3D12PipelineLibrary1` made at device creation from the library bytes of the caller's
    /// blob, which the device copies and keeps for its life because the library reads them in place, or an empty library. A
    /// creation looks its stream up by name, a hit when the library returns a pipeline state, and stores what it had to
    /// create. A device whose runtime refuses even an empty library has none, and every creation is a miss. The cache's mutex
    /// is a leaf: it is held only around the library's calls, never while a pool lock is held, and never while recording.
    class PipelineCache
    {
      public:
        PipelineCache() = default;
        PipelineCache(const PipelineCache&) = delete;
        PipelineCache& operator=(const PipelineCache&) = delete;

        /// Makes the library of `impl`'s device from `blob`. An empty blob makes an empty library and reports nothing. A blob
        /// `libraryOf` refuses, or library bytes `CreatePipelineLibrary` refuses (another adapter or driver, or bytes that are
        /// no library), is one warning through the message callback and leaves an empty library. A runtime that refuses an
        /// empty library leaves no library; that is one warning too, which also names a discarded blob.
        void open(DeviceImpl& impl, std::span<const std::byte> blob);

        /// Returns the pipeline state `stream` describes, with one reference the caller owns, stored under `name`, and sets
        /// `hit` when the library served it. A miss creates it with `CreatePipelineState` and stores it under `name`; a name
        /// already stored by another creation stays as stored, and any other refused store is one warning and an uncached
        /// pipeline. A failed look-up or creation is `d3d12::failure` naming the pipeline `label`. Free-threaded; the creation
        /// itself runs outside the cache's lock, so creations compile side by side.
        Result<ID3D12PipelineState*> create(DeviceImpl& impl, const PipelineStream& stream, const std::wstring& name, std::string_view label, bool& hit);

        /// Returns the blob `Device::pipelineCacheData` hands out: `blobHeader` and the serialized library, under the cache's
        /// lock, or the header with `library 0` without a library. A failed serialization is `d3d12::failure`.
        Result<std::vector<std::byte>> data(DeviceImpl& impl);

        /// Releases the library and then the bytes it reads. Called once every pipeline of the device has retired.
        void release();

      private:
        std::vector<std::byte> m_bytes;        ///< The library bytes of the creation blob, which `m_library` reads in place.
        Com<ID3D12PipelineLibrary1> m_library; ///< The library, or null when the runtime refused one.
        std::mutex m_mutex;                    ///< Guards the library's calls; a leaf.
    };
} // namespace rojoRHI::d3d12
