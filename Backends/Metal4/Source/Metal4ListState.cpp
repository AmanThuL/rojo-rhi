//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4ListState.cpp
/// @brief Implements the creation and release of the native objects a Metal 4 command list owns.
//----------------------------------------------------------------------------------------------------------------------

#include "Metal4ListState.h"

#include <rojoRHI/RHI.h>

#include "Metal4Common.h"

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include <cstdint>
#include <format>
#include <string>

namespace rojoRHI::metal4 {
    namespace {
        /// The buffer bindings of a list's argument table: the root, the view table, the sampler table and a mesh draw's record.
        constexpr NS::UInteger kTableBindings = 4;

        /// The binding of the view table's address (ADR 0018).
        constexpr NS::UInteger kViewTableBinding = 1;

        /// The binding of the sampler table's address (ADR 0018).
        constexpr NS::UInteger kSamplerTableBinding = 2;
    } // namespace

    //==================================================================================================================
    Result<void> ListState::create(MTL::Device* device, uint64_t viewTableAddress, uint64_t samplerTableAddress)
    {
        // The table's label and the error are autoreleased.
        const AutoreleasePool pool;
        allocator = device->newCommandAllocator();
        if (allocator == nullptr)
        {
            return std::unexpected(Error{ ErrorCategory::OutOfMemory, "the command allocator of a command list was not created" });
        }
        commandBuffer = device->newCommandBuffer();
        if (commandBuffer == nullptr)
        {
            release();
            return std::unexpected(Error{ ErrorCategory::OutOfMemory, "the command buffer of a command list was not created" });
        }
        MTL4::ArgumentTableDescriptor* descriptor = MTL4::ArgumentTableDescriptor::alloc()->init();
        descriptor->setMaxBufferBindCount(kTableBindings);
        descriptor->setInitializeBindings(true);
        descriptor->setLabel(makeString("rojorhi.list.table"));
        NS::Error* error = nullptr;
        table = device->newArgumentTable(descriptor, &error);
        descriptor->release();
        if (table == nullptr)
        {
            const std::string reason = describe(error);
            release();
            return std::unexpected(Error{ ErrorCategory::DriverFailure, std::format("the argument table of a command list was not created: {}", reason) });
        }
        table->setAddress(viewTableAddress, kViewTableBinding);
        table->setAddress(samplerTableAddress, kSamplerTableBinding);
        return {};
    }

    //==================================================================================================================
    void ListState::release()
    {
        for (auto it = scopeLabels.rbegin(); it != scopeLabels.rend(); ++it)
        {
            (*it)->release();
        }
        scopeLabels.clear();
        if (label != nullptr)
        {
            label->release();
            label = nullptr;
        }
        if (table != nullptr)
        {
            table->release();
            table = nullptr;
        }
        if (commandBuffer != nullptr)
        {
            commandBuffer->release();
            commandBuffer = nullptr;
        }
        if (allocator != nullptr)
        {
            allocator->release();
            allocator = nullptr;
        }
    }
} // namespace rojoRHI::metal4
