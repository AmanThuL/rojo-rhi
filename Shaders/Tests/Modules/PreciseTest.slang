//----------------------------------------------------------------------------------------------------------------------
/// @file PreciseTest.slang
/// @brief The shared flat root for the precise multiply-add conformance kernel and layout probe.
//----------------------------------------------------------------------------------------------------------------------
module PreciseTest;

import RojoRHI;

public struct PreciseRoot
{
    public rojoRHI::Addr input;
    public rojoRHI::Addr output;
};
