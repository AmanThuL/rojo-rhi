//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Common.cpp
/// @brief Implements the helpers every D3D12 backend file shares: HRESULT errors, names, fence waits and the stub entries.
//----------------------------------------------------------------------------------------------------------------------

#include "D3D12Common.h"

#include "Base/Assert.h"
#include "Base/Log.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <string>

namespace rojoRHI::d3d12 {
    namespace {
        /// The longest single wait on a fence event, in milliseconds; a longer wait runs in slices of this length.
        constexpr uint64_t kWaitSliceMs = 10'000;

        //==============================================================================================================
        /// Returns the 32 bits of `result`, which a message prints as eight hexadecimal digits.
        uint32_t hresultBits(HRESULT result)
        {
            return static_cast<uint32_t>(result);
        }
    } // namespace

    //==================================================================================================================
    void notImplemented(std::string_view entry)
    {
        ROJORHI_LOG_ERROR("{} is not implemented by the D3D12 backend", entry);
        std::abort();
    }

    //==================================================================================================================
    std::unexpected<Error> unsupported(std::string_view entry)
    {
        return std::unexpected(Error{ ErrorCategory::Unsupported, std::format("{} is not supported by the D3D12 backend", entry) });
    }

    //==================================================================================================================
    Error hresultError(HRESULT result, std::string_view what)
    {
        const ErrorCategory category = result == E_OUTOFMEMORY ? ErrorCategory::OutOfMemory : ErrorCategory::DriverFailure;
        return Error{ category, std::format("{} failed with HRESULT 0x{:08X}", what, hresultBits(result)) };
    }

    //==================================================================================================================
    void setName(ID3D12Object* object, std::string_view label)
    {
        ROJORHI_INTERNAL_ASSERT(object != nullptr, "setName was given no object");
        ROJORHI_INTERNAL_ASSERT(!label.empty(), "setName was given an empty label; the core defaults every label");
        // A UTF-8 sequence never takes fewer bytes than the UTF-16 code units it decodes to, so one call fills the string.
        std::wstring name(label.size(), L'\0');
        const int written = MultiByteToWideChar(CP_UTF8, 0, label.data(), static_cast<int>(label.size()), name.data(), static_cast<int>(name.size()));
        name.resize(static_cast<size_t>(written > 0 ? written : 0));
        if (const HRESULT named = object->SetName(name.c_str()); FAILED(named))
        {
            ROJORHI_LOG_ERROR("ID3D12Object::SetName for \"{}\" failed with HRESULT 0x{:08X}", label, hresultBits(named));
            std::abort();
        }
    }

    //==================================================================================================================
    bool waitForFence(ID3D12Fence* fence, uint64_t value, uint64_t timeoutNs, std::string_view what)
    {
        // A removed device reports UINT64_MAX, which passes every value, so no wait outlives the device.
        if (fence->GetCompletedValue() >= value)
        {
            return true;
        }
        const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (event == nullptr)
        {
            ROJORHI_LOG_ERROR("CreateEventW for a wait on {} failed with error {}", what, GetLastError());
            std::abort();
        }
        if (const HRESULT armed = fence->SetEventOnCompletion(value, event); FAILED(armed))
        {
            CloseHandle(event);
            ROJORHI_LOG_ERROR("ID3D12Fence::SetEventOnCompletion for {} at {} failed with HRESULT 0x{:08X}", what, value, hresultBits(armed));
            std::abort();
        }
        const bool endless = timeoutNs == UINT64_MAX;
        uint64_t remainingMs = timeoutNs / 1'000'000 + (timeoutNs % 1'000'000 != 0 ? 1 : 0);
        bool reached = false;
        for (;;)
        {
            const uint64_t slice = endless ? kWaitSliceMs : std::min(remainingMs, kWaitSliceMs);
            const DWORD waited = WaitForSingleObject(event, static_cast<DWORD>(slice));
            if (waited == WAIT_OBJECT_0)
            {
                reached = true;
                break;
            }
            if (waited != WAIT_TIMEOUT)
            {
                CloseHandle(event);
                ROJORHI_LOG_ERROR("WaitForSingleObject for {} at {} returned {} with error {}", what, value, waited, GetLastError());
                std::abort();
            }
            if (endless)
            {
                ROJORHI_LOG_WARN("still waiting for {} to reach {} after another {} ms", what, value, kWaitSliceMs);
                continue;
            }
            remainingMs -= slice;
            if (remainingMs == 0)
            {
                break;
            }
        }
        CloseHandle(event);
        return reached;
    }
} // namespace rojoRHI::d3d12
