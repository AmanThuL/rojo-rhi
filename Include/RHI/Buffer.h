//----------------------------------------------------------------------------------------------------------------------
/// @file Buffer.h
/// @brief Declares GPU buffers, their descriptor, byte ranges, and barrier uses.
//----------------------------------------------------------------------------------------------------------------------
#pragma once
#include <cstdint>
#include <string_view>

namespace lmx::rhi {

/// Describes a GPU buffer allocation and its allowed usages.
///
/// Vertex, index, and uniform reads need no usage flag: they are what a buffer is for. The storage
/// flags below are the ones a shader's read-write bindings need, and they exist so that a
/// bindStorageBuffer with an access the buffer was never created for is a caller error with a
/// message rather than undefined shader behavior.
struct BufferDesc {
    uint64_t size = 0;         ///< Allocation size in bytes.
    bool storageRead = false;  ///< Enables shader reads through a storage binding.
    bool storageWrite = false; ///< Enables shader writes through a storage binding.
    bool cpuReadback = false;  ///< Enables blocking CPU readback of the buffer's contents.
    std::string_view label;    ///< Diagnostic object label.
};
/// Provides access to an immutable-size GPU buffer.
class Buffer {
public:
    /// Destroys the buffer after its owning device has finished using it.
    virtual ~Buffer() = default;
    /// Returns the allocation size in bytes.
    virtual uint64_t size() const = 0;
    /// Blocking readback of the buffer's leading bytes (requires cpuReadback). outSize must not
    /// exceed size(), and the caller is responsible for having completed the GPU work that wrote
    /// the range (Device::waitIdle) -- this call performs no synchronization of its own.
    /// Copies the first `outSize` bytes of the buffer into `out` after GPU work has completed.
    virtual void readback(void* out, uint64_t outSize) = 0;
};

/// BufferRange::size: every byte from `offset` to the end of the allocation.
inline constexpr uint64_t kWholeBuffer = ~uint64_t{0};

/// Names the bytes of a buffer that a barrier covers.
///
/// The default covers the whole allocation, which is what a caller with no sub-range detail means;
/// a narrower range is how one pass's slice of a shared buffer is ordered against another's. Ranges
/// are validated against the buffer they are used with: an empty range, or one running past the end
/// of the allocation, is a caller error.
struct BufferRange {
    uint64_t offset = 0;          ///< First byte in the range.
    uint64_t size = kWholeBuffer; ///< Bytes covered, or kWholeBuffer.
};

/// The same, for buffers -- the resource kind fillBuffer, the storage bindings, and the indirect
/// argument reads all hazard on, and which no texture edge can honestly stand in for.
/// Identifies buffer use on either side of an explicit barrier.
enum class BufferUse {
    /// Read by a shader through a non-storage binding: uniforms, vertex pulling, or indices.
    ShaderRead,
    StorageRead,     ///< Read through a storage binding.
    StorageWrite,    ///< Written through a storage binding.
    CopySource,      ///< Read by a copy command.
    CopyDestination, ///< Written by a copy command or by fillBuffer.
    IndirectArgument ///< Read by the GPU as the arguments of an indirect dispatch or draw.
};

} // namespace lmx::rhi
