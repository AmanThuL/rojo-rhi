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
    Extent3D mipExtent(const Extent3D& extent, uint32_t mip);

    // The one site of the description rules that creation and the placement query share.
    void checkTextureDesc(const DeviceCore& core, const TextureDesc& desc);

    // The description rules, and the texture is not Sparse.
    void checkTextureRequirementsQuery(const DeviceCore& core, const TextureDesc& desc);

    // A placed texture's heap and placement are checked against `requirements`, which the backend computes only then. A
    // backend calls it first in `createTexture`; the record has no native payload yet.
    TextureRecord prepareTexture(
        DeviceCore& core, const TextureDesc& desc, std::span<const TextureData> initial, const Placement& placement, const PlacementRequirements& requirements
    );

    // A full native table is `OutOfMemory`, counting and tracking nothing; the caller still owns `record.native` and
    // releases it.
    Result<Texture> addTexture(DeviceCore& core, TextureRecord record);

    // Deferred as `DeviceCore::destroy` is.
    void destroyTexture(DeviceCore& core, Texture texture);

    // Every entry that takes a texture other than as a color attachment, a resolve target or in a barrier asks it.
    void checkNotSwapchainImage(const TextureRecord& texture, std::string_view use);

    // The one site of the subresource range rules, shared by views and barriers.
    void checkSubresourceRange(const TextureRecord& texture, const SubresourceRange& range);

    // The record's address is stable while the texture lives. A backend calls it first in `createView`, then `addView`.
    const TextureRecord& checkView(DeviceCore& core, Texture texture, const TextureViewDesc& desc);

    // A full heap is `OutOfMemory`, counting nothing; the caller still owns `native` and releases it. Otherwise the slot's
    // payload goes to the `view` retire hook when the slot retires or the device is destroyed.
    Result<ViewIndex> addView(DeviceCore& core, Texture texture, void* native);

    // Asks the rules of `checkView` as one question. A backend calls it first in `writeView`, then `placeView`.
    const TextureRecord& checkViewWrite(DeviceCore& core, IndexRange range, uint32_t index, Texture texture, const TextureViewDesc& desc);

    // The replaced payload goes to the `view` retire hook once the open frame, or the next, retires. Returns false, placing
    // nothing, when a racing release took the range after the check; the caller then still owns `native` and releases it.
    bool placeView(DeviceCore& core, IndexRange range, uint32_t index, Texture texture, void* native);

    // Deferred to the retirement of the open frame, or of the next one.
    void destroyView(DeviceCore& core, ViewIndex view);
} // namespace rojoRHI::core
