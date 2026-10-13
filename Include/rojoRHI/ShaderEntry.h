//----------------------------------------------------------------------------------------------------------------------
/// @file ShaderEntry.h
/// @brief Declares the reference to one entry point of a loaded shader library.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/Handles.h>

#include <string_view>

namespace rojoRHI {
    /// Names one entry point of a loaded shader library. A default-constructed entry (null `library`) means the stage is
    /// absent. When the stage is present, `library` is live and `name` is not empty (VU-rojo-pipeline-0001) and names an entry
    /// point of `library` whose shader stage is the stage of the descriptor field holding the entry (VU-rojo-pipeline-0002).
    /// `Device::createPipeline` copies `name`, so it need not outlive the call.
    struct ShaderEntry
    {
        /// The library holding the entry point; null when the stage is absent.
        ShaderLibrary library;
        /// The entry point's name inside `library`.
        std::string_view name;
    };
} // namespace rojoRHI
