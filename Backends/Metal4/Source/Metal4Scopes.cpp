//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Scopes.cpp
/// @brief Implements the Metal 4 scope entries: debug groups and timestamps, on the command buffer outside a pass, on the render encoder inside.
//----------------------------------------------------------------------------------------------------------------------

#include "Metal4Device.h"

#include <rojoRHI/RHI.h>

#include "Core/CommandLists.h"
#include "Core/RenderPasses.h"
#include "Metal4Common.h"
#include "Metal4DrawState.h"
#include "Metal4Timestamps.h"

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include <string_view>

namespace rojoRHI {
    //==================================================================================================================
    void CommandList::beginScope(std::string_view label)
    {
        CommandListImpl& list = CommandListImpl::of(*this);
        // The core records the scope with its label in the list's arena page, or its default label, which the record keeps.
        core::beginScope(list.core, label);
        metal4::ListState& state = list.state;
        // The pair is claimed with one atomic add on the frame slot's cursor, or is `core::kNoTimestamp` on a device without
        // scope timings; the record keeps it for the frame wait's resolve.
        const uint32_t pair = metal4::claimPair(list);
        // A scope with no debug marker (ADR 0034) and no timestamp sends Metal nothing, and leaves the open encoder open.
        if (!state.markers && pair == core::kNoTimestamp)
        {
            return;
        }
        if (list.draw.passOpen)
        {
            // Inside a pass the group and the timestamps go on the render encoder, which opens now if it has not, under the
            // enclosing label. A split ends the encoder and pushes the group again on the resumed one, but writes no timestamp.
            MTL4::RenderCommandEncoder* encoder = metal4::ensureRender(list);
            if (state.markers)
            {
                NS::String* string = state.makeGroupLabel(list.core.scopeLog.back().label, "rojorhi.scope.unnamed");
                {
                    const metal4::AutoreleasePool pool;
                    encoder->pushDebugGroup(string);
                }
                core::noteNativeCall(list.core);
                state.scopeLabels.push_back(string);
                ++list.draw.passGroups;
            }
            metal4::writeInPass(list, encoder, pair);
            return;
        }
        // The debug group and the timestamp go on the command buffer, which takes them only between encoders.
        list.encoders.close(list.core);
        if (state.markers)
        {
            // The string stays on the label stack for the encoders the scope encloses.
            NS::String* string = state.makeGroupLabel(list.core.scopeLog.back().label, "rojorhi.scope.unnamed");
            {
                const metal4::AutoreleasePool pool;
                state.commandBuffer->pushDebugGroup(string);
            }
            core::noteNativeCall(list.core);
            state.scopeLabels.push_back(string);
        }
        metal4::writeBetweenEncoders(list, pair);
    }

    //==================================================================================================================
    void CommandList::endScope()
    {
        CommandListImpl& list = CommandListImpl::of(*this);
        core::endScope(list.core);
        metal4::ListState& state = list.state;
        const uint32_t end = metal4::popPairEnd(list);
        if (!state.markers && end == core::kNoTimestamp)
        {
            return;
        }
        // A scope ends on the side of the pass boundary it began on (VU-rojo-cmd-0033): one ending inside a pass writes its end
        // timestamp on the render encoder open now, which a split may have resumed since the scope began.
        if (list.draw.passOpen)
        {
            MTL4::RenderCommandEncoder* encoder = metal4::ensureRender(list);
            metal4::writeInPass(list, encoder, end);
            if (state.markers)
            {
                encoder->popDebugGroup();
                core::noteNativeCall(list.core);
                state.scopeLabels.back()->release();
                state.scopeLabels.pop_back();
                --list.draw.passGroups;
            }
            return;
        }
        list.encoders.close(list.core);
        metal4::writeBetweenEncoders(list, end);
        if (state.markers)
        {
            state.commandBuffer->popDebugGroup();
            core::noteNativeCall(list.core);
            state.scopeLabels.back()->release();
            state.scopeLabels.pop_back();
        }
    }
} // namespace rojoRHI
