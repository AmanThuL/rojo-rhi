//----------------------------------------------------------------------------------------------------------------------
/// @file AbiAtomic.slang
/// @brief Contended uint atomic additions, preserving each invocation's returned old value.
//----------------------------------------------------------------------------------------------------------------------

import RojoRHI;
import AtomicTest;

[shader("compute")]
[numthreads(64, 1, 1)]
void fanInMain(uint3 tid : SV_DispatchThreadID)
{
    AtomicRoot root = rojoRHI::root<AtomicRoot>();
    if (tid.x >= root.count)
        return;
    uint old = rojoRHI::atomicAdd(root.target, root.index, root.value);
    rojoRHI::store<uint>(root.output, tid.x, old);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void divergentMain(uint3 tid : SV_DispatchThreadID)
{
    AtomicRoot root = rojoRHI::root<AtomicRoot>();
    if (tid.x >= root.count)
        return;
    rojoRHI::Addr target = rojoRHI::load<rojoRHI::Addr>(root.target, tid.x & 1);
    uint old = rojoRHI::atomicAdd(rojoRHI::nonUniform(target), root.index, root.value);
    rojoRHI::store<uint>(root.output, tid.x, old);
}
