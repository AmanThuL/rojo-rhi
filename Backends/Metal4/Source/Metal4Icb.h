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
    enum class IcbKind : uint8_t
    {
        Draw,
        Indexed,
        Mesh,
    };

    inline constexpr size_t kIcbKinds = 3;

    // So the small counted draws of a list share one size.
    inline constexpr uint32_t kIcbMinimumCommands = 64;

    // Kept so a counted draw sends the buffer no message.
    struct IcbSlot
    {
        MTL::IndirectCommandBuffer* buffer = nullptr; // One retained reference.
        MTL::ResourceID id = {};
        uint32_t capacity = 0;
    };

    // Per kind, slot `i` serves the `i`-th counted draw of that kind in a recording, and the slots outlive the
    // recording. A list is acquired again only once its frame slot's work retired, so a slot replaced for a larger
    // `maxCount` is no longer in use. Recording takes no lock (ADR 0020), so it only notes the buffers made and
    // replaced; `Queue::submit` hands them to the device residency set before the command buffer commits. Only the
    // list's recording thread touches it until submission.
    class IcbPool
    {
      public:
        // The growth path opens an autorelease pool and counts each native object it makes. A buffer Metal refuses is
        // fatal.
        const IcbSlot& take(MTL::Device* device, core::DeviceCore& core, IcbKind kind, uint32_t maxCount);

        // Nothing to hand over costs two tests. `Queue::submit` calls it for each list before it commits the set.
        void handOver(Residency& residency)
        {
            if (!m_joining.empty() || !m_leaving.empty())
            {
                handOverChanges(residency);
            }
        }

        void reset();

        // The caller holds an autorelease pool and no queue holds work that uses them.
        void release(Residency& residency);

        size_t slots(IcbKind kind) const
        {
            return m_slots[static_cast<size_t>(kind)].size();
        }

        const MTL::IndirectCommandBuffer* buffer(IcbKind kind, size_t index) const
        {
            return m_slots[static_cast<size_t>(kind)][index].buffer;
        }

        uint32_t capacity(IcbKind kind, size_t index) const
        {
            return m_slots[static_cast<size_t>(kind)][index].capacity;
        }

      private:
        // The set's lock is taken once for each buffer that joins or leaves.
        void handOverChanges(Residency& residency);

        std::array<std::vector<IcbSlot>, kIcbKinds> m_slots;
        std::array<uint32_t, kIcbKinds> m_cursor = {};
        std::vector<MTL::IndirectCommandBuffer*> m_joining; // The slots own them.
        std::vector<MTL::IndirectCommandBuffer*> m_leaving; // One reference each.
    };
} // namespace rojoRHI::metal4
