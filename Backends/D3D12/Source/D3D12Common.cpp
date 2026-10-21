//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Common.cpp
/// @brief Implements the helpers every D3D12 backend file shares: HRESULT errors, names, fence waits and the stub entries.
//----------------------------------------------------------------------------------------------------------------------

#include "D3D12Common.h"

#include "Base/Assert.h"
#include "Base/Log.h"
#include "D3D12Debug.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <string>

namespace rojoRHI::d3d12 {
    namespace {
        /// The longest single wait on a fence event, in milliseconds; a longer wait runs in slices of this length.
        constexpr uint64_t kWaitSliceMs = 10'000;

        /// The calling thread's fence-wait event: auto-reset, made on the thread's first wait and closed at its exit unless a
        /// wait timed out, which leaves the event armed for good.
        class WaitEvent
        {
          public:
            //==========================================================================================================
            WaitEvent() = default;

            //==========================================================================================================
            WaitEvent(const WaitEvent&) = delete;

            //==========================================================================================================
            WaitEvent& operator=(const WaitEvent&) = delete;

            //==========================================================================================================
            ~WaitEvent()
            {
                // An armed event stays open: closing it would let the fence signal whatever handle reuses its value.
                if (m_event != nullptr && !m_armed)
                {
                    CloseHandle(m_event);
                }
            }

            //==========================================================================================================
            /// Returns the event, made on the first call; logs and aborts, in every build, when none can be made for `what`.
            HANDLE get(std::string_view what)
            {
                if (m_event == nullptr)
                {
                    m_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                    if (m_event == nullptr)
                    {
                        ROJORHI_LOG_FATAL("CreateEventW for a wait on {} failed with error {}", what, GetLastError());
                        std::abort();
                    }
                }
                return m_event;
            }

            //==========================================================================================================
            /// Records that a wait timed out, so an arming of the event may never be signalled before the thread exits.
            void leaveArmed()
            {
                m_armed = true;
            }

          private:
            HANDLE m_event = nullptr;
            bool m_armed = false;
        };
    } // namespace

    //==================================================================================================================
    uint32_t hresultBits(HRESULT result)
    {
        return static_cast<uint32_t>(result);
    }

    //==================================================================================================================
    std::string utf8(std::wstring_view text)
    {
        if (text.empty())
        {
            return {};
        }
        const int wide = static_cast<int>(text.size());
        const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), wide, nullptr, 0, nullptr, nullptr);
        std::string result(static_cast<size_t>(size > 0 ? size : 0), '\0');
        WideCharToMultiByte(CP_UTF8, 0, text.data(), wide, result.data(), size, nullptr, nullptr);
        return result;
    }

    //==================================================================================================================
    Error hresultError(HRESULT result, std::string_view what)
    {
        const ErrorCategory category = result == E_OUTOFMEMORY ? ErrorCategory::OutOfMemory : ErrorCategory::DriverFailure;
        return Error{ category, std::format("{} failed with HRESULT 0x{:08X}", what, hresultBits(result)) };
    }

    //==================================================================================================================
    void widen(std::string_view label, std::wstring& out)
    {
        // A UTF-8 sequence never takes fewer bytes than the UTF-16 code units it decodes to, so one call fills the string.
        out.resize(label.size());
        if (label.empty())
        {
            return;
        }
        const int written = MultiByteToWideChar(CP_UTF8, 0, label.data(), static_cast<int>(label.size()), out.data(), static_cast<int>(out.size()));
        if (written <= 0)
        {
            ROJORHI_LOG_FATAL("MultiByteToWideChar for \"{}\" failed with error {}", label, GetLastError());
            std::abort();
        }
        out.resize(static_cast<size_t>(written));
    }

    //==================================================================================================================
    void setName(ID3D12Object* object, std::string_view label)
    {
        ROJORHI_INTERNAL_ASSERT(object != nullptr, "setName was given no object");
        ROJORHI_INTERNAL_ASSERT(!label.empty(), "setName was given an empty label; the core defaults every label");
        std::wstring name;
        widen(label, name);
        if (const HRESULT named = object->SetName(name.c_str()); FAILED(named))
        {
            ROJORHI_LOG_FATAL("ID3D12Object::SetName for \"{}\" failed with HRESULT 0x{:08X}", label, hresultBits(named));
            std::abort();
        }
    }

    //==================================================================================================================
    FenceWait waitForFence(ID3D12Fence* fence, uint64_t value, uint64_t timeoutNs, std::string_view what)
    {
        // A removed device reports UINT64_MAX, which passes every value, so no wait outlives the device.
        FenceWait wait{ false, fence->GetCompletedValue(), 1 };
        wait.reached = wait.completed >= value;
        if (wait.reached || timeoutNs == 0)
        {
            return wait;
        }
        thread_local WaitEvent waitEvent;
        const HANDLE event = waitEvent.get(what);
        const HRESULT armed = fence->SetEventOnCompletion(value, event);
        ++wait.hostCalls;
        if (FAILED(armed))
        {
            ROJORHI_LOG_FATAL("ID3D12Fence::SetEventOnCompletion for {} at {} failed with HRESULT 0x{:08X}", what, value, hresultBits(armed));
            std::abort();
        }
        const bool endless = timeoutNs == UINT64_MAX;
        uint64_t remainingMs = timeoutNs / 1'000'000 + (timeoutNs % 1'000'000 != 0 ? 1 : 0);
        for (;;)
        {
            const uint64_t slice = endless ? kWaitSliceMs : std::min(remainingMs, kWaitSliceMs);
            const auto start = std::chrono::steady_clock::now();
            const DWORD waited = WaitForSingleObject(event, static_cast<DWORD>(slice));
            if (waited == WAIT_OBJECT_0)
            {
                // The wake may be an earlier timed-out wait's arming, so only the fence says whether this value was reached.
                wait.completed = fence->GetCompletedValue();
                ++wait.hostCalls;
                if (wait.completed >= value)
                {
                    wait.reached = true;
                    return wait;
                }
                if (!endless)
                {
                    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
                    remainingMs -= std::min(remainingMs, static_cast<uint64_t>(elapsed.count()));
                }
            }
            else if (waited != WAIT_TIMEOUT)
            {
                ROJORHI_LOG_FATAL("WaitForSingleObject for {} at {} returned {} with error {}", what, value, waited, GetLastError());
                std::abort();
            }
            else if (endless)
            {
                ROJORHI_LOG_WARN("still waiting for {} to reach {} after another {} ms", what, value, kWaitSliceMs);
            }
            else
            {
                remainingMs -= slice;
            }
            if (!endless && remainingMs == 0)
            {
                // The value may have been reached as the last slice ran out, so the fence decides the result. The event stays
                // marked armed either way: the runtime may still be about to signal it.
                wait.completed = fence->GetCompletedValue();
                ++wait.hostCalls;
                wait.reached = wait.completed >= value;
                waitEvent.leaveArmed();
                return wait;
            }
        }
    }

    //==================================================================================================================
    void requireSucceeded(DeviceImpl& impl, HRESULT result, std::string_view what)
    {
        if (SUCCEEDED(result))
        {
            return;
        }
        if (deviceRemoved(result))
        {
            reportRemoval(impl, what);
        }
        ROJORHI_LOG_FATAL("{} failed with HRESULT 0x{:08X}", what, hresultBits(result));
        std::abort();
    }
} // namespace rojoRHI::d3d12
