//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12CapabilitiesTests.cpp
/// @brief Tests what a D3D12 device reports: its floor name, adapter, limits, format table, and the format and heap mappings.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/Formats.h"
#include "D3D12Device.h"
#include "D3D12Formats.h"
#include "D3D12Memory.h"
#include "Support/Backend.h"

#include <catch2/catch_test_macros.hpp>

#include <dxgi1_6.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using namespace rojoRHI;

namespace {
    //==================================================================================================================
    /// Returns the format support `device` reports for `format`.
    D3D12_FEATURE_DATA_FORMAT_SUPPORT supportOf(ID3D12Device12* device, DXGI_FORMAT format)
    {
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support = {};
        support.Format = format;
        REQUIRE(SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, static_cast<UINT>(sizeof(support)))));
        return support;
    }
} // namespace

//======================================================================================================================
TEST_CASE("the floor name gives feature level 12_2 and the highest shader model", "[d3d12]")
{
    Result<Device> created = createDevice();
    REQUIRE(created.has_value());
    DeviceImpl& impl = DeviceImpl::of(*created);
    const std::string_view floor = created->capabilities().floorName;
    INFO("floor " << std::string(floor));
    REQUIRE(floor.starts_with("FL 12_2, SM 6."));
    CHECK((impl.features.shaderModel >> 4) == 6u);
    CHECK(static_cast<uint32_t>(floor[std::string_view("FL 12_2, SM 6.").size()] - '0') == (impl.features.shaderModel & 0xFu));
    destroyDevice(*created);
}

//======================================================================================================================
TEST_CASE("the adapter name is the adapter's description", "[d3d12]")
{
    Result<Device> created = createDevice();
    REQUIRE(created.has_value());
    DeviceImpl& impl = DeviceImpl::of(*created);
    DXGI_ADAPTER_DESC1 adapter = {};
    REQUIRE(SUCCEEDED(impl.loaded.adapter->GetDesc1(&adapter)));
    const std::string name(created->capabilities().adapterName);
    CHECK(name == d3d12::utf8(adapter.Description));
#if defined(ROJORHI_D3D12_ADAPTER_WARP)
    CHECK(name.find("Microsoft Basic Render Driver") != std::string::npos);
#endif
    destroyDevice(*created);
}

//======================================================================================================================
TEST_CASE("maxAddrOffset is the raw-view reach", "[d3d12]")
{
    Result<Device> created = createDevice();
    REQUIRE(created.has_value());
    CHECK(created->capabilities().limits.maxAddrOffset == (1ull << 29) - 1);
    destroyDevice(*created);
}

//======================================================================================================================
TEST_CASE("capabilities stay honest until the features exist", "[d3d12]")
{
    Result<Device> created = createDevice();
    REQUIRE(created.has_value());
    const Capabilities& capabilities = created->capabilities();
    CHECK_FALSE(capabilities.placementSparse);
    CHECK_FALSE(capabilities.conservativeRaster);
    CHECK(capabilities.upscalers.empty());
    CHECK(capabilities.meshTasksIndirect);
    destroyDevice(*created);
}

//======================================================================================================================
TEST_CASE("uploadInDeviceMemory follows GPUUploadHeapSupported", "[d3d12]")
{
    Result<Device> created = createDevice();
    REQUIRE(created.has_value());
    D3D12_FEATURE_DATA_D3D12_OPTIONS16 options = {};
    REQUIRE(SUCCEEDED(DeviceImpl::of(*created).device()->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS16, &options, static_cast<UINT>(sizeof(options)))));
    CHECK(created->capabilities().uploadInDeviceMemory == (options.GPUUploadHeapSupported != FALSE));
    destroyDevice(*created);
}

//======================================================================================================================
TEST_CASE("every format the table reports as sampled is sampleable by D3D12", "[d3d12]")
{
    Result<Device> created = createDevice();
    REQUIRE(created.has_value());
    ID3D12Device12* native = DeviceImpl::of(*created).device();
    for (uint32_t value = 1; value < core::kFormatCount; ++value)
    {
        const Format format = static_cast<Format>(value);
        CAPTURE(value);
        const FormatCapabilities caps = created->formatCapabilities(format);
        CHECK_FALSE(caps.sparse);
        if (caps.sampled)
        {
            // A depth format is sampled through its float view.
            const DXGI_FORMAT shader = core::isDepthFormat(format) ? d3d12::viewFormat(format, Format::Undefined, TextureAspect::Depth, TextureUsage::Sampled)
                                                                   : d3d12::dxgiFormat(format);
            const UINT support = static_cast<UINT>(supportOf(native, shader).Support1);
            CHECK((support & (D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE | D3D12_FORMAT_SUPPORT1_SHADER_LOAD)) != 0);
        }
    }
    destroyDevice(*created);
}

//======================================================================================================================
TEST_CASE("the format table never exceeds the core's ceilings", "[d3d12]")
{
    Result<Device> created = createDevice();
    REQUIRE(created.has_value());
    for (uint32_t value = 1; value < core::kFormatCount; ++value)
    {
        const Format format = static_cast<Format>(value);
        CAPTURE(value);
        const FormatCapabilities caps = created->formatCapabilities(format);
        const FormatCapabilities ceiling = core::formatCeiling(format);
        CHECK((!caps.sampled || ceiling.sampled));
        CHECK((!caps.filterLinear || ceiling.filterLinear));
        CHECK((!caps.storageRead || ceiling.storageRead));
        CHECK((!caps.storageWrite || ceiling.storageWrite));
        CHECK((!caps.colorAttachment || ceiling.colorAttachment));
        CHECK((!caps.blend || ceiling.blend));
        CHECK((!caps.depthAttachment || ceiling.depthAttachment));
        CHECK((!caps.resolve || ceiling.resolve));
        CHECK((!caps.sparse || ceiling.sparse));
        CHECK((caps.sampleCounts & ~ceiling.sampleCounts) == 0u);
    }
    destroyDevice(*created);
}

//======================================================================================================================
TEST_CASE("an sRGB format is created castable or typeless", "[d3d12]")
{
    CHECK(d3d12::resourceFormat(Format::RGBA8Unorm_sRGB, TextureUsage::Sampled, false) == DXGI_FORMAT_R8G8B8A8_TYPELESS);
    CHECK(d3d12::resourceFormat(Format::BC1Unorm_sRGB, TextureUsage::Sampled, false) == DXGI_FORMAT_BC1_TYPELESS);
    CHECK(d3d12::resourceFormat(Format::RGBA8Unorm_sRGB, TextureUsage::Sampled, true) == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    const std::vector<DXGI_FORMAT> castable = d3d12::castableFormats(Format::RGBA8Unorm_sRGB);
    REQUIRE(castable.size() == 2);
    CHECK(castable[0] == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    CHECK(castable[1] == DXGI_FORMAT_R8G8B8A8_UNORM);
    CHECK(d3d12::castableFormats(Format::RGBA16Float).empty());
}

//======================================================================================================================
TEST_CASE("a sampled depth texture is created typeless and viewed per aspect", "[d3d12]")
{
    CHECK(d3d12::resourceFormat(Format::D32Float, TextureUsage::Sampled | TextureUsage::DepthStencil, false) == DXGI_FORMAT_R32_TYPELESS);
    CHECK(d3d12::resourceFormat(Format::D32FloatS8, TextureUsage::Sampled | TextureUsage::DepthStencil, true) == DXGI_FORMAT_R32G8X24_TYPELESS);
    CHECK(d3d12::resourceFormat(Format::D32Float, TextureUsage::DepthStencil, false) == DXGI_FORMAT_D32_FLOAT);
    CHECK(d3d12::viewFormat(Format::D32Float, Format::Undefined, TextureAspect::Depth, TextureUsage::Sampled) == DXGI_FORMAT_R32_FLOAT);
    CHECK(d3d12::viewFormat(Format::D32FloatS8, Format::Undefined, TextureAspect::Depth, TextureUsage::Sampled) == DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS);
    CHECK(d3d12::viewFormat(Format::D32FloatS8, Format::Undefined, TextureAspect::Stencil, TextureUsage::Sampled) == DXGI_FORMAT_X32_TYPELESS_G8X24_UINT);
    CHECK(d3d12::dsvFormat(Format::D32Float) == DXGI_FORMAT_D32_FLOAT);
    CHECK(d3d12::dsvFormat(Format::D32FloatS8) == DXGI_FORMAT_D32_FLOAT_S8X24_UINT);
    CHECK(d3d12::viewFormat(Format::RGBA8Unorm, Format::RGBA8Unorm_sRGB, TextureAspect::Color, TextureUsage::Sampled) == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    CHECK(d3d12::viewFormat(Format::RGBA8Unorm_sRGB, Format::Undefined, TextureAspect::Color, TextureUsage::Storage) == DXGI_FORMAT_R8G8B8A8_UNORM);
}

//======================================================================================================================
TEST_CASE("Upload memory maps to the GPU upload heap only when reported", "[d3d12]")
{
    CHECK(d3d12::heapType(Memory::Upload, true) == D3D12_HEAP_TYPE_GPU_UPLOAD);
    CHECK(d3d12::heapType(Memory::Upload, false) == D3D12_HEAP_TYPE_UPLOAD);
    CHECK(d3d12::heapType(Memory::Gpu, true) == D3D12_HEAP_TYPE_DEFAULT);
    CHECK(d3d12::heapType(Memory::Readback, false) == D3D12_HEAP_TYPE_READBACK);
}
