//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Common.h
/// @brief Declares the helpers every D3D12 backend file shares: COM ownership, HRESULT errors, the executable's directory, names and fence waits.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include <windows.h>

// Defines, rather than declares, every GUID the headers below name (each definition is selectany), so a CLSID the Agility
// headers add links without the Windows SDK's dxguid.lib carrying it.
#include <initguid.h>

#include <d3d12.h>

#if defined(_M_X64)
#include <emmintrin.h>
#endif

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

namespace rojoRHI {
    struct DeviceImpl;
}

namespace rojoRHI::d3d12 {
#if defined(_M_X64)
    // One 16-byte move; the ranges do not overlap.
    inline void copy16(std::byte* destination, const std::byte* source)
    {
        _mm_storeu_si128(reinterpret_cast<__m128i*>(destination), _mm_loadu_si128(reinterpret_cast<const __m128i*>(source)));
    }
#endif

    // A block of 16 to 256 bytes, as a root block or other frame data is, moves inline in 16-byte steps, four to a loop
    // trip, the last step ending at the block's end: `memcpy` of a size the compiler cannot see is a call into the C
    // runtime, which costs more than such a copy. The ranges do not overlap. A size that is a multiple of 16 writes each
    // byte once, which matters in write-combined upload memory, where a second write to a flushed line is a partial one.
    inline void copySmall(std::byte* destination, const std::byte* source, size_t size)
    {
#if defined(_M_X64)
        if (size >= 16 && size <= 256)
        {
            size_t offset = 0;
            // The trips stop short of the last 16 bytes, which the final step writes: a 256-byte root block takes three
            // trips, three single steps and the final one.
            for (; offset + 64 < size; offset += 64)
            {
                copy16(destination + offset, source + offset);
                copy16(destination + offset + 16, source + offset + 16);
                copy16(destination + offset + 32, source + offset + 32);
                copy16(destination + offset + 48, source + offset + 48);
            }
            for (; offset + 16 < size; offset += 16)
            {
                copy16(destination + offset, source + offset);
            }
            // Of a size that is no multiple of 16, the last step covers bytes the step before it wrote, with the same values.
            copy16(destination + size - 16, source + size - 16);
            return;
        }
#endif
        std::memcpy(destination, source, size);
    }

    // Holds one reference, released on reset, reassignment or destruction; a copy takes a reference of its own.
    template<class T> class Com
    {
      public:
        Com() = default;

        // Takes over the one reference `owned` carries.
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

        T* get() const
        {
            return m_pointer;
        }

        T* operator->() const
        {
            return m_pointer;
        }

        T& operator*() const
        {
            return *m_pointer;
        }

        explicit operator bool() const
        {
            return m_pointer != nullptr;
        }

        // Releases the interface held first.
        T** put()
        {
            reset();
            return &m_pointer;
        }

        void reset()
        {
            if (T* held = std::exchange(m_pointer, nullptr); held != nullptr)
            {
                held->Release();
            }
        }

        // The caller now owns the reference.
        T* detach()
        {
            return std::exchange(m_pointer, nullptr);
        }

      private:
        T* m_pointer = nullptr;
    };

    uint32_t hresultBits(HRESULT result);

    std::string utf8(std::wstring_view text);

    // Without a trailing separator; empty when the path cannot be read. The Agility and FidelityFX runtimes are both
    // found beside the executable.
    std::wstring executableDirectory();

    // `OutOfMemory` for E_OUTOFMEMORY, `DriverFailure` for any other failure.
    Error hresultError(HRESULT result, std::string_view what);

    // Once `out`'s capacity holds as many code units as `label` has bytes, it allocates nothing. Logs and aborts, in
    // every build, when a non-empty label converts to nothing.
    void widen(std::string_view label, std::wstring& out);

    // So the debug layer, DRED and captures show it. Logs and aborts, in every build, when the runtime refuses the
    // name.
    void setName(ID3D12Object* object, std::string_view label);

    struct FenceWait
    {
        bool reached = false;
        uint64_t completed = 0; // UINT64_MAX on a removed device.
        uint32_t hostCalls = 0; // GetCompletedValue and SetEventOnCompletion calls, one `nativeHostCalls` each; the caller counts them.
    };

    // Returns at once when the value is reached or `timeoutNs` is 0. Otherwise it arms the thread's wait event and
    // waits in slices of at most ten seconds, rereading the completed value after every wake and once more when the
    // timeout runs out; `UINT64_MAX` waits without end, warning after each full slice. A timed-out wait leaves the
    // event armed, since D3D12 cannot disarm one: a later wake from it finds the value unreached and waits on, and the
    // thread's exit leaves the event open, so no handle that reuses its value is signalled. On a removed device the
    // wait ends at once with UINT64_MAX, so a caller checks `completed` before it passes a value to
    // `core::completeFence`. Logs and aborts, in every build, when no event can be made or armed or the wait fails.
    FenceWait waitForFence(ID3D12Fence* fence, uint64_t value, uint64_t timeoutNs, std::string_view what);

    // The failure path of `requireSucceeded`, out of line.
    [[noreturn]] void failRequired(DeviceImpl& impl, HRESULT result, std::string_view what);

    // For an entry that returns no Result: on failure, reports the removal when the device was removed, then logs the
    // call and aborts, in every build. Inline, so a success, as each list's `Reset` and `Close` are, costs one test.
    inline void requireSucceeded(DeviceImpl& impl, HRESULT result, std::string_view what)
    {
        if (FAILED(result)) [[unlikely]]
        {
            failRequired(impl, result, what);
        }
    }
} // namespace rojoRHI::d3d12
