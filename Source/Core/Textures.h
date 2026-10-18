//----------------------------------------------------------------------------------------------------------------------
/// @file Textures.h
/// @brief Declares the checks of the texture and view rules of chapter 04, shared by every backend.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "Core/DeviceCore.h"
#include "Core/Records.h"

#include <span>
#include <string_view>

namespace rojoRHI::core {
    /// Returns the extent of mip `mip` of a texture whose mip 0 is `extent`, in texels and never below one.
    Extent3D mipExtent(const Extent3D& extent, uint32_t mip);

    /// Checks a texture description (VU-rojo-texture-0001 to 0012, VU-rojo-format-0003, VU-rojo-format-0004) against the
    /// device's limits, capabilities and format table; the one site of those rules, shared by creation and by the placement
    /// query.
    void checkTextureDesc(const DeviceCore& core, const TextureDesc& desc);

    /// Checks the arguments of `Device::placementRequirements` for a texture (VU-rojo-heap-0002): the descriptor obeys the
    /// texture rules and is not Sparse.
    void checkTextureRequirementsQuery(const DeviceCore& core, const TextureDesc& desc);

    /// Checks a texture description and its initial data (VU-rojo-texture-0001 to 0015, 0018 to 0021, VU-rojo-core-0006,
    /// VU-rojo-core-0007) against the device, and, when `placement.heap` is not null, the placement (VU-rojo-core-0001 to
    /// 0003 for the heap, VU-rojo-heap-0003 to 0006) against `requirements`, which the backend computes only for a placed
    /// texture. It returns the record of the texture with no native payload. A backend calls it first in `createTexture`.
    TextureRecord prepareTexture(
        DeviceCore& core, const TextureDesc& desc, std::span<const TextureData> initial, const Placement& placement, const PlacementRequirements& requirements
    );

    /// Stores `record`, which `prepareTexture` returned, in the texture pool, counts it against its heap and returns its
    /// handle.
    Texture addTexture(DeviceCore& core, TextureRecord record);

    /// Destroys `texture`, deferred as `DeviceCore::destroy` does, after checking that it is not a swapchain image
    /// (VU-rojo-present-0013) and that no view slot still holds a view of it (VU-rojo-texture-0028), and releases its claim on
    /// its heap.
    void destroyTexture(DeviceCore& core, Texture texture);

    /// Issues VU-rojo-present-0012 when `texture` is a swapchain image; `use` names the entry that would use it. Every entry
    /// that takes a texture other than as a color attachment, a resolve target or in a barrier asks it.
    void checkNotSwapchainImage(const TextureRecord& texture, std::string_view use);

    /// Checks that `range` names at least one mip and layer of `texture` and lies inside it (VU-rojo-texture-0016, 0017); the
    /// one site of the rules, shared by views and barriers.
    void checkSubresourceRange(const TextureRecord& texture, const SubresourceRange& range);

    /// Creates a view of `texture`: checks the descriptor against the texture (VU-rojo-core-0001 to 0003 for the handle,
    /// VU-rojo-texture-0016, 0017, 0022 to 0027, VU-rojo-format-0003), takes a slot of the view heap and counts the view
    /// against the texture. A full heap is an error result. The Null backend has no native view, so this is the whole entry.
    Result<ViewIndex> createView(DeviceCore& core, Texture texture, const TextureViewDesc& desc);

    /// Writes a view of `texture` into slot `range.base + index` of a reserved view range (VU-rojo-texture-0033; VU-rojo-core-0001
    /// to 0003 for the handle; VU-rojo-texture-0034 for the descriptor, which is the rules of `createView` asked as one
    /// question; VU-rojo-texture-0031 for a range that is not a reserved, unreleased one as returned), replacing the view
    /// there, and moves the per-texture view counts.
    void writeView(DeviceCore& core, IndexRange range, uint32_t index, Texture texture, const TextureViewDesc& desc);

    /// Releases the view slot `view`, deferred to the retirement of the frame that is open, or of the next one; it was
    /// returned by `createView` (VU-rojo-device-0010). The texture it described no longer counts the view.
    void destroyView(DeviceCore& core, ViewIndex view);
} // namespace rojoRHI::core
