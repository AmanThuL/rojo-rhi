//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Icb.h
/// @brief Declares the indirect command buffers a Metal 4 command list keeps for its counted draws, and the list's own residency set.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include "Core/CommandLists.h"
#include "Core/DeviceCore.h"

#include <Metal/Metal.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace rojoRHI::metal4 {
    /// The kinds of indirect command buffer a counted draw encodes into, one command type each.
    enum class IcbKind : uint8_t
    {
        Draw,    ///< `MTL::IndirectCommandTypeDraw`, for `drawIndirect`.
        Indexed, ///< `MTL::IndirectCommandTypeDrawIndexed`, for `drawIndexedIndirect`.
        Mesh,    ///< `MTL::IndirectCommandTypeDrawMeshThreadgroups`, for `drawMeshTasksIndirect`.
    };

    /// How many kinds of indirect command buffer there are.
    inline constexpr size_t kIcbKinds = 3;

    /// The fewest commands an indirect command buffer is made with, so the small counted draws of a list share one size.
    inline constexpr uint32_t kIcbMinimumCommands = 64;

    /// One slot of a list's indirect command buffers: its buffer, the resource ID a kernel reaches it by, and the commands it
    /// holds, kept so a counted draw sends the buffer no message.
    struct IcbSlot
    {
        MTL::IndirectCommandBuffer* buffer = nullptr; ///< The buffer, one retained reference.
        MTL::ResourceID id = {};                      ///< Its `gpuResourceID()`.
        uint32_t capacity = 0;                        ///< Its commands.
    };

    /// The indirect command buffers of one command list and the residency set that keeps them resident. Per kind, slot `i`
    /// serves the `i`-th counted draw of that kind in a recording; the slots outlive the recording, so a list that records
    /// the same counted draws again makes no native object. A slot whose capacity is below a draw's `maxCount` is replaced by
    /// one of `bit_ceil(max(maxCount, kIcbMinimumCommands))` commands: the list is acquired again only once its frame slot's
    /// work has retired, so the replaced buffer is no longer in use. The buffers join the list's residency set, which the list's
    /// command buffer uses from the recording's first counted draw and which is committed at `CommandList::end` when it
    /// changed. Only the list's recording thread touches it.
    class IcbPool
    {
      public:
        /// Returns the next slot of `kind` for a counted draw of `maxCount` records, making its buffer on `device` when the
        /// slot is new or too small: the growth path opens an autorelease pool, makes the list's residency set at its first
        /// buffer, and counts each native object it makes in `core`'s `nativeCreateCalls`. A buffer Metal refuses is fatal.
        const IcbSlot& take(MTL::Device* device, core::DeviceCore& core, IcbKind kind, uint32_t maxCount);

        /// Makes `commandBuffer` use the list's residency set, once per recording; counts the call on `list`. A buffer must
        /// have been taken.
        void attach(core::CommandListCore& list, MTL4::CommandBuffer* commandBuffer);

        /// Commits the residency set, inside an autorelease pool, when a buffer joined or left it since the last commit;
        /// returns true when it committed.
        bool commit();

        /// Starts a new recording: every slot is free again and the residency set is not yet used.
        void reset();

        /// Releases every buffer and the residency set. The caller holds an autorelease pool and no queue holds work that uses
        /// them.
        void release();

        /// Returns how many slots of `kind` exist.
        size_t slots(IcbKind kind) const
        {
            return m_slots[static_cast<size_t>(kind)].size();
        }

        /// Returns the buffer of slot `index` of `kind`.
        const MTL::IndirectCommandBuffer* buffer(IcbKind kind, size_t index) const
        {
            return m_slots[static_cast<size_t>(kind)][index].buffer;
        }

        /// Returns the commands slot `index` of `kind` holds.
        uint32_t capacity(IcbKind kind, size_t index) const
        {
            return m_slots[static_cast<size_t>(kind)][index].capacity;
        }

        /// Returns the list's residency set, or null before its first buffer.
        const MTL::ResidencySet* residencySet() const
        {
            return m_set;
        }

        /// Returns true when the recording's command buffer uses the residency set.
        bool attached() const
        {
            return m_attached;
        }

      private:
        std::array<std::vector<IcbSlot>, kIcbKinds> m_slots; ///< Per kind, the slots in the order a recording takes them.
        std::array<uint32_t, kIcbKinds> m_cursor = {};       ///< Per kind, the next slot this recording takes.
        MTL::ResidencySet* m_set = nullptr;                  ///< The list's residency set, one retained reference.
        bool m_attached = false;                             ///< This recording's command buffer uses `m_set`.
        bool m_changed = false;                              ///< A buffer joined or left `m_set` since its last commit.
    };
} // namespace rojoRHI::metal4
