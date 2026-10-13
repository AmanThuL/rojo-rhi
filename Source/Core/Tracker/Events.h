//----------------------------------------------------------------------------------------------------------------------
/// @file Events.h
/// @brief Declares the events a command list logs for the validation tracker: the uses its commands make and its barriers.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/Access.h>
#include <rojoRHI/BufferBarrier.h>
#include <rojoRHI/GlobalBarrier.h>
#include <rojoRHI/Handles.h>
#include <rojoRHI/Layout.h>
#include <rojoRHI/Stage.h>
#include <rojoRHI/SubresourceRange.h>
#include <rojoRHI/TextureBarrier.h>

#include <cstdint>
#include <initializer_list>
#include <variant>

namespace rojoRHI::core {
    /// A set of layouts: bit `1 << Layout` for each member.
    using LayoutSet = uint16_t;

    /// Returns the set that holds exactly `layouts`.
    constexpr LayoutSet layoutSet(std::initializer_list<Layout> layouts)
    {
        LayoutSet set = 0;
        for (const Layout layout : layouts)
        {
            set = static_cast<LayoutSet>(set | (1u << static_cast<uint32_t>(layout)));
        }
        return set;
    }

    /// Returns true when `set` holds `layout`.
    constexpr bool holds(LayoutSet set, Layout layout)
    {
        return (set & (1u << static_cast<uint32_t>(layout))) != 0;
    }

    /// The valid-usage rule of the entry that made a use, which the use reports ahead of the general rules. A texture use
    /// reports it when its subresources are in no layout of its set, or VU-rojo-sync-0004 for a use no entry names a rule for.
    /// An indirect argument or count read reports it when no barrier since the last write of its bytes on the queue has a
    /// destination stage and access that include `IndirectArgs`; each read names its arm, the records or the count.
    enum class UseRule : uint8_t
    {
        None,                ///< A buffer use other than an indirect read: buffers have no layout.
        Sync0004,            ///< A declared shader access (VU-rojo-sync-0004).
        Cmd0007,             ///< A color attachment or a resolve target at `beginRenderPass` (VU-rojo-cmd-0007).
        Cmd0010,             ///< A depth-stencil attachment at `beginRenderPass` (VU-rojo-cmd-0010).
        Cmd0054,             ///< Either texture of `copyTexture` (VU-rojo-cmd-0054).
        Cmd0067,             ///< The destination of `copyBufferToTexture` (VU-rojo-cmd-0067).
        Cmd0069,             ///< The source of `copyTextureToBuffer` (VU-rojo-cmd-0069).
        Cmd0070,             ///< The range of `clearTexture` (VU-rojo-cmd-0070).
        Upscale0016,         ///< An input or the output of `upscale` (VU-rojo-upscale-0016).
        Indirect0005Records, ///< The records `drawIndirect` reads (VU-rojo-indirect-0005).
        Indirect0005Count,   ///< The count `drawIndirect` reads (VU-rojo-indirect-0005).
        Indirect0011Records, ///< The records `drawIndexedIndirect` reads (VU-rojo-indirect-0011).
        Indirect0011Count,   ///< The count `drawIndexedIndirect` reads (VU-rojo-indirect-0011).
        Indirect0017Records, ///< The records `drawMeshTasksIndirect` reads (VU-rojo-indirect-0017).
        Indirect0017Count,   ///< The count `drawMeshTasksIndirect` reads (VU-rojo-indirect-0017).
        Indirect0020Records  ///< The record `dispatchIndirect` reads (VU-rojo-indirect-0020).
    };

    /// One use a command makes of a buffer range or of texture subresources: the rows of chapter 09's table "Accesses commands
    /// make", chapter 12's upscaler uses and the declared accesses of chapter 14. Exactly one of `buffer` and `texture` is not
    /// null.
    struct UseEvent
    {
        Buffer buffer = {};           ///< The buffer used; null for a texture use.
        uint64_t offset = 0;          ///< The first byte of the buffer range.
        uint64_t size = kWholeSize;   ///< Bytes of the buffer range; `kWholeSize` runs to the end of the buffer.
        Texture texture = {};         ///< The texture used; null for a buffer use.
        SubresourceRange range;       ///< The subresources of the texture used.
        Stage stage = Stage::None;    ///< The stage that makes the use.
        Access access = Access::None; ///< The accesses the use makes.
        LayoutSet layouts = 0;        ///< The layouts that permit the use; empty for a buffer.
        UseRule rule = UseRule::None; ///< The rule of the entry that made the use, reported ahead of the general rules.
    };

    /// One event of a list's log: a use, or one record of a barrier batch as the list was given it.
    using TrackerEvent = std::variant<UseEvent, GlobalBarrier, BufferBarrier, TextureBarrier>;

    /// Returns the use of `range` of `texture` at `stage` with `access`, permitted by `layouts` and reported as `rule`.
    constexpr UseEvent textureUse(Texture texture, const SubresourceRange& range, Stage stage, Access access, LayoutSet layouts, UseRule rule)
    {
        return UseEvent{ .texture = texture, .range = range, .stage = stage, .access = access, .layouts = layouts, .rule = rule };
    }

    /// Returns the use of `size` bytes from `offset` of `buffer` at `stage` with `access`, reported as `rule`.
    constexpr UseEvent bufferUse(Buffer buffer, uint64_t offset, uint64_t size, Stage stage, Access access, UseRule rule = UseRule::None)
    {
        return UseEvent{ .buffer = buffer, .offset = offset, .size = size, .stage = stage, .access = access, .rule = rule };
    }
} // namespace rojoRHI::core
