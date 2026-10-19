//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4DeviceTests.cpp
/// @brief Tests a Metal 4 device against the native device it wraps: capabilities, formats, frames and ID tables.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/Formats.h"
#include "Metal4Capabilities.h"
#include "Metal4Common.h"
#include "Metal4Device.h"
#include "Metal4Formats.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <Metal/Metal.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <utility>

using namespace rojoRHI;

namespace {
    //==================================================================================================================
    /// The native device a RojoRHI device wraps.
    MTL::Device* nativeOf(Device device)
    {
        return DeviceImpl::of(device).device;
    }

    //==================================================================================================================
    /// Runs `count` empty frames on `device`.
    void runFrames(Device device, uint32_t count)
    {
        for (uint32_t i = 0; i < count; ++i)
        {
            device.beginFrame();
            device.endFrame();
        }
    }
} // namespace

//======================================================================================================================
TEST_CASE("a Metal 4 device names its adapter and its Apple family", "[metal4]")
{
    Device device = *createDevice();
    MTL::Device* native = nativeOf(device);
    const Capabilities& caps = device.capabilities();
    CHECK_FALSE(caps.adapterName.empty());
    {
        // name() is a +0 getter whose string the pool collects.
        const metal4::AutoreleasePool pool;
        CHECK(caps.adapterName == metal4::toStdString(native->name()));
    }
    constexpr std::array<std::pair<MTL::GPUFamily, const char*>, 4> families = { {
        { MTL::GPUFamilyApple10, "Apple10" },
        { MTL::GPUFamilyApple9, "Apple9" },
        { MTL::GPUFamilyApple8, "Apple8" },
        { MTL::GPUFamilyApple7, "Apple7" },
    } };
    std::string expected;
    for (const auto& [family, name] : families)
    {
        if (native->supportsFamily(family))
        {
            expected = name;
            break;
        }
    }
    REQUIRE_FALSE(expected.empty());
    CHECK(caps.floorName == expected);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("the limits a Metal 4 device reports come from the device", "[metal4]")
{
    Device device = *createDevice();
    MTL::Device* native = nativeOf(device);
    const Capabilities& caps = device.capabilities();
    const Limits& limits = caps.limits;
    CHECK(limits.maxBufferSize == native->maxBufferLength());
    CHECK(limits.maxAddrOffset == limits.maxBufferSize - 1);
    CHECK(limits.maxGroupSharedMemory == native->maxThreadgroupMemoryLength());
    CHECK(limits.maxSamplers == native->maxArgumentBufferSamplerCount());
    CHECK(limits.maxViews == 1'000'000);
    CHECK(caps.maxViews == limits.maxViews);
    CHECK(caps.maxSamplers == limits.maxSamplers);
    const MTL::Size threads = native->maxThreadsPerThreadgroup();
    CHECK(limits.maxThreadsPerGroup == threads.width);
    CHECK(limits.maxGroupSize[0] == threads.width);
    CHECK(limits.maxGroupSize[1] == threads.height);
    CHECK(limits.maxGroupSize[2] == threads.depth);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("the floor check refuses a device below any floor item it reports", "[metal4]")
{
    Device device = *createDevice();
    const metal4::FloorReport met = metal4::floorReport(nativeOf(device));
    CHECK(metal4::checkFloor(met).has_value());
    // Each item exactly at its floor is accepted.
    metal4::FloorReport atFloor = met;
    atFloor.samplers = 996;
    atFloor.maxBufferLength = uint64_t{ 1 } << 32;
    atFloor.maxThreadsPerThreadgroup = MTL::Size(1'024, 1'024, 64);
    atFloor.maxThreadgroupMemoryLength = 32'768;
    CHECK(metal4::checkFloor(atFloor).has_value());
    const auto lowered = [&](const char* name, auto change) {
        CAPTURE(name);
        metal4::FloorReport report = met;
        change(report);
        const Result<void> checked = metal4::checkFloor(report);
        REQUIRE_FALSE(checked.has_value());
        CHECK(checked.error().category == ErrorCategory::Unsupported);
        CHECK(checked.error().message.find(name) != std::string::npos);
    };
    lowered("GPUFamilyMetal4", [](metal4::FloorReport& r) { r.metal4Family = false; });
    lowered("GPUFamilyApple7", [](metal4::FloorReport& r) { r.apple7Family = false; });
    lowered("ReadWriteTextureTier2", [](metal4::FloorReport& r) { r.readWriteTextureTier2 = false; });
    lowered("samplers", [](metal4::FloorReport& r) { r.samplers = 995; });
    lowered("maxBufferLength", [](metal4::FloorReport& r) { r.maxBufferLength = (uint64_t{ 1 } << 32) - 1; });
    lowered("maxThreadsPerThreadgroup", [](metal4::FloorReport& r) { r.maxThreadsPerThreadgroup.width = 512; });
    lowered("maxThreadsPerThreadgroup", [](metal4::FloorReport& r) { r.maxThreadsPerThreadgroup.height = 512; });
    lowered("maxThreadsPerThreadgroup", [](metal4::FloorReport& r) { r.maxThreadsPerThreadgroup.depth = 32; });
    lowered("maxThreadgroupMemoryLength", [](metal4::FloorReport& r) { r.maxThreadgroupMemoryLength = 16'384; });
    lowered("supportsBCTextureCompression", [](metal4::FloorReport& r) { r.bcTextureCompression = false; });
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("the format table follows the device's BC, float-filtering and sample-count support", "[metal4]")
{
    Device device = *createDevice();
    MTL::Device* native = nativeOf(device);
    const bool bc = native->supportsBCTextureCompression();
    CHECK(device.formatCapabilities(Format::BC1Unorm).sampled == bc);
    CHECK(device.formatCapabilities(Format::BC1Unorm_sRGB).sampled == bc);
    CHECK(device.formatCapabilities(Format::BC1Unorm).filterLinear == bc);
    CHECK(device.formatCapabilities(Format::R32Float).filterLinear == native->supports32BitFloatFiltering());
    for (uint32_t value = 1; value < core::kFormatCount; ++value)
    {
        const Format format = static_cast<Format>(value);
        CAPTURE(value);
        const FormatCapabilities ceiling = core::formatCeiling(format);
        const FormatCapabilities caps = device.formatCapabilities(format);
        CHECK((caps.sampleCounts & 1u) == 1u);
        for (const uint32_t samples : { 2u, 4u, 8u })
        {
            CAPTURE(samples);
            const uint32_t bit = samples;
            const bool expected = (ceiling.sampleCounts & bit) != 0 && native->supportsTextureSampleCount(samples);
            CHECK(((caps.sampleCounts & bit) != 0) == expected);
        }
    }
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("the format table narrows storage, R32Float resolve and depth filtering as the Apple family tables state", "[metal4]")
{
    Device device = *createDevice();
    MTL::Device* native = nativeOf(device);
    for (const Format format : { Format::BGRA8Unorm, Format::RGB10A2Unorm })
    {
        CAPTURE(static_cast<uint32_t>(format));
        CHECK_FALSE(device.formatCapabilities(format).storageRead);
        CHECK_FALSE(device.formatCapabilities(format).storageWrite);
    }
    CHECK_FALSE(device.formatCapabilities(Format::RG16Float).storageRead);
    CHECK(device.formatCapabilities(Format::RG16Float).storageWrite);
    const FormatCapabilities r32 = device.formatCapabilities(Format::R32Float);
    CHECK(r32.resolve == native->supports32BitFloatFiltering());
    CHECK(r32.blend);
    const bool depthFilter = native->supportsFamily(MTL::GPUFamilyApple9);
    CHECK(device.formatCapabilities(Format::D32Float).filterLinear == depthFilter);
    CHECK(device.formatCapabilities(Format::D32FloatS8).filterLinear == depthFilter);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("the device reports no conservative raster, upload in device memory and mesh tasks on Apple9", "[metal4]")
{
    Device device = *createDevice();
    MTL::Device* native = nativeOf(device);
    const Capabilities& caps = device.capabilities();
    CHECK_FALSE(caps.conservativeRaster);
    CHECK(caps.uploadInDeviceMemory);
    CHECK(caps.meshTasksIndirect == native->supportsFamily(MTL::GPUFamilyApple9));
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("placementSparse follows the device, and a format is sparse when the device tiles its 2D textures at 64 KiB", "[metal4]")
{
    Device device = *createDevice();
    MTL::Device* native = nativeOf(device);
    CHECK(device.capabilities().placementSparse == native->supportsPlacementSparse());
    for (uint32_t value = 1; value < core::kFormatCount; ++value)
    {
        const auto format = static_cast<Format>(value);
        CAPTURE(value);
        const MTL::Size tile = native->sparseTileSize(MTL::TextureType2D, metal4::pixelFormat(format), 1, MTL::SparsePageSize64);
        CHECK(device.formatCapabilities(format).sparse == (native->supportsPlacementSparse() && tile.width != 0));
    }
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("frames past the first framesInFlight wait on the frame event", "[metal4]")
{
    const uint32_t framesInFlight = GENERATE(2u, 3u);
    CAPTURE(framesInFlight);
    Device device = *createDevice({ .framesInFlight = framesInFlight });
    const Counters start = device.counters();
    runFrames(device, framesInFlight);
    const Counters early = device.counters();
    CHECK(early.nativeHostCalls == start.nativeHostCalls);
    CHECK(early.nativeSubmitCalls == start.nativeSubmitCalls + framesInFlight);
    MTL::SharedEvent* frameEvent = DeviceImpl::of(device).frameEvent;
    for (uint64_t frame = framesInFlight + 1; frame <= 2 * framesInFlight; ++frame)
    {
        CAPTURE(frame);
        device.beginFrame();
        // The frame whose slot this one reuses has completed.
        CHECK(frameEvent->signaledValue() >= frame - framesInFlight);
        device.endFrame();
    }
    const Counters late = device.counters();
    CHECK(late.nativeHostCalls == start.nativeHostCalls + framesInFlight);
    CHECK(late.nativeSubmitCalls == start.nativeSubmitCalls + 2 * framesInFlight);
    REQUIRE(frameEvent->waitUntilSignaledValue(2 * framesInFlight, 10'000));
    CHECK(frameEvent->signaledValue() == 2 * framesInFlight);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("two devices pace their frames independently", "[metal4]")
{
    Device first = *createDevice({ .framesInFlight = 2 });
    Device second = *createDevice({ .framesInFlight = 3 });
    CHECK(DeviceImpl::of(first).frameEvent != DeviceImpl::of(second).frameEvent);
    const Counters firstStart = first.counters();
    const Counters secondStart = second.counters();
    second.beginFrame();
    runFrames(first, 6);
    second.endFrame();
    CHECK(first.counters().nativeHostCalls == firstStart.nativeHostCalls + 4);
    CHECK(second.counters().nativeHostCalls == secondStart.nativeHostCalls);
    CHECK(second.counters().nativeSubmitCalls == secondStart.nativeSubmitCalls + 1);
    MTL::SharedEvent* firstEvent = DeviceImpl::of(first).frameEvent;
    MTL::SharedEvent* secondEvent = DeviceImpl::of(second).frameEvent;
    REQUIRE(firstEvent->waitUntilSignaledValue(6, 10'000));
    REQUIRE(secondEvent->waitUntilSignaledValue(1, 10'000));
    CHECK(firstEvent->signaledValue() == 6);
    CHECK(secondEvent->signaledValue() == 1);
    destroyDevice(second);
    runFrames(first, 2);
    CHECK(first.counters().nativeHostCalls == firstStart.nativeHostCalls + 6);
    destroyDevice(first);
}

//======================================================================================================================
TEST_CASE("the view and sampler tables are resident Shared buffers of maxViews and maxSamplers entries", "[metal4]")
{
    Device device = *createDevice();
    DeviceImpl& impl = DeviceImpl::of(device);
    const Capabilities& caps = device.capabilities();
    REQUIRE(impl.viewTable != nullptr);
    REQUIRE(impl.samplerTable != nullptr);
    CHECK(impl.viewTable->length() == uint64_t{ caps.maxViews } * sizeof(MTL::ResourceID));
    CHECK(impl.samplerTable->length() == uint64_t{ caps.maxSamplers } * sizeof(MTL::ResourceID));
    CHECK(impl.viewTable->storageMode() == MTL::StorageModeShared);
    CHECK(impl.samplerTable->storageMode() == MTL::StorageModeShared);
    CHECK(impl.residency.contains(impl.viewTable));
    CHECK(impl.residency.contains(impl.samplerTable));
    destroyDevice(device);
}
