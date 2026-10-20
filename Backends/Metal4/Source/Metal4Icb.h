//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Icb.h
/// @brief Declares the indirect command buffers a Metal 4 command list keeps for its counted draws, resident through the device's set.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include "Core/CommandLists.h"
#include "Core/DeviceCore.h"
#include "Metal4Residency.h"

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

    /// The indirect command buffers of one command list. Per kind, slot `i` serves the `i`-th counted draw of that kind in a
    /// recording; the slots outlive the recording, so a list that records the same counted draws again makes no native object.
    /// A slot whose capacity is below a draw's `maxCount` is replaced by one of `bit_ceil(max(maxCount, kIcbMinimumCommands))`
    /// commands: the list is acquired again only once its frame slot's work has retired, so the replaced buffer is no longer in
    /// use. The buffers are resident through the device's residency set, which every queue keeps: recording only notes the
    /// buffers made and replaced, since it takes no lock (ADR 0020), and `Queue::submit` hands them over, so a buffer joins the
    /// set before the command buffer that uses it commits and a replaced one leaves it at the list's next submission, after its
    /// last use retired. Only the list's recording thread touches it until the list is submitted.
    class IcbPool
    {
      public:
        /// Returns the next slot of `kind` for a counted draw of `maxCount` records, making its buffer on `device` when the
        /// slot is new or too small: the growth path opens an autorelease pool and counts each native object it makes in
        /// `core`'s `nativeCreateCalls`. A buffer Metal refuses is fatal.
        const IcbSlot& take(MTL::Device* device, core::DeviceCore& core, IcbKind kind, uint32_t maxCount);

        /// Adds the buffers made since the last hand-over to `residency`, and removes and releases those they replaced.
        /// Nothing to hand over costs two tests. `Queue::submit` calls it for each list before it commits the set.
        void handOver(Residency& residency)
        {
            if (!m_joining.empty() || !m_leaving.empty())
            {
                handOverChanges(residency);
            }
        }

        /// Starts a new recording: every slot is free again.
        void reset();

        /// Releases every buffer, removing from `residency` each one that joined it. The caller holds an autorelease pool and
        /// no queue holds work that uses them.
        void release(Residency& residency);

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

      private:
        /// `handOver` when there is something to hand over: the set's lock is taken once for each buffer that joins or leaves.
        void handOverChanges(Residency& residency);

        std::array<std::vector<IcbSlot>, kIcbKinds> m_slots; ///< Per kind, the slots in the order a recording takes them.
        std::array<uint32_t, kIcbKinds> m_cursor = {};       ///< Per kind, the next slot this recording takes.
        std::vector<MTL::IndirectCommandBuffer*> m_joining;  ///< Buffers made since the last hand-over; the slots own them.
        std::vector<MTL::IndirectCommandBuffer*> m_leaving;  ///< Buffers replaced since the last hand-over, one reference each.
    };
} // namespace rojoRHI::metal4
