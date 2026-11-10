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
    SaxpyRoot root = rojoRHI::root<SaxpyRoot>();
    if (thread.x >= root.count)
    {
        return;
    }

    float x = rojoRHI::load<float>(root.x, thread.x);
    float y = rojoRHI::load<float>(root.y, thread.x);
    rojoRHI::store<float>(root.result, thread.x, root.a * x + y);
}
