//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Icb.cpp
/// @brief Implements the indirect command buffers of a Metal 4 command list's counted draws, resident through the device's set.
//----------------------------------------------------------------------------------------------------------------------

#include "Metal4Icb.h"

#include "Base/Log.h"
#include "Core/CommandLists.h"
#include "Core/Counters.h"
#include "Core/DeviceCore.h"
#include "Metal4Common.h"

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string_view>
#include <vector>

namespace rojoRHI::metal4 {
    namespace {
        /// The label of each kind's indirect command buffers, indexed by `IcbKind`.
        constexpr std::array<std::string_view, kIcbKinds> kIcbLabels{ "rojorhi.list.icb.draw", "rojorhi.list.icb.indexed", "rojorhi.list.icb.mesh" };

        /// The command type of each kind's indirect command buffers, indexed by `IcbKind`.
        constexpr std::array<MTL::IndirectCommandType, kIcbKinds> kCommandTypes{ MTL::IndirectCommandTypeDraw,
                                                                                 MTL::IndirectCommandTypeDrawIndexed,
                                                                                 MTL::IndirectCommandTypeDrawMeshThreadgroups };

        /// The buffers a mesh command binds on each of the object, mesh and fragment stages: the four reserved bindings
        /// (ADR 0018).
        constexpr NS::UInteger kMeshBufferBindings = 4;

        //==============================================================================================================
        /// Makes an indirect command buffer of `kind` with `capacity` commands on `device`, labelled. Its commands take the
        /// pipeline and every piece of render state from the render encoder that runs them. Draw and indexed commands also take
        /// its buffer bindings and bind nothing themselves. A mesh command binds its own record, which a command that inherits
        /// buffers cannot, so it inherits none and binds all four reserved buffers on the object, mesh and fragment stages. The
        /// caller holds an autorelease pool. Returns null when Metal refuses it.
        MTL::IndirectCommandBuffer* newIcb(MTL::Device* device, IcbKind kind, uint32_t capacity)
        {
            MTL::IndirectCommandBufferDescriptor* descriptor = MTL::IndirectCommandBufferDescriptor::alloc()->init();
            descriptor->setCommandTypes(kCommandTypes[static_cast<size_t>(kind)]);
            const bool mesh = kind == IcbKind::Mesh;
            descriptor->setInheritPipelineState(true);
            descriptor->setInheritBuffers(!mesh);
            descriptor->setInheritCullMode(true);
            descriptor->setInheritDepthBias(true);
            descriptor->setInheritDepthClipMode(true);
            descriptor->setInheritDepthStencilState(true);
            descriptor->setInheritFrontFacingWinding(true);
            descriptor->setInheritTriangleFillMode(true);
            descriptor->setMaxVertexBufferBindCount(0);
            descriptor->setMaxFragmentBufferBindCount(mesh ? kMeshBufferBindings : 0);
            if (mesh)
            {
                descriptor->setMaxObjectBufferBindCount(kMeshBufferBindings);
                descriptor->setMaxMeshBufferBindCount(kMeshBufferBindings);
            }
            MTL::IndirectCommandBuffer* buffer = device->newIndirectCommandBuffer(descriptor, capacity, MTL::ResourceStorageModePrivate);
            descriptor->release();
            if (buffer != nullptr)
            {
                buffer->setLabel(makeString(kIcbLabels[static_cast<size_t>(kind)]));
            }
            return buffer;
        }
    } // namespace

    //==================================================================================================================
    const IcbSlot& IcbPool::take(MTL::Device* device, core::DeviceCore& core, IcbKind kind, uint32_t maxCount)
    {
        std::vector<IcbSlot>& slots = m_slots[static_cast<size_t>(kind)];
        uint32_t& cursor = m_cursor[static_cast<size_t>(kind)];
        if (cursor < slots.size() && slots[cursor].capacity >= maxCount)
        {
            return slots[cursor++];
        }
        // The growth path, which a list that records the same counted draws again never takes. The factory and the label
        // autorelease.
        const AutoreleasePool pool;
        const uint32_t capacity = std::bit_ceil(std::max(maxCount, kIcbMinimumCommands));
        MTL::IndirectCommandBuffer* buffer = newIcb(device, kind, capacity);
        if (buffer == nullptr)
        {
            ROJORHI_LOG_FATAL("an indirect command buffer {} of {} commands was not created", kIcbLabels[static_cast<size_t>(kind)], capacity);
            std::abort();
        }
        core::DeviceCounts::add(core.counts().nativeCreateCalls);
        if (cursor < slots.size())
        {
            // The slot's last use retired before the list was acquired again. A buffer that never reached the set, from a
            // recording never submitted, goes at once; one in the set leaves it at the hand-over.
            MTL::IndirectCommandBuffer* replaced = slots[cursor].buffer;
            if (const auto joining = std::find(m_joining.begin(), m_joining.end(), replaced); joining != m_joining.end())
            {
                m_joining.erase(joining);
                replaced->release();
            }
            else
            {
                m_leaving.push_back(replaced);
            }
            slots[cursor] = IcbSlot{ buffer, buffer->gpuResourceID(), capacity };
        }
        else
        {
            slots.push_back(IcbSlot{ buffer, buffer->gpuResourceID(), capacity });
        }
        m_joining.push_back(buffer);
        return slots[cursor++];
    }

    //==================================================================================================================
    void IcbPool::handOverChanges(Residency& residency)
    {
        for (const MTL::IndirectCommandBuffer* buffer : m_joining)
        {
            residency.add(buffer);
        }
        m_joining.clear();
        if (!m_leaving.empty())
        {
            // A release may autorelease, and a submission holds no pool of its own.
            const AutoreleasePool pool;
            for (MTL::IndirectCommandBuffer* buffer : m_leaving)
            {
                residency.remove(buffer);
                buffer->release();
            }
            m_leaving.clear();
        }
    }

    //==================================================================================================================
    void IcbPool::reset()
    {
        m_cursor = {};
    }

    //==================================================================================================================
    void IcbPool::release(Residency& residency)
    {
        for (std::vector<IcbSlot>& slots : m_slots)
        {
            for (const IcbSlot& slot : slots)
            {
                if (std::find(m_joining.begin(), m_joining.end(), slot.buffer) == m_joining.end())
                {
                    residency.remove(slot.buffer);
                }
                slot.buffer->release();
            }
            slots.clear();
        }
        for (MTL::IndirectCommandBuffer* buffer : m_leaving)
        {
            residency.remove(buffer);
            buffer->release();
        }
        m_joining.clear();
        m_leaving.clear();
        m_cursor = {};
    }
} // namespace rojoRHI::metal4
