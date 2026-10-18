//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Scopes.cpp
/// @brief Implements the scope entries for the Metal 4 backend: each scope closes the open encoder and is a debug group on the list's command buffer.
//----------------------------------------------------------------------------------------------------------------------

#include "Metal4Device.h"

#include <rojoRHI/RHI.h>

#include "Core/CommandLists.h"
#include "Core/RenderPasses.h"

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
        // The debug group goes on the command buffer, which takes it only between encoders.
        list.encoders.close(list.core);
        metal4::ListState& state = list.state;
        // The record's label is not NUL-terminated; the string is made with alloc and init, so it is not autoreleased and no
        // pool is opened. It stays on the label stack for the encoders the scope encloses.
        state.scratch.assign(list.core.scopeLog.back().label);
        NS::String* string = NS::String::alloc()->init(state.scratch.c_str(), NS::UTF8StringEncoding);
        if (string == nullptr)
        {
            // A label that is not valid UTF-8 makes no string, and a debug group needs one: the group takes the default label.
            string = NS::String::alloc()->init("rojorhi.scope.unnamed", NS::UTF8StringEncoding);
        }
        state.commandBuffer->pushDebugGroup(string);
        core::noteNativeCall(list.core);
        state.scopeLabels.push_back(string);
    }

    //==================================================================================================================
    void CommandList::endScope()
    {
        CommandListImpl& list = CommandListImpl::of(*this);
        core::endScope(list.core);
        list.encoders.close(list.core);
        metal4::ListState& state = list.state;
        state.commandBuffer->popDebugGroup();
        core::noteNativeCall(list.core);
        state.scopeLabels.back()->release();
        state.scopeLabels.pop_back();
    }
} // namespace rojoRHI
