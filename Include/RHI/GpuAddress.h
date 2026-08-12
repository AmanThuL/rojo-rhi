//----------------------------------------------------------------------------------------------------------------------
/// @file GpuAddress.h
/// @brief Declares the backend-neutral address a shader reads GPU-visible memory through.
//----------------------------------------------------------------------------------------------------------------------
#pragma once
#include <cstdint>
#include <type_traits>

namespace lmx::rhi {

/// The address a shader reads a block of GPU-visible memory through, carried as a plain value.
///
/// It holds no backend handle and owns nothing: it is the number the hardware dereferences, which
/// is what lets one block's address be written into another block a shader follows. Zero is the
/// null address and is what a default-constructed value holds.
///
/// An address says nothing about how long the memory behind it stays valid -- that belongs to
/// whatever produced it. CommandList::bindFrameData, the only operation returning one today,
/// states its own lifetime rule on its declaration: the address names memory the open frame owns,
/// and caching it past that frame reads storage a later frame has overwritten.
///
/// Arithmetic is deliberately absent. Offsetting an address asserts a size and layout this value
/// does not carry, so the operation waits for a caller that can justify it rather than existing in
/// advance.
struct GpuAddress {
    uint64_t value = 0; ///< The backend's GPU virtual address; zero is the null address.

    /// Returns true when this names an allocation rather than the null address.
    constexpr bool isValid() const { return value != 0; }

    /// Compares two addresses by the location they name.
    constexpr bool operator==(const GpuAddress&) const = default;
};

static_assert(std::is_standard_layout_v<GpuAddress>,
              "GpuAddress must be standard-layout: it crosses the CPU/GPU boundary inside blocks "
              "a shader reads");
static_assert(std::is_trivially_copyable_v<GpuAddress>,
              "GpuAddress must be trivially copyable: bindFrameData copies blocks containing it "
              "with memcpy");

} // namespace lmx::rhi
