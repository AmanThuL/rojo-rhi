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
    struct StageDxil
    {
        std::span<const std::byte> vertex; // Empty for a mesh pipeline.
        std::span<const std::byte> task;
        std::span<const std::byte> mesh; // Empty for a vertex pipeline.
        std::span<const std::byte> fragment;
    };

    // 128-bit FNV-1a: offset basis 0x6c62272e07bb014262b821756295c58d, prime 2^88 + 0x13b; the high 64 bits first.
    std::array<uint64_t, 2> pipelineKey(std::span<const std::byte> canonical);

    // 32 lowercase hexadecimal digits, the high 64 bits first.
    std::wstring keyName(std::array<uint64_t, 2> key);

    // The kind, then each field of `desc` that reaches the pipeline state stream, in declaration order, as fixed-width
    // little-endian values. A present stage is its entry name and DXIL digest. The class is written for a vertex
    // pipeline only: a mesh pipeline's topology follows from its mesh entry, whose digest is in the key. The label is
    // not part of the key.
    std::vector<std::byte> canonicalKey(const GraphicsPipelineDesc& desc, const StageDxil& dxil);

    std::vector<std::byte> canonicalKey(const ComputePipelineDesc& desc, std::span<const std::byte> dxil);

    // The lines `rojorhi.d3d12.pipelinecache`, `version 1`, `rojorhi <major>.<minor>.<patch>` and `library
    // <libraryBytes>`, each ended by one line feed; the library's bytes follow it.
    std::string blobHeader(size_t libraryBytes);

    // The reason is a phrase that completes "the pipeline cache blob ...".
    std::expected<std::span<const std::byte>, std::string_view> libraryOf(std::span<const std::byte> blob);
} // namespace rojoRHI::d3d12
