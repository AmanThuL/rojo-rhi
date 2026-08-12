//----------------------------------------------------------------------------------------------------------------------
/// @file Heap.h
/// @brief Declares placement heaps and the size and alignment a placed resource costs.
//----------------------------------------------------------------------------------------------------------------------
#pragma once
#include <cstdint>
#include <string_view>

namespace lmx::rhi {

/// What one resource costs inside a heap: the bytes it occupies and the alignment its placement
/// offset must satisfy. Both are the backend's answer for that exact descriptor, so a caller that
/// packs resources into a heap asks rather than assuming a texel-count-times-size arithmetic that
/// no driver promises.
struct SizeAlign {
    uint64_t size = 0;      ///< Bytes the resource occupies in a heap.
    uint64_t alignment = 0; ///< Alignment, in bytes, its heap offset must be a multiple of.
};

/// Describes a placement heap: one device allocation resources are created inside at explicit
/// offsets.
struct HeapDesc {
    uint64_t size = 0;      ///< Allocation size in bytes.
    std::string_view label; ///< Diagnostic object label.
};

/// Owns one block of device-private memory that placed resources are created inside at
/// caller-chosen offsets.
///
/// Placement is explicit and the heap tracks no hazards of its own: two resources whose byte ranges
/// overlap share those bytes, and nothing in the backend orders one against the other. Every
/// ordering between a placed resource and whatever last used the memory under it is the caller's,
/// stated through the barriers on CommandList -- which is exactly what makes a heap the substrate a
/// render graph aliases transients in, and what makes an unstated overlap silent corruption rather
/// than a stall.
///
/// A Heap must outlive every resource placed in it, and must not outlive the Device that created
/// it. Destroying it while the GPU still reads a resource placed in it is a caller error: the
/// three-frames-in-flight pacing is what a caller proves retirement with.
class Heap {
public:
    /// Destroys the heap after its owning device has finished using it.
    virtual ~Heap() = default;
    /// Returns the heap's size in bytes.
    virtual uint64_t size() const = 0;
};

} // namespace lmx::rhi
