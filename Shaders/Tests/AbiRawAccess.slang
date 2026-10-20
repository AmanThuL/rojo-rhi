//----------------------------------------------------------------------------------------------------------------------
/// @file AbiRawAccess.slang
/// @brief Compile-only kernel whose DXIL shows the raw accesses of the module: a flat root of which two fields are
/// read, one 16-byte load and one 16-byte store, checked by Tools/check_dxil.py.
//----------------------------------------------------------------------------------------------------------------------

import RojoRHI;

struct RawAccessRoot
{
    rojoRHI::Addr input;
    rojoRHI::Addr output;
    rojoRHI::Addr unused0;
    rojoRHI::Addr unused1;
    uint4 unusedTail;
};

[shader("compute")]
[numthreads(64, 1, 1)]
void rawAccessMain(uint3 tid : SV_DispatchThreadID)
{
    RawAccessRoot root = rojoRHI::root<RawAccessRoot>();
    uint4 value = rojoRHI::load<uint4>(root.input, tid.x);
    rojoRHI::store<uint4>(root.output, tid.x, value + 1);
}
