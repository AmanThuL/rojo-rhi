//----------------------------------------------------------------------------------------------------------------------
/// @file AbiWriteOnly.slang
/// @brief Conformance kernels, one per write-only texture form: each stores the root's value into one view slot of
/// the module at a fixed coordinate, so a readback checks one form at a time.
//----------------------------------------------------------------------------------------------------------------------

import RojoRHI;

struct WriteRoot
{
    uint view;
    uint pad0;
    uint pad1;
    uint pad2;
    float4 value;
};

[shader("compute")]
[numthreads(1, 1, 1)]
void wTexture1DMain()
{
    WriteRoot root = rojoRHI::root<WriteRoot>();
    rojoRHI::wTexture1D(root.view).Store(1, root.value);
}

[shader("compute")]
[numthreads(1, 1, 1)]
void wTexture1DArrayMain()
{
    WriteRoot root = rojoRHI::root<WriteRoot>();
    rojoRHI::wTexture1DArray(root.view).Store(uint2(1, 1), root.value);
}

[shader("compute")]
[numthreads(1, 1, 1)]
void wTexture2DMain()
{
    WriteRoot root = rojoRHI::root<WriteRoot>();
    rojoRHI::wTexture2D(root.view).Store(uint2(1, 1), root.value);
}

[shader("compute")]
[numthreads(1, 1, 1)]
void wTexture2DArrayMain()
{
    WriteRoot root = rojoRHI::root<WriteRoot>();
    rojoRHI::wTexture2DArray(root.view).Store(uint3(1, 1, 1), root.value);
}

[shader("compute")]
[numthreads(1, 1, 1)]
void wTexture3DMain()
{
    WriteRoot root = rojoRHI::root<WriteRoot>();
    rojoRHI::wTexture3D(root.view).Store(uint3(1, 1, 1), root.value);
}
