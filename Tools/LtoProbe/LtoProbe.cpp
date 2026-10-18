//----------------------------------------------------------------------------------------------------------------------
/// @file LtoProbe.cpp
/// @brief Calls one trivial library entry from a function that Tools/check_lto_inline.py disassembles.
//----------------------------------------------------------------------------------------------------------------------

#include <rojoRHI/RHI.h>

#include <cstdio>

#if defined(_MSC_VER)
#define ROJORHI_PROBE_NOINLINE __declspec(noinline)
#else
#define ROJORHI_PROBE_NOINLINE __attribute__((noinline, used))
#endif

//======================================================================================================================
// With link-time optimization the call to Device::capabilities inlines here, so the disassembly holds no call to it.
extern "C" ROJORHI_PROBE_NOINLINE uint32_t rojorhiLtoProbe(const rojoRHI::Device& device)
{
    return device.capabilities().maxViews;
}

//======================================================================================================================
int main()
{
    rojoRHI::Result<rojoRHI::Device> device = rojoRHI::createDevice();
    if (!device)
    {
        std::fprintf(stderr, "createDevice failed: %s\n", device.error().message.c_str());
        return 1;
    }
    const uint32_t maxViews = rojorhiLtoProbe(*device);
    rojoRHI::destroyDevice(*device);
    std::printf("maxViews %u\n", maxViews);
    return maxViews == 0 ? 1 : 0;
}
