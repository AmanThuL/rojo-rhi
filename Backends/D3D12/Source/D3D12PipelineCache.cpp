//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12PipelineCache.cpp
/// @brief Implements the D3D12 pipeline cache: the pipeline library, its look-ups and stores, and the blob it serializes.
//----------------------------------------------------------------------------------------------------------------------

#include "D3D12PipelineCache.h"

#include <rojoRHI/RHI.h>

#include "Base/Log.h"
#include "D3D12Common.h"
#include "D3D12Debug.h"
#include "D3D12Device.h"
#include "D3D12PipelineKey.h"
#include "D3D12PipelineState.h"

#include <dxgi1_6.h>

#include <cstddef>
#include <expected>
#include <format>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rojoRHI::d3d12 {
    namespace {
        //==============================================================================================================
        /// Logs the warning of a blob that is discarded because `reason`.
        void discard(std::string_view reason)
        {
            ROJORHI_LOG_WARN("the pipeline cache blob {} and is discarded; every pipeline is compiled", reason);
        }

        //==============================================================================================================
        /// Names the library of `impl`'s device after the device.
        void nameLibrary(DeviceImpl& impl, ID3D12PipelineLibrary1* library)
        {
            setName(library, std::format("{}.pipelinecache", impl.core.label()));
        }
    } // namespace

    //==================================================================================================================
    void PipelineCache::open(DeviceImpl& impl, std::span<const std::byte> blob)
    {
        ID3D12Device12* device = impl.device();
        if (!blob.empty())
        {
            const std::expected<std::span<const std::byte>, std::string_view> library = libraryOf(blob);
            if (!library)
            {
                discard(library.error());
            }
            else if (!library->empty())
            {
                // The library reads the bytes in place for its whole life, and the caller's blob lives only until
                // createDevice returns.
                m_bytes.assign(library->begin(), library->end());
                const HRESULT created = device->CreatePipelineLibrary(m_bytes.data(), m_bytes.size(), IID_PPV_ARGS(m_library.put()));
                if (SUCCEEDED(created))
                {
                    nameLibrary(impl, m_library.get());
                    return;
                }
                m_bytes = {};
                if (created == DXGI_ERROR_UNSUPPORTED)
                {
                    ROJORHI_LOG_WARN("the D3D12 runtime supports no pipeline library; the pipeline cache blob is discarded and every pipeline is compiled");
                    return;
                }
                // D3D12_ERROR_DRIVER_VERSION_MISMATCH, D3D12_ERROR_ADAPTER_NOT_FOUND or E_INVALIDARG: another driver, another
                // adapter, or bytes that are no library.
                discard(std::format("holds a pipeline library that CreatePipelineLibrary refused with HRESULT 0x{:08x}", hresultBits(created)));
            }
        }
        if (const HRESULT created = device->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(m_library.put())); FAILED(created))
        {
            m_library.reset();
            ROJORHI_LOG_WARN(
                "ID3D12Device1::CreatePipelineLibrary refused an empty pipeline library with HRESULT 0x{:08x}; every pipeline is compiled and none is cached",
                hresultBits(created)
            );
            return;
        }
        nameLibrary(impl, m_library.get());
    }

    //==================================================================================================================
    Result<ID3D12PipelineState*> PipelineCache::create(
        DeviceImpl& impl, const PipelineStream& stream, const std::wstring& name, std::string_view label, bool& hit
    )
    {
        hit = false;
        const D3D12_PIPELINE_STATE_STREAM_DESC desc = stream.desc();
        Com<ID3D12PipelineState> pso;
        // Without a library every creation is a miss, as a name the library does not hold is.
        HRESULT loaded = E_INVALIDARG;
        if (m_library)
        {
            const std::lock_guard lock(m_mutex);
            loaded = m_library->LoadPipeline(name.c_str(), &desc, IID_PPV_ARGS(pso.put()));
        }
        if (SUCCEEDED(loaded))
        {
            hit = true;
            return pso.detach();
        }
        // E_INVALIDARG is a miss: no pipeline is stored under the name, or the one stored has another description.
        if (loaded != E_INVALIDARG)
        {
            return std::unexpected(failure(impl, loaded, std::format("ID3D12PipelineLibrary1::LoadPipeline for pipeline \"{}\"", label)));
        }
        if (const HRESULT created = impl.device()->CreatePipelineState(&desc, IID_PPV_ARGS(pso.put())); FAILED(created))
        {
            return std::unexpected(failure(impl, created, std::format("ID3D12Device2::CreatePipelineState for pipeline \"{}\"", label)));
        }
        HRESULT stored = S_OK;
        if (m_library)
        {
            const std::lock_guard lock(m_mutex);
            stored = m_library->StorePipeline(name.c_str(), pso.get());
        }
        // E_INVALIDARG means another creation stored the name first, or a description whose key is the same holds it; the
        // pipeline is still created, only not stored.
        if (FAILED(stored) && stored != E_INVALIDARG)
        {
            return std::unexpected(failure(impl, stored, std::format("ID3D12PipelineLibrary::StorePipeline for pipeline \"{}\"", label)));
        }
        return pso.detach();
    }

    //==================================================================================================================
    Result<std::vector<std::byte>> PipelineCache::data(DeviceImpl& impl)
    {
        std::vector<std::byte> blob;
        HRESULT serialized = S_OK;
        {
            const std::lock_guard lock(m_mutex);
            const size_t size = m_library ? m_library->GetSerializedSize() : 0;
            const std::string header = blobHeader(size);
            const auto* text = reinterpret_cast<const std::byte*>(header.data());
            blob.assign(text, text + header.size());
            blob.resize(header.size() + size);
            if (size != 0)
            {
                serialized = m_library->Serialize(blob.data() + header.size(), size);
            }
        }
        if (FAILED(serialized))
        {
            return std::unexpected(failure(impl, serialized, "ID3D12PipelineLibrary::Serialize"));
        }
        return blob;
    }

    //==================================================================================================================
    void PipelineCache::release()
    {
        const std::lock_guard lock(m_mutex);
        m_library.reset();
        m_bytes = {};
    }
} // namespace rojoRHI::d3d12
