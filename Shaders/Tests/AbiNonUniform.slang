//----------------------------------------------------------------------------------------------------------------------
/// @file AbiNonUniform.slang
/// @brief Compile-only kernels, one per heap fetch kind, each fed by a per-thread index marked with `nonUniform`, so
/// the DXIL of each shows whether the non-uniform flag reaches that kind's descriptor handle (Tools/check_dxil.py).
//----------------------------------------------------------------------------------------------------------------------

import RojoRHI;

struct NonUniformRoot
{
    rojoRHI::Addr evenBase;
    rojoRHI::Addr oddBase;
    rojoRHI::Addr output;
    uint evenView;
    uint oddView;
    uint evenSampler;
    uint oddSampler;
    uint pad0;
    uint pad1;
};

// The even or odd value of a pair, chosen per thread, so neighbouring threads use different descriptors.
rojoRHI::Addr perThread(uint thread, rojoRHI::Addr even, rojoRHI::Addr odd)
{
    return (thread & 1) != 0 ? odd : even;
}

uint perThread(uint thread, uint even, uint odd)
{
    return (thread & 1) != 0 ? odd : even;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void loadMain(uint3 tid : SV_DispatchThreadID)
{
    NonUniformRoot root = rojoRHI::root<NonUniformRoot>();
    rojoRHI::Addr base = rojoRHI::nonUniform(perThread(tid.x, root.evenBase, root.oddBase));
    uint4 value = rojoRHI::load<uint4>(base, tid.x);
    rojoRHI::store<uint4>(rojoRHI::nonUniform(root.output), tid.x, value);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void storeMain(uint3 tid : SV_DispatchThreadID)
{
    NonUniformRoot root = rojoRHI::root<NonUniformRoot>();
    rojoRHI::Addr base = rojoRHI::nonUniform(perThread(tid.x, root.evenBase, root.oddBase));
    rojoRHI::store<uint4>(base, tid.x, uint4(tid.x, 0, 0, 1));
}

[shader("compute")]
[numthreads(64, 1, 1)]
void atomicMain(uint3 tid : SV_DispatchThreadID)
{
    NonUniformRoot root = rojoRHI::root<NonUniformRoot>();
    uint old = rojoRHI::atomicAdd(rojoRHI::nonUniform(perThread(tid.x, root.evenBase, root.oddBase)), 3, 1);
    rojoRHI::store<uint>(rojoRHI::nonUniform(root.output), tid.x, old);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void textureMain(uint3 tid : SV_DispatchThreadID)
{
    NonUniformRoot root = rojoRHI::root<NonUniformRoot>();
    uint view = rojoRHI::nonUniform(perThread(tid.x, root.evenView, root.oddView));
    float4 value = rojoRHI::texture2D(view).Load(int3(0, 0, 0));
    rojoRHI::store<float4>(rojoRHI::nonUniform(root.output), tid.x, value);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void wTextureMain(uint3 tid : SV_DispatchThreadID)
{
    NonUniformRoot root = rojoRHI::root<NonUniformRoot>();
    uint view = rojoRHI::nonUniform(perThread(tid.x, root.evenView, root.oddView));
    rojoRHI::wTexture2D(view).Store(uint2(tid.x, 0), float4(1, 0, 0, 1));
}

[shader("compute")]
[numthreads(64, 1, 1)]
void samplerMain(uint3 tid : SV_DispatchThreadID)
{
    NonUniformRoot root = rojoRHI::root<NonUniformRoot>();
    uint index = rojoRHI::nonUniform(perThread(tid.x, root.evenSampler, root.oddSampler));
    SamplerState linearClamp = rojoRHI::sampler(index);
    float4 value = rojoRHI::texture2D(rojoRHI::nonUniform(root.evenView)).SampleLevel(linearClamp, float2(0.5, 0.5), 0);
    rojoRHI::store<float4>(rojoRHI::nonUniform(root.output), tid.x, value);
}
