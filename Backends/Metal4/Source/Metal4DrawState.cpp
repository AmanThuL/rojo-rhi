//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4DrawState.cpp
/// @brief Implements the reset and release of what a Metal 4 command list keeps of its open render pass, and its prefix.
//----------------------------------------------------------------------------------------------------------------------

#include "Metal4DrawState.h"

#include "Core/ScopeRecord.h"
#include "Metal4Common.h"

#include <Metal/Metal.hpp>

#include <cstddef>
#include <cstdint>

namespace rojoRHI::metal4 {
    //==================================================================================================================
    void DrawState::reset()
    {
        endPass();
        root = 0;
        tableRoot = 0;
        tableDrawRecord = 0;
        zeroRecord = 0;
        resolved = {};
        icbs.reset();
    }

    //==================================================================================================================
    void DrawState::endPass()
    {
        colors.clear();
        depth = {};
        passOpen = false;
        resuming = false;
        passGroups = 0;
        passBegin = core::kNoTimestamp;
        pipeline = nullptr;
        topology = MTL::PrimitiveTypeTriangle;
        bias = {};
        held = {};
        clearPrefix();
        compute = nullptr;
    }

    //==================================================================================================================
    void DrawState::record(const PrefixCommand& command)
    {
        uint32_t& latest = prefixLatest[static_cast<size_t>(command.kind)];
        if (latest != 0)
        {
            prefix[latest - 1] = command;
            return;
        }
        prefix.push_back(command);
        latest = static_cast<uint32_t>(prefix.size());
    }

    //==================================================================================================================
    void DrawState::appendBarrierEntry(const PrefixCommand& command)
    {
        prefix.push_back(command);
        prefixLatest = {};
    }

    //==================================================================================================================
    void DrawState::clearPrefix()
    {
        prefix.clear();
        prefixLatest = {};
    }

    //==================================================================================================================
    void DrawState::release(Residency& residency)
    {
        reset();
        for (MTL::RenderPassColorAttachmentDescriptor* attachment : colorAttachments)
        {
            attachment->release();
        }
        colorAttachments.clear();
        colorHeld.clear();
        depthHeld = {};
        stencilHeld = {};
        if (depthAttachment != nullptr)
        {
            depthAttachment->release();
            depthAttachment = nullptr;
        }
        if (stencilAttachment != nullptr)
        {
            stencilAttachment->release();
            stencilAttachment = nullptr;
        }
        if (descriptor != nullptr)
        {
            descriptor->release();
            descriptor = nullptr;
        }
        descriptorColors = 0;
        icbs.release(residency);
    }

    //==================================================================================================================
    void DrawState::forgetTextures()
    {
        // Every attachment the descriptor held a texture in was configured through a kept attachment descriptor. The setters
        // only drop the descriptor's references: each texture's pool record keeps its own until the texture's deferred
        // destruction, which retires after `end`, so no texture is freed here and nothing here needs an autorelease pool.
        for (uint32_t i = 0; i < descriptorColors; ++i)
        {
            HeldAttachment& held = colorHeld[i];
            if (held.texture != nullptr)
            {
                colorAttachments[i]->setTexture(nullptr);
                held.texture = nullptr;
            }
            if (held.resolve != nullptr)
            {
                colorAttachments[i]->setResolveTexture(nullptr);
                held.resolve = nullptr;
            }
        }
        descriptorColors = 0;
        if (depthHeld.texture != nullptr)
        {
            depthAttachment->setTexture(nullptr);
            depthHeld.texture = nullptr;
        }
        if (stencilHeld.texture != nullptr)
        {
            stencilAttachment->setTexture(nullptr);
            stencilHeld.texture = nullptr;
        }
    }
} // namespace rojoRHI::metal4
