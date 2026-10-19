//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Icb.cpp
/// @brief Implements the indirect command buffers of a Metal 4 command list's counted draws and the list's residency set.
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
        // The growth path, which a list that records the same counted draws again never takes. The factories and the label
        // autorelease.
        const AutoreleasePool pool;
        if (m_set == nullptr)
        {
            MTL::ResidencySetDescriptor* descriptor = MTL::ResidencySetDescriptor::alloc()->init();
            descriptor->setLabel(makeString("rojorhi.list.residency"));
            NS::Error* error = nullptr;
            m_set = device->newResidencySet(descriptor, &error);
            descriptor->release();
            if (m_set == nullptr)
            {
                ROJORHI_LOG_ERROR("a command list's residency set rojorhi.list.residency was not created: {}", describe(error));
                std::abort();
            }
            core::DeviceCounts::add(core.counts().nativeCreateCalls);
        }
        const uint32_t capacity = std::bit_ceil(std::max(maxCount, kIcbMinimumCommands));
        MTL::IndirectCommandBuffer* buffer = newIcb(device, kind, capacity);
        if (buffer == nullptr)
        {
            ROJORHI_LOG_ERROR("an indirect command buffer {} of {} commands was not created", kIcbLabels[static_cast<size_t>(kind)], capacity);
            std::abort();
        }
        core::DeviceCounts::add(core.counts().nativeCreateCalls);
        m_set->addAllocation(buffer);
        if (cursor < slots.size())
        {
            // The slot's last use retired before the list was acquired again.
            m_set->removeAllocation(slots[cursor].buffer);
            slots[cursor].buffer->release();
            slots[cursor] = IcbSlot{ buffer, buffer->gpuResourceID(), capacity };
        }
        else
        {
            slots.push_back(IcbSlot{ buffer, buffer->gpuResourceID(), capacity });
        }
        m_changed = true;
        return slots[cursor++];
    }

    //==================================================================================================================
    void IcbPool::attach(core::CommandListCore& list, MTL4::CommandBuffer* commandBuffer)
    {
        if (m_attached)
        {
            return;
        }
        commandBuffer->useResidencySet(m_set);
        core::noteNativeCall(list);
        m_attached = true;
    }

    //==================================================================================================================
    bool IcbPool::commit()
    {
        if (!m_changed)
        {
            return false;
        }
        // The commit autoreleases.
        const AutoreleasePool pool;
        m_set->commit();
        m_changed = false;
        return true;
    }

    //==================================================================================================================
    void IcbPool::reset()
    {
        m_cursor = {};
        m_attached = false;
    }

    //==================================================================================================================
    void IcbPool::release()
    {
        for (std::vector<IcbSlot>& slots : m_slots)
        {
            for (const IcbSlot& slot : slots)
            {
                slot.buffer->release();
            }
            slots.clear();
        }
        if (m_set != nullptr)
        {
            m_set->release();
            m_set = nullptr;
        }
        m_cursor = {};
        m_attached = false;
        m_changed = false;
    }
} // namespace rojoRHI::metal4
