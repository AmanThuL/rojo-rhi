//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Common.h
/// @brief Declares the helpers every D3D12 backend file shares: COM ownership, HRESULT errors, names, fence waits and stubs.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include <windows.h>

// Defines, rather than declares, every GUID the headers below name (each definition is selectany), so a CLSID the Agility
// headers add links without the Windows SDK's dxguid.lib carrying it.
#include <initguid.h>

#include <d3d12.h>

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

namespace rojoRHI::d3d12 {
    /// An owning pointer to a COM interface: it holds one reference, which it releases when it is reset, reassigned or
    /// destroyed. A copy takes a reference of its own.
    template<class T> class Com
    {
      public:
        Com() = default;

        /// Takes over the one reference `owned` carries.
        explicit Com(T* owned) : m_pointer(owned)
        {
        }

        Com(const Com& other) : m_pointer(other.m_pointer)
        {
            if (m_pointer != nullptr)
            {
                m_pointer->AddRef();
            }
        }

        Com(Com&& other) noexcept : m_pointer(std::exchange(other.m_pointer, nullptr))
        {
        }

        Com& operator=(Com other) noexcept
        {
            std::swap(m_pointer, other.m_pointer);
            return *this;
        }

        ~Com()
        {
            reset();
        }

        /// Returns the interface, still owned.
        T* get() const
        {
            return m_pointer;
        }

        T* operator->() const
        {
            return m_pointer;
        }

        explicit operator bool() const
        {
            return m_pointer != nullptr;
        }

        /// Releases the interface held, if any, and returns the address a creating call writes its new reference to.
        T** put()
        {
            reset();
            return &m_pointer;
        }

        /// Releases the interface held, if any.
        void reset()
        {
            if (T* held = std::exchange(m_pointer, nullptr); held != nullptr)
            {
                held->Release();
            }
        }

        /// Gives up the reference held without releasing it and returns the interface; the caller now owns that reference.
        T* detach()
        {
            return std::exchange(m_pointer, nullptr);
        }

      private:
        T* m_pointer = nullptr;
    };

    /// Logs that the D3D12 backend does not implement `entry`, named `Class::member`, then aborts, in every build.
    [[noreturn]] void notImplemented(std::string_view entry);

    /// Returns the `Unsupported` error an entry that returns a `Result` gives, naming `entry` as `Class::member`.
    std::unexpected<Error> unsupported(std::string_view entry);

    /// Returns the 32 bits of `result`, which a message prints as eight hexadecimal digits.
    uint32_t hresultBits(HRESULT result);

    /// Returns `text` converted from UTF-16 to UTF-8: for messages, and for the path the Agility device factory reads.
    std::string utf8(std::wstring_view text);

    /// Returns the error of a failed native call: `OutOfMemory` for `E_OUTOFMEMORY`, `DriverFailure` for any other failure,
    /// with the message "<what> failed with HRESULT 0x<8 hex digits>".
    Error hresultError(HRESULT result, std::string_view what);

    /// Names `object` with `label`, converted from UTF-8 to UTF-16 once, so the debug layer, DRED and captures show it. The
    /// label is never empty: the core defaults every label. Logs and aborts, in every build, when the runtime refuses the
    /// name.
    void setName(ID3D12Object* object, std::string_view label);

    /// Blocks until `fence` reaches `value` or `timeoutNs` nanoseconds pass, and returns whether it reached the value. A
    /// completed value already at `value` returns at once; otherwise one event, closed before the return, waits in slices of
    /// at most ten seconds, the timeout rounded up to whole milliseconds. With `UINT64_MAX` it waits without end and logs a
    /// warning naming `what` and the value after each full slice. On a removed device `GetCompletedValue` returns `UINT64_MAX`,
    /// so the wait reports completion at once: a caller checks for removal before it passes a value to `core::completeFence`.
    /// Logs and aborts, in every build, when no event can be made or armed.
    bool waitForFence(ID3D12Fence* fence, uint64_t value, uint64_t timeoutNs, std::string_view what);
} // namespace rojoRHI::d3d12
