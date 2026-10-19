//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Scopes.cpp
/// @brief Implements the scope entries for the Metal 4 backend: debug groups on the command buffer, or on the render encoder inside a pass.
//----------------------------------------------------------------------------------------------------------------------

#include "Metal4Device.h"

#include <rojoRHI/RHI.h>

#include "Core/CommandLists.h"
#include "Core/RenderPasses.h"
#include "Metal4DrawState.h"

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
        if (list.draw.passOpen)
        {
            // Inside a pass the group goes on the render encoder, which opens now if it has not, under the enclosing label.
            MTL4::RenderCommandEncoder* encoder = metal4::ensureRender(list);
            NS::String* string = state.makeGroupLabel(list.core.scopeLog.back().label, "rojorhi.scope.unnamed");
            encoder->pushDebugGroup(string);
            core::noteNativeCall(list.core);
            state.scopeLabels.push_back(string);
            ++list.draw.passGroups;
            return;
        }
        // The debug group goes on the command buffer, which takes it only between encoders.
        list.encoders.close(list.core);
        // The string stays on the label stack for the encoders the scope encloses.
        NS::String* string = state.makeGroupLabel(list.core.scopeLog.back().label, "rojorhi.scope.unnamed");
        state.commandBuffer->pushDebugGroup(string);
        core::noteNativeCall(list.core);
        state.scopeLabels.push_back(string);
    }

    //==================================================================================================================
    void CommandList::endScope()
    {
        CommandListImpl& list = CommandListImpl::of(*this);
        core::endScope(list.core);
        metal4::ListState& state = list.state;
        // A scope ends on the side of the pass boundary it began on (VU-rojo-cmd-0033), so one ending inside a pass began there.
        if (list.draw.passOpen)
        {
            metal4::ensureRender(list)->popDebugGroup();
            core::noteNativeCall(list.core);
            state.scopeLabels.back()->release();
            state.scopeLabels.pop_back();
            --list.draw.passGroups;
            return;
        }
        list.encoders.close(list.core);
        state.commandBuffer->popDebugGroup();
        core::noteNativeCall(list.core);
        state.scopeLabels.back()->release();
        state.scopeLabels.pop_back();
    }
} // namespace rojoRHI
