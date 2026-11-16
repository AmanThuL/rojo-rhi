//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12PipelineKey.h
/// @brief Declares the D3D12 pipeline cache's key and blob header, which include no Windows header.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rojoRHI::d3d12 {
    /// The DXIL container of each stage of a graphics pipeline, empty for an absent stage.
    struct StageDxil
    {
        std::span<const std::byte> vertex;   ///< The vertex entry's DXIL; empty for a mesh pipeline.
        std::span<const std::byte> task;     ///< The task entry's DXIL, or empty.
        std::span<const std::byte> mesh;     ///< The mesh entry's DXIL; empty for a vertex pipeline.
        std::span<const std::byte> fragment; ///< The fragment entry's DXIL, or empty.
    };

    /// Returns the 128-bit FNV-1a hash of `canonical` (offset basis `0x6c62272e07bb014262b821756295c58d`, prime
    /// 2^88 + 0x13b), the high 64 bits first.
    std::array<uint64_t, 2> pipelineKey(std::span<const std::byte> canonical);

    /// Returns the name a pipeline is stored under in the pipeline library: `key` as 32 lowercase hexadecimal digits, the
    /// high 64 bits first.
    std::wstring keyName(std::array<uint64_t, 2> key);

    /// Returns the canonical key bytes of the graphics pipeline `desc`, whose stages' DXIL is `dxil`: the kind (vertex or
    /// mesh), then each field of `desc` that reaches the pipeline state stream, in declaration order, as fixed-width
    /// little-endian values. A present stage is its entry name and its DXIL digest, bytes 4 to 19 of its container; the class
    /// is written for a vertex pipeline only, since a mesh pipeline's topology follows from its mesh entry, whose DXIL
    /// digest is in the key; the label is not part of the key.
    std::vector<std::byte> canonicalKey(const GraphicsPipelineDesc& desc, const StageDxil& dxil);

    /// Returns the canonical key bytes of the compute pipeline `desc`, whose entry's DXIL is `dxil`: the kind, then the
    /// entry's name and DXIL digest.
    std::vector<std::byte> canonicalKey(const ComputePipelineDesc& desc, std::span<const std::byte> dxil);

    /// Returns the header of the blob `Device::pipelineCacheData` hands out around `libraryBytes` bytes of serialized
    /// pipeline library: the lines `rojorhi.d3d12.pipelinecache`, `version 1`, `rojorhi <major>.<minor>.<patch>` and
    /// `library <libraryBytes>`, each ended by one line feed. The library's bytes follow it.
    std::string blobHeader(size_t libraryBytes);

    /// Returns the library bytes of `blob`, a view into it, or the reason it is refused, as a phrase that completes "the
    /// pipeline cache blob ...": it was not written by the D3D12 backend, in this blob version, by this RojoRHI version, or
    /// its library line does not count the bytes that follow it.
    std::expected<std::span<const std::byte>, std::string_view> libraryOf(std::span<const std::byte> blob);
} // namespace rojoRHI::d3d12
