//----------------------------------------------------------------------------------------------------------------------
/// @file AtomicTest.slang
/// @brief The shared flat root for the address-first atomic-add conformance kernels and layout probe.
//----------------------------------------------------------------------------------------------------------------------
module AtomicTest;

import RojoRHI;

public struct AtomicRoot
{
    public rojoRHI::Addr target;
    public rojoRHI::Addr output;
    public uint count;
    public uint index;
    public uint value;
    public uint pad;
};
