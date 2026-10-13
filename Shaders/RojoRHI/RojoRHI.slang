//----------------------------------------------------------------------------------------------------------------------
/// @file RojoRHI.slang
/// @brief The RojoRHI shader module: the reserved bindings and the entries every shader imports.
//----------------------------------------------------------------------------------------------------------------------

module RojoRHI;

namespace rojoRHI
{
    /// A 64-bit device address, the same two-word pair the C++ `Addr` carries.
    public typealias Addr = uint2;
}

/// The shader ABI version; changing a reserved binding bumps it.
public static const uint ROJORHI_SHADER_ABI_VERSION = 1;
