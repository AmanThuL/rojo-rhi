//----------------------------------------------------------------------------------------------------------------------
/// @file FragmentStoreOracle.slang
/// @brief Conformance fragment producer: a fragment entry that spins a while, then stores one `uint4` for its pixel at
/// the address its root names, so a copy of that buffer shows whether the fragment work finished before it. It is alone
/// in its file because Slang passes every reserved binding that any entry of a file touches to every entry, which
/// would give a vertex entry writable buffer parameters.
//----------------------------------------------------------------------------------------------------------------------

import RojoRHI;

struct StoreRoot
{
    rojoRHI::Addr destination;
    uint iterations;
    uint zero;
    uint edge;
    uint pad0;
    uint pad1;
    uint pad2;
    uint pad3;
};

// The record of pixel `cell`: the case computes the same four words.
[shader("fragment")]
float4 storeFragmentMain(float4 position : SV_Position) : SV_Target
{
    StoreRoot root = rojoRHI::root<StoreRoot>();
    uint cell = uint(position.y) * root.edge + uint(position.x);
    // Every step keeps the value odd, and `root.zero` is zero, so the compare below never holds; the compiler cannot know
    // that, so it keeps the loop.
    uint spin = cell | 1u;
    for (uint step = 0; step < root.iterations; ++step)
    {
        spin = (spin * 1664525u + 1013904223u) | 1u;
    }
    rojoRHI::store<uint4>(root.destination, cell, uint4(cell, ~cell, 0xF00DF00Du, spin == root.zero ? 1u : 0u));
    return float4(0.25, 0.5, 0.75, 1.0);
}
