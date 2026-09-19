//----------------------------------------------------------------------------------------------------------------------
/// @file ShaderLibrary.h
/// @brief Declares the loaded backend shader library handle.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

namespace rojoRHI {

/// Represents a loaded backend shader library.
class ShaderLibrary {
public:
    /// Destroys the shader library.
    virtual ~ShaderLibrary() = default;
};

} // namespace rojoRHI
