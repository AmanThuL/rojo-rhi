//----------------------------------------------------------------------------------------------------------------------
/// @file NullCapabilitiesTests.cpp
/// @brief Tests what the Null backend reports, which no other backend need match.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include <catch2/catch_test_macros.hpp>

using namespace rojoRHI;

//======================================================================================================================
TEST_CASE("the Null device reports every optional feature and the floor name Null", "[null]")
{
    Device device = *createDevice();
    const Capabilities& caps = device.capabilities();
    CHECK(caps.adapterName == "Null");
    CHECK(caps.floorName == "Null");
    CHECK(caps.uploadInDeviceMemory);
    CHECK(caps.placementSparse);
    CHECK(caps.meshTasksIndirect);
    CHECK(caps.conservativeRaster);
    REQUIRE(caps.upscalers.size() == 4);
    CHECK(caps.upscalers[0].kind == UpscalerKind::MetalFX);
    CHECK(caps.upscalers[1].kind == UpscalerKind::DLSS);
    CHECK(caps.upscalers[2].kind == UpscalerKind::FSR);
    CHECK(caps.upscalers[3].kind == UpscalerKind::XeSS);
    for (const UpscalerSupport& support : caps.upscalers)
    {
        CHECK(support.minInputScale == 0.25f);
        CHECK(support.maxInputScale == 1.0f);
    }
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("the Null limits are the Floor column of the reference", "[null]")
{
    Device device = *createDevice();
    const Capabilities& caps = device.capabilities();
    const Limits& limits = caps.limits;
    CHECK(caps.maxViews == 1'000'000);
    CHECK(caps.maxSamplers == 996);
    CHECK(limits.maxViews == 1'000'000);
    CHECK(limits.maxSamplers == 996);
    CHECK(limits.maxRootBlockSize == 65'536);
    CHECK(limits.rootAlignment == 256);
    CHECK(limits.frameDataAlignment == 256);
    CHECK(limits.maxBufferSize == (1ull << 32));
    CHECK(limits.maxAddrOffset == 536'870'911ull);
    CHECK(limits.maxTextureDimension1D == 16'384);
    CHECK(limits.maxTextureDimension2D == 16'384);
    CHECK(limits.maxTextureDimension3D == 2'048);
    CHECK(limits.maxTextureDimensionCube == 16'384);
    CHECK(limits.maxTextureLayers == 2'048);
    CHECK(limits.maxColorAttachments == 8);
    CHECK(limits.maxColorBytesPerSample == 64);
    CHECK(limits.maxSamplerAnisotropy == 16);
    CHECK(limits.maxDrawIndirectCount == 65'535);
    CHECK(limits.maxDispatchGroups == 65'535);
    CHECK(limits.maxThreadsPerGroup == 1'024);
    CHECK(limits.maxGroupSize[0] == 1'024);
    CHECK(limits.maxGroupSize[1] == 1'024);
    CHECK(limits.maxGroupSize[2] == 64);
    CHECK(limits.maxGroupSharedMemory == 32'768);
    CHECK(limits.maxMeshTaskGroups == 65'535);
    CHECK(limits.maxMeshTaskGroupsTotal == 4'194'304);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a Null device with validation selected is created in Debug and unsupported in Release", "[null]")
{
    Result<Device> created = createDevice({ .validation = true });
#ifdef NDEBUG
    CHECK_FALSE(created.has_value());
#else
    REQUIRE(created.has_value());
    destroyDevice(*created);
#endif
}
