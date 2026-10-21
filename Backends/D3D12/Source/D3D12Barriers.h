//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Barriers.h
/// @brief Declares the lowering of barrier batches to D3D12 enhanced barriers and the scratch a list lowers them into.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "D3D12Common.h"

#include <cstdint>
#include <vector>

namespace rojoRHI::d3d12 {
    struct TexturePayload;

    /// The arrays a command list lowers one barrier batch into before its native `Barrier` call; the list's own, so
    /// recording takes no lock, and grown, never shrunk. They hold the last batch until the next one clears them.
    struct BarrierScratch
    {
        std::vector<D3D12_GLOBAL_BARRIER> globals;   ///< The batch's global barriers.
        std::vector<D3D12_BUFFER_BARRIER> buffers;   ///< The batch's buffer barriers.
        std::vector<D3D12_TEXTURE_BARRIER> textures; ///< The batch's texture barriers.
    };

    /// What one side of a barrier names, which decides how its accesses lower.
    enum class AccessTarget : uint8_t
    {
        Global,         ///< A global barrier.
        Buffer,         ///< A buffer barrier of a buffer without a raw unordered-access view.
        WritableBuffer, ///< A buffer barrier of a `ShaderWrite` buffer, which the fill kernel writes through that view.
        Texture         ///< A texture barrier, whose side's layout decides how shader reads lower.
    };

    /// Returns the sync scope `stages` lowers to on a list of `type`: the union of chapter 09's row for each bit, `SYNC_DRAW |
    /// SYNC_EXECUTE_INDIRECT | SYNC_RESOLVE` for every bit of `AllGraphics`, `SYNC_ALL` for `All` and `SYNC_NONE` for none.
    /// `Copy` lowers as `copyStageSync(type)` does.
    D3D12_BARRIER_SYNC syncOf(Stage stages, QueueType type);

    /// Returns the sync scope of the `Copy` stage on a list of `type`: copies, the fill kernel and clears on a Graphics list,
    /// copies, the fill kernel and unordered-access clears on a Compute list, and copies alone on a Copy list.
    D3D12_BARRIER_SYNC copyStageSync(QueueType type);

    /// Returns the access bits `access` lowers to on a side that names `target`. `layout` is the side's layout for a texture,
    /// ignored otherwise: shader reads in `ShaderWrite` lower to unordered access, and a side in `Undefined` names no access.
    /// A `CopyWrite` on a global barrier or a writable buffer adds unordered access for the fill kernel. No access is
    /// `ACCESS_NO_ACCESS`.
    D3D12_BARRIER_ACCESS accessOf(Access access, AccessTarget target, Layout layout);

    /// Returns `sync` without the bits that are compatible with none of the bits of `access`, the side's access: the debug
    /// layer rejects a side whose sync names a scope its access never runs in. A side with no access drops only the
    /// unordered-access clear's scope, `SYNC_NONE` and `SYNC_ALL` stay, and a sync narrowing would leave empty widens to
    /// `SYNC_ALL`.
    D3D12_BARRIER_SYNC narrowSync(D3D12_BARRIER_SYNC sync, D3D12_BARRIER_ACCESS access);

    /// Returns the barrier layout `layout` lowers to. `ShaderRead` and `DepthStencilRead` lower to the generic
    /// `LAYOUT_SHADER_RESOURCE`, which direct and compute queues share.
    D3D12_BARRIER_LAYOUT layoutOf(Layout layout);

    /// Returns the subresources of the texture `payload` describes that `range` covers: its mips and layers with `kAllMips`
    /// and `kAllLayers` resolved against the texture, one array slice for a 3D texture, and both planes of a format with a
    /// stencil plane.
    D3D12_BARRIER_SUBRESOURCE_RANGE rangeOf(const TexturePayload& payload, const SubresourceRange& range);
} // namespace rojoRHI::d3d12
