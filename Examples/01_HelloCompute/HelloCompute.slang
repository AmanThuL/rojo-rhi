import RojoRHI;

struct SaxpyRoot
{
    rojoRHI::Addr x;
    rojoRHI::Addr y;
    rojoRHI::Addr result;
    float a;
    uint count;
};

[shader("compute")]
[numthreads(64, 1, 1)]
void saxpy(uint3 thread : SV_DispatchThreadID)
{
    // Reads the block bound by pushRoot. The module owns that binding, so this shader declares none.
    SaxpyRoot root = rojoRHI::root<SaxpyRoot>();
    if (thread.x >= root.count)
    {
        return;
    }

    // load and store index elements of T past an Addr; only the module knows how each backend dereferences one.
    float x = rojoRHI::load<float>(root.x, thread.x);
    float y = rojoRHI::load<float>(root.y, thread.x);
    rojoRHI::store<float>(root.result, thread.x, root.a * x + y);
}
