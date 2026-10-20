//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12RootSignature.cpp
/// @brief Creates the D3D12 device's global and kernel root signatures and its four command signatures.
//----------------------------------------------------------------------------------------------------------------------

#include "D3D12RootSignature.h"

#include "D3D12Debug.h"
#include "D3D12Device.h"

#include <cstddef>
#include <expected>
#include <format>
#include <span>
#include <string_view>
#include <utility>

namespace rojoRHI::d3d12 {
    namespace {
        /// The root parameter that holds the draw record's `drawId`.
        constexpr UINT kDrawIdParameter = 1;

        //==============================================================================================================
        /// Serializes `desc` with the device's configuration interface, so the Agility runtime owns the format, and returns
        /// the blob.
        Result<Com<ID3DBlob>> serialize(DeviceImpl& impl, const D3D12_VERSIONED_ROOT_SIGNATURE_DESC& desc, std::string_view what)
        {
            Com<ID3D12DeviceConfiguration> configuration;
            if (HRESULT hr = impl.device()->QueryInterface(IID_PPV_ARGS(configuration.put())); FAILED(hr))
            {
                return std::unexpected(failure(impl, hr, "ID3D12Device::QueryInterface(ID3D12DeviceConfiguration)"));
            }
            Com<ID3DBlob> blob;
            Com<ID3DBlob> errors;
            if (HRESULT hr = configuration->SerializeVersionedRootSignature(&desc, blob.put(), errors.put()); FAILED(hr))
            {
                Error error = failure(impl, hr, std::format("serializing the {} root signature", what));
                if (errors)
                {
                    error.message += std::format(": {}", std::string_view(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()));
                }
                return std::unexpected(std::move(error));
            }
            return blob;
        }

        //==============================================================================================================
        /// Creates the root signature `blob` holds on `impl`'s device and names it `label`.
        Result<Com<ID3D12RootSignature>> createRoot(DeviceImpl& impl, ID3DBlob& blob, std::string_view label)
        {
            Com<ID3D12RootSignature> root;
            if (HRESULT hr = impl.device()->CreateRootSignature(0, blob.GetBufferPointer(), blob.GetBufferSize(), IID_PPV_ARGS(root.put())); FAILED(hr))
            {
                return std::unexpected(failure(impl, hr, std::format("ID3D12Device::CreateRootSignature for {}", label)));
            }
            setName(root.get(), label);
            return root;
        }

        //==============================================================================================================
        /// Creates the command signature of `argumentCount` arguments of `arguments`, `stride` bytes for each record, over
        /// `root` (null when no argument changes the root), and names it `label`.
        Result<Com<ID3D12CommandSignature>> createCommand(
            DeviceImpl& impl, ID3D12RootSignature* root, const D3D12_INDIRECT_ARGUMENT_DESC* arguments, UINT argumentCount, UINT stride, std::string_view label
        )
        {
            D3D12_COMMAND_SIGNATURE_DESC desc = {};
            desc.ByteStride = stride;
            desc.NumArgumentDescs = argumentCount;
            desc.pArgumentDescs = arguments;
            Com<ID3D12CommandSignature> signature;
            if (HRESULT hr = impl.device()->CreateCommandSignature(&desc, root, IID_PPV_ARGS(signature.put())); FAILED(hr))
            {
                return std::unexpected(failure(impl, hr, std::format("ID3D12Device::CreateCommandSignature for {}", label)));
            }
            setName(signature.get(), label);
            return signature;
        }

        //==============================================================================================================
        /// Creates the command signature of a draw-like command: the record's first value goes to the draw-id root constant,
        /// then `command` reads the rest.
        Result<Com<ID3D12CommandSignature>> createDrawCommand(DeviceImpl& impl, D3D12_INDIRECT_ARGUMENT_TYPE command, size_t stride, std::string_view label)
        {
            D3D12_INDIRECT_ARGUMENT_DESC arguments[2] = {};
            arguments[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
            arguments[0].Constant.RootParameterIndex = kDrawIdParameter;
            arguments[0].Constant.DestOffsetIn32BitValues = 0;
            arguments[0].Constant.Num32BitValuesToSet = 1;
            arguments[1].Type = command;
            return createCommand(impl, impl.roots.global(), arguments, 2, static_cast<UINT>(stride), label);
        }
    } // namespace

    //==================================================================================================================
    Result<void> RootSignatures::create(DeviceImpl& impl)
    {
        // The global root signature: the root SRV at t0 and the draw id at b1, both visible to every stage. Both heaps are
        // indexed directly and nothing is bound to the input assembler, because every vertex is pulled.
        D3D12_ROOT_PARAMETER1 global[2] = {};
        global[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        global[0].Descriptor.ShaderRegister = 0;
        global[0].Descriptor.RegisterSpace = 0;
        global[0].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
        global[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        global[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        global[1].Constants.ShaderRegister = 1;
        global[1].Constants.RegisterSpace = 0;
        global[1].Constants.Num32BitValues = 1;
        global[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC globalDesc = {};
        globalDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        globalDesc.Desc_1_1.NumParameters = 2;
        globalDesc.Desc_1_1.pParameters = global;
        globalDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;

        // The kernel root signature: four constants at b0 and the view heap.
        D3D12_ROOT_PARAMETER1 kernel = {};
        kernel.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        kernel.Constants.ShaderRegister = 0;
        kernel.Constants.RegisterSpace = 0;
        kernel.Constants.Num32BitValues = 4;
        kernel.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC kernelDesc = {};
        kernelDesc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        kernelDesc.Desc_1_1.NumParameters = 1;
        kernelDesc.Desc_1_1.pParameters = &kernel;
        kernelDesc.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;

        Result<Com<ID3DBlob>> globalBlob = serialize(impl, globalDesc, "global");
        if (!globalBlob)
        {
            return std::unexpected(std::move(globalBlob.error()));
        }
        const auto* bytes = static_cast<const std::byte*>((*globalBlob)->GetBufferPointer());
        m_serializedGlobal.assign(bytes, bytes + (*globalBlob)->GetBufferSize());
        Result<Com<ID3D12RootSignature>> globalRoot = createRoot(impl, **globalBlob, "rojorhi.root");
        if (!globalRoot)
        {
            return std::unexpected(std::move(globalRoot.error()));
        }
        m_global = std::move(*globalRoot);

        Result<Com<ID3DBlob>> kernelBlob = serialize(impl, kernelDesc, "kernel");
        if (!kernelBlob)
        {
            return std::unexpected(std::move(kernelBlob.error()));
        }
        Result<Com<ID3D12RootSignature>> kernelRoot = createRoot(impl, **kernelBlob, "rojorhi.root.kernel");
        if (!kernelRoot)
        {
            return std::unexpected(std::move(kernelRoot.error()));
        }
        m_kernel = std::move(*kernelRoot);

        // The command signatures. The record of each draw starts with its draw id; a dispatch has no draw id and changes no
        // root argument, so it needs no root signature.
        Result<Com<ID3D12CommandSignature>> draw =
            createDrawCommand(impl, D3D12_INDIRECT_ARGUMENT_TYPE_DRAW, sizeof(DrawIndirectArgs), "rojorhi.signature.draw");
        if (!draw)
        {
            return std::unexpected(std::move(draw.error()));
        }
        m_draw = std::move(*draw);
        Result<Com<ID3D12CommandSignature>> indexed =
            createDrawCommand(impl, D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED, sizeof(DrawIndexedIndirectArgs), "rojorhi.signature.indexed");
        if (!indexed)
        {
            return std::unexpected(std::move(indexed.error()));
        }
        m_drawIndexed = std::move(*indexed);
        Result<Com<ID3D12CommandSignature>> mesh =
            createDrawCommand(impl, D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH, sizeof(DrawMeshTasksIndirectArgs), "rojorhi.signature.mesh");
        if (!mesh)
        {
            return std::unexpected(std::move(mesh.error()));
        }
        m_mesh = std::move(*mesh);
        D3D12_INDIRECT_ARGUMENT_DESC dispatchArgument = {};
        dispatchArgument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
        Result<Com<ID3D12CommandSignature>> dispatch =
            createCommand(impl, nullptr, &dispatchArgument, 1, static_cast<UINT>(sizeof(DispatchIndirectArgs)), "rojorhi.signature.dispatch");
        if (!dispatch)
        {
            return std::unexpected(std::move(dispatch.error()));
        }
        m_dispatch = std::move(*dispatch);
        return {};
    }

    //==================================================================================================================
    ID3D12RootSignature* RootSignatures::global() const
    {
        return m_global.get();
    }

    //==================================================================================================================
    ID3D12RootSignature* RootSignatures::kernel() const
    {
        return m_kernel.get();
    }

    //==================================================================================================================
    ID3D12CommandSignature* RootSignatures::draw() const
    {
        return m_draw.get();
    }

    //==================================================================================================================
    ID3D12CommandSignature* RootSignatures::drawIndexed() const
    {
        return m_drawIndexed.get();
    }

    //==================================================================================================================
    ID3D12CommandSignature* RootSignatures::mesh() const
    {
        return m_mesh.get();
    }

    //==================================================================================================================
    ID3D12CommandSignature* RootSignatures::dispatch() const
    {
        return m_dispatch.get();
    }

    //==================================================================================================================
    std::span<const std::byte> RootSignatures::serializedGlobal() const
    {
        return m_serializedGlobal;
    }
} // namespace rojoRHI::d3d12
