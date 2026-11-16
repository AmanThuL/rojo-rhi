//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12RootSignatureTests.cpp
/// @brief Tests the D3D12 device's global root signature, its labelled signatures and the strides of its indirect records.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "D3D12Common.h"
#include "D3D12Device.h"
#include "D3D12RootSignature.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <span>
#include <string>

using namespace rojoRHI;

namespace {
    //==================================================================================================================
    /// Returns the debug name of `object` as UTF-8, empty when it has none.
    std::string nameOf(ID3D12Object* object)
    {
        wchar_t name[64] = {};
        UINT size = static_cast<UINT>(sizeof(name));
        if (FAILED(object->GetPrivateData(WKPDID_D3DDebugObjectNameW, &size, name)))
        {
            return {};
        }
        // The size counts bytes; a terminating null, when stored, is dropped.
        std::wstring read(name, size / sizeof(wchar_t));
        while (!read.empty() && read.back() == L'\0')
        {
            read.pop_back();
        }
        return d3d12::utf8(read);
    }

    //==================================================================================================================
    /// Returns the cost in 32-bit values of `parameter` in a root signature: two for a root descriptor, one for each root
    /// constant, one for a descriptor table.
    UINT rootCost(const D3D12_ROOT_PARAMETER1& parameter)
    {
        switch (parameter.ParameterType)
        {
        case D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS: return parameter.Constants.Num32BitValues;
        case D3D12_ROOT_PARAMETER_TYPE_CBV:
        case D3D12_ROOT_PARAMETER_TYPE_SRV:
        case D3D12_ROOT_PARAMETER_TYPE_UAV: return 2;
        case D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE: return 1;
        }
        return 0;
    }

    //==================================================================================================================
    /// Deserializes the global root signature of `impl` at version 1.1 into `deserializer` and returns its description.
    const D3D12_ROOT_SIGNATURE_DESC1& globalDesc(DeviceImpl& impl, d3d12::Com<ID3D12VersionedRootSignatureDeserializer>& deserializer)
    {
        d3d12::Com<ID3D12DeviceConfiguration> configuration;
        REQUIRE(SUCCEEDED(impl.device()->QueryInterface(IID_PPV_ARGS(configuration.put()))));
        const std::span<const std::byte> blob = impl.roots.serializedGlobal();
        REQUIRE(!blob.empty());
        REQUIRE(SUCCEEDED(configuration->CreateVersionedRootSignatureDeserializer(blob.data(), blob.size(), IID_PPV_ARGS(deserializer.put()))));
        const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* desc = nullptr;
        REQUIRE(SUCCEEDED(deserializer->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_1, &desc)));
        REQUIRE(desc != nullptr);
        REQUIRE(desc->Version == D3D_ROOT_SIGNATURE_VERSION_1_1);
        return desc->Desc_1_1;
    }
} // namespace

//======================================================================================================================
TEST_CASE("the global root signature holds the root SRV at t0 and one root constant at b1", "[d3d12]")
{
    Result<Device> created = createDevice();
    REQUIRE(created.has_value());
    d3d12::Com<ID3D12VersionedRootSignatureDeserializer> deserializer;
    const D3D12_ROOT_SIGNATURE_DESC1& desc = globalDesc(DeviceImpl::of(*created), deserializer);
    CHECK((desc.Flags & D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED) != 0);
    CHECK((desc.Flags & D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED) != 0);
    CHECK((desc.Flags & D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT) == 0);
    CHECK(desc.NumStaticSamplers == 0);
    REQUIRE(desc.NumParameters == 2);
    const D3D12_ROOT_PARAMETER1& srv = desc.pParameters[0];
    CHECK(srv.ParameterType == D3D12_ROOT_PARAMETER_TYPE_SRV);
    CHECK(srv.Descriptor.ShaderRegister == 0);
    CHECK(srv.Descriptor.RegisterSpace == 0);
    CHECK((srv.Descriptor.Flags & D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE) != 0);
    const D3D12_ROOT_PARAMETER1& constant = desc.pParameters[1];
    CHECK(constant.ParameterType == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS);
    CHECK(constant.Constants.ShaderRegister == 1);
    CHECK(constant.Constants.RegisterSpace == 0);
    CHECK(constant.Constants.Num32BitValues == 1);
    deserializer.reset();
    destroyDevice(*created);
}

//======================================================================================================================
TEST_CASE("the root signatures and command signatures exist and carry their labels", "[d3d12]")
{
    Result<Device> created = createDevice();
    REQUIRE(created.has_value());
    const d3d12::RootSignatures& roots = DeviceImpl::of(*created).roots;
    REQUIRE(roots.global() != nullptr);
    REQUIRE(roots.kernel() != nullptr);
    REQUIRE(roots.draw() != nullptr);
    REQUIRE(roots.drawIndexed() != nullptr);
    REQUIRE(roots.mesh() != nullptr);
    REQUIRE(roots.dispatch() != nullptr);
    CHECK(nameOf(roots.global()) == "rojorhi.root");
    CHECK(nameOf(roots.kernel()) == "rojorhi.root.kernel");
    CHECK(nameOf(roots.draw()) == "rojorhi.signature.draw");
    CHECK(nameOf(roots.drawIndexed()) == "rojorhi.signature.indexed");
    CHECK(nameOf(roots.mesh()) == "rojorhi.signature.mesh");
    CHECK(nameOf(roots.dispatch()) == "rojorhi.signature.dispatch");
    destroyDevice(*created);
}

//======================================================================================================================
TEST_CASE("the root parameters fit the root budget", "[d3d12]")
{
    Result<Device> created = createDevice();
    REQUIRE(created.has_value());
    d3d12::Com<ID3D12VersionedRootSignatureDeserializer> deserializer;
    const D3D12_ROOT_SIGNATURE_DESC1& desc = globalDesc(DeviceImpl::of(*created), deserializer);
    UINT cost = 0;
    for (UINT i = 0; i < desc.NumParameters; ++i)
    {
        cost += rootCost(desc.pParameters[i]);
    }
    CHECK(cost == 3);
    CHECK(cost < 64);
    deserializer.reset();
    destroyDevice(*created);
}

//======================================================================================================================
TEST_CASE("the command signatures' strides match the indirect records", "[d3d12]")
{
    CHECK(sizeof(DrawIndirectArgs) == 20U);
    CHECK(sizeof(DrawIndexedIndirectArgs) == 24U);
    CHECK(sizeof(DrawMeshTasksIndirectArgs) == 16U);
    CHECK(sizeof(DispatchIndirectArgs) == 12U);
}
