//----------------------------------------------------------------------------------------------------------------------
/// @file BenchCompute.slang
/// @brief C4 stores its root's value at its root's address; C6 adds one to a counter at its root's address.
//----------------------------------------------------------------------------------------------------------------------

import RojoRHI;

// Mirrors StoreRoot in Benchmarks/Common/CpuRoots.h: 16 bytes.
struct StoreRoot
{
    rojoRHI::Addr address;
    uint value;
    uint reserved;
};

// Mirrors CounterRoot in Benchmarks/Common/CpuRoots.h: 8 bytes.
struct CounterRoot
{
    rojoRHI::Addr counter;
};

// Every thread of the group stores the same value to the same address.
[shader("compute")]
[numthreads(64, 1, 1)]
void storeMain(uint3 thread : SV_GroupThreadID)
{
    StoreRoot root = rojoRHI::root<StoreRoot>();
    rojoRHI::store<uint>(root.address, 0, root.value);
}

// One thread adds one; the barrier after each dispatch orders the next dispatch's read after this write.
[shader("compute")]
[numthreads(64, 1, 1)]
void countMain(uint3 thread : SV_GroupThreadID)
{
    if (thread.x == 0)
    {
        CounterRoot root = rojoRHI::root<CounterRoot>();
        rojoRHI::store<uint>(root.counter, 0, rojoRHI::load<uint>(root.counter, 0) + 1);
    }
}
