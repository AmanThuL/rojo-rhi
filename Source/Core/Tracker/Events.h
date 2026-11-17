//----------------------------------------------------------------------------------------------------------------------
/// @file Events.h
/// @brief Declares the events a command list logs for the validation tracker: the uses its commands make and its barriers.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "Core/Tracker/Tiles.h"

#include <cstdint>
#include <initializer_list>
#include <optional>
#include <variant>

namespace rojoRHI::core {
    using LayoutSet = uint16_t;

    constexpr LayoutSet layoutSet(std::initializer_list<Layout> layouts)
    {
        LayoutSet set = 0;
        for (const Layout layout : layouts)
        {
            set = static_cast<LayoutSet>(set | (1u << static_cast<uint32_t>(layout)));
        }
        return set;
    }

    constexpr bool holds(LayoutSet set, Layout layout)
    {
        return (set & (1u << static_cast<uint32_t>(layout))) != 0;
    }

    // The entry's rule, reported ahead of the general ones. A texture use reports it when its subresources are in no layout
    // of its set; an indirect read reports it when no barrier since the last write of its bytes on the queue has a
    // destination that includes `IndirectArgs`.
    enum class UseRule : uint8_t
    {
        None,                // A buffer use other than an indirect read: buffers have no layout.
        Sync0004,            // A declared shader access (VU-rojo-sync-0004).
        Cmd0007,             // A color attachment or a resolve target at `beginRenderPass` (VU-rojo-cmd-0007).
        Cmd0010,             // A depth-stencil attachment at `beginRenderPass` (VU-rojo-cmd-0010).
        Cmd0054,             // Either texture of `copyTexture` (VU-rojo-cmd-0054).
        Cmd0067,             // The destination of `copyBufferToTexture` (VU-rojo-cmd-0067).
        Cmd0069,             // The source of `copyTextureToBuffer` (VU-rojo-cmd-0069).
        Cmd0072,             // The range of `clearTexture` (VU-rojo-cmd-0072).
        Upscale0016,         // An input or the output of `upscale` (VU-rojo-upscale-0016).
        Indirect0005Records, // The records `drawIndirect` reads (VU-rojo-indirect-0005).
        Indirect0005Count,   // The count `drawIndirect` reads (VU-rojo-indirect-0005).
        Indirect0011Records, // The records `drawIndexedIndirect` reads (VU-rojo-indirect-0011).
        Indirect0011Count,   // The count `drawIndexedIndirect` reads (VU-rojo-indirect-0011).
        Indirect0017Records, // The records `drawMeshTasksIndirect` reads (VU-rojo-indirect-0017).
        Indirect0017Count,   // The count `drawMeshTasksIndirect` reads (VU-rojo-indirect-0017).
        Indirect0020Records  // The record `dispatchIndirect` reads (VU-rojo-indirect-0020).
    };

    // A row of chapter 09's "Accesses commands make", an upscaler use of chapter 12 or a declared access of chapter 14.
    // Exactly one of `buffer` and `texture` is not null.
    struct UseEvent
    {
        Buffer buffer = {};
        uint64_t offset = 0;
        uint64_t size = kWholeSize;
        Texture texture = {};
        SubresourceRange range;
        Stage stage = Stage::None;
        Access access = Access::None;
        LayoutSet layouts = 0;
        UseRule rule = UseRule::None;
        // A copy's texels in each subresource of `range`, which the sparse mapping rule judges per tile; empty for whole
        // subresources.
        std::optional<TexelBox> box;
    };

    using TrackerEvent = std::variant<UseEvent, GlobalBarrier, BufferBarrier, TextureBarrier>;

    constexpr UseEvent textureUse(Texture texture, const SubresourceRange& range, Stage stage, Access access, LayoutSet layouts, UseRule rule)
    {
        return UseEvent{ .texture = texture, .range = range, .stage = stage, .access = access, .layouts = layouts, .rule = rule };
    }

    constexpr UseEvent regionUse(Texture texture, const TextureRegion& region, Access access, LayoutSet layouts, UseRule rule)
    {
        const SubresourceRange range{ .baseMip = region.mip, .mipCount = 1, .baseLayer = region.layer, .layerCount = region.layers };
        UseEvent use = textureUse(texture, range, Stage::Copy, access, layouts, rule);
        use.box = TexelBox{ .offset = region.offset, .extent = region.extent };
        return use;
    }

    constexpr UseEvent bufferUse(Buffer buffer, uint64_t offset, uint64_t size, Stage stage, Access access, UseRule rule = UseRule::None)
    {
        return UseEvent{ .buffer = buffer, .offset = offset, .size = size, .stage = stage, .access = access, .rule = rule };
    }
} // namespace rojoRHI::core
