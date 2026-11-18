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

    enum class SingleBarrier : uint8_t
    {
        None, // The last batch held several records, or the list has recorded none.
        Global,
        Buffer,
        Texture,
    };

    // The list's own, so recording takes no lock; grown, never shrunk. They hold the last batch until the next clears
    // them and, when it was one record, that record: a batch that repeats it sends the arrays again unlowered, in a later
    // recording of the list too. A lowering reads only the list's queue type, which its pool fixes, and payloads, which
    // are fixed while their resources live, and a handle's bits name one resource for the pool's whole life.
    struct BarrierScratch
    {
        std::vector<D3D12_GLOBAL_BARRIER> globals;
        std::vector<D3D12_BUFFER_BARRIER> buffers;
        std::vector<D3D12_TEXTURE_BARRIER> textures;
        SingleBarrier single = SingleBarrier::None; // Kept across recordings.
        GlobalBarrier lastGlobal;
        BufferBarrier lastBuffer;
        TextureBarrier lastTexture;
    };

    enum class AccessTarget : uint8_t
    {
        Global,
        Buffer,         // A buffer with no raw UAV.
        WritableBuffer, // A `ShaderWrite` buffer, which the fill kernel writes through its raw UAV.
        Texture         // The side's layout decides how shader reads lower.
    };

    // The union of chapter 09's row for each bit, SYNC_DRAW | SYNC_EXECUTE_INDIRECT | SYNC_RESOLVE for every bit of
    // `AllGraphics`, SYNC_ALL for `All` and SYNC_NONE for none. `Copy` lowers as `copyStageSync(type)` does.
    D3D12_BARRIER_SYNC syncOf(Stage stages, QueueType type);

    // Copies, the fill kernel and clears on a Graphics list; copies, the fill kernel and unordered-access clears on a
    // Compute list; copies alone on a Copy list.
    D3D12_BARRIER_SYNC copyStageSync(QueueType type);

    // `layout` matters for a texture only: shader reads in `ShaderWrite` lower to unordered access, and a side in
    // `Undefined` names no access. A `CopyWrite` on a global barrier or a writable buffer adds unordered access for the
    // fill kernel.
    D3D12_BARRIER_ACCESS accessOf(Access access, AccessTarget target, Layout layout);

    // Drops the bits of `sync` compatible with no bit of `access`: the debug layer rejects a side whose sync names a
    // scope its access never runs in. A side with no access drops only the unordered-access clear's scope; SYNC_NONE
    // and SYNC_ALL stay, and a sync narrowed to nothing widens to SYNC_ALL.
    D3D12_BARRIER_SYNC narrowSync(D3D12_BARRIER_SYNC sync, D3D12_BARRIER_ACCESS access);

    // `ShaderRead` and `DepthStencilRead` lower to the generic LAYOUT_SHADER_RESOURCE, which direct and compute queues
    // share.
    D3D12_BARRIER_LAYOUT layoutOf(Layout layout);

    D3D12_BARRIER_SUBRESOURCE_RANGE rangeOf(const TexturePayload& payload, const SubresourceRange& range);
} // namespace rojoRHI::d3d12
