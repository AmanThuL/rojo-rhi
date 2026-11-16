//----------------------------------------------------------------------------------------------------------------------
/// @file TestShared.slang
/// @brief A shared test module with no entry point, reached through `slang.includedirs`, so that the build proves an
/// entry file can import a module that lives outside `Shaders/RojoRHI`.
//----------------------------------------------------------------------------------------------------------------------

module TestShared;

namespace rojoRHI::test
{
    /// The value `SharedImport.slang` writes, proving the import resolved.
    public uint sharedValue()
    {
        return 7;
    }
}
