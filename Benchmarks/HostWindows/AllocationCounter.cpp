//----------------------------------------------------------------------------------------------------------------------
/// @file AllocationCounter.cpp
/// @brief Implements the allocation counter by replacing the global operator new and delete in every standard form.
//----------------------------------------------------------------------------------------------------------------------
#include "Host/AllocationCounter.h"

#include <malloc.h>

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <format>
#include <new>

namespace rojoRHI::bench {
    namespace {
        // Every operator new of every thread, counted once per call. Constant-initialized, so an allocation made while
        // static objects are constructed counts too.
        constinit std::atomic<uint64_t> allocations{ 0 };

        //==============================================================================================================
        // A zero-byte request still returns a unique block, as the standard requires of operator new.
        void* allocate(std::size_t size) noexcept
        {
            allocations.fetch_add(1, std::memory_order_relaxed);
            return std::malloc(size == 0 ? 1 : size);
        }

        //==============================================================================================================
        // _aligned_malloc takes any power-of-two alignment, which std::align_val_t always is.
        void* allocateAligned(std::size_t size, std::align_val_t alignment) noexcept
        {
            allocations.fetch_add(1, std::memory_order_relaxed);
            return _aligned_malloc(size == 0 ? 1 : size, static_cast<std::size_t>(alignment));
        }

        //==============================================================================================================
        // The forms that throw report a failed allocation with std::bad_alloc, as the standard requires of them.
        void* allocateOrThrow(std::size_t size)
        {
            void* block = allocate(size);
            if (block == nullptr)
            {
                throw std::bad_alloc();
            }
            return block;
        }

        //==============================================================================================================
        void* allocateAlignedOrThrow(std::size_t size, std::align_val_t alignment)
        {
            void* block = allocateAligned(size, alignment);
            if (block == nullptr)
            {
                throw std::bad_alloc();
            }
            return block;
        }

        //==============================================================================================================
        // The count one call of a form adds, read around a block stored through a volatile pointer so that the compiler
        // cannot elide the pair.
        template<class Allocate, class Free> uint64_t countOf(Allocate allocateBlock, Free freeBlock)
        {
            const uint64_t before = allocations.load(std::memory_order_relaxed);
            auto* volatile block = allocateBlock();
            const uint64_t counted = allocations.load(std::memory_order_relaxed) - before;
            freeBlock(block);
            return counted;
        }

        //==============================================================================================================
        // Each form of operator new the harness and RojoRHI reach must count exactly once: a form the runtime library
        // served itself would count zero and hide allocations.
        std::expected<void, std::string> selfCheck()
        {
            struct alignas(64) Aligned
            {
                std::byte bytes[64];
            };
            const struct
            {
                const char* form;
                uint64_t counted;
            } forms[] = {
                { "operator new", countOf([] { return new int(1); }, [](int* block) { delete block; }) },
                { "operator new[]", countOf([] { return new int[4]; }, [](int* block) { delete[] block; }) },
                { "aligned operator new", countOf([] { return new Aligned(); }, [](Aligned* block) { delete block; }) },
                { "aligned operator new[]", countOf([] { return new Aligned[2]; }, [](Aligned* block) { delete[] block; }) },
                { "nothrow operator new", countOf([] { return new (std::nothrow) int(1); }, [](int* block) { delete block; }) },
                { "nothrow operator new[]", countOf([] { return new (std::nothrow) int[4]; }, [](int* block) { delete[] block; }) },
                { "aligned nothrow operator new", countOf([] { return new (std::nothrow) Aligned(); }, [](Aligned* block) { delete block; }) },
                { "aligned nothrow operator new[]", countOf([] { return new (std::nothrow) Aligned[2]; }, [](Aligned* block) { delete[] block; }) },
            };
            for (const auto& [form, counted] : forms)
            {
                if (counted != 1)
                {
                    return std::unexpected(
                        std::format(
                            "allocation counter: one {} counted {} times, so the executable's replacement does not serve it; allocation counts "
                            "would be wrong",
                            form,
                            counted
                        )
                    );
                }
            }
            return {};
        }
    } // namespace

    //==================================================================================================================
    std::expected<void, std::string> installAllocationCounter()
    {
        static const std::expected<void, std::string> installed = selfCheck();
        return installed;
    }

    //==================================================================================================================
    uint64_t allocationCount()
    {
        return allocations.load(std::memory_order_relaxed);
    }
} // namespace rojoRHI::bench

// The replaceable global allocation functions, every form the standard declares. A delete counts nothing and frees with
// the function matching its new: free for the unaligned forms, _aligned_free for the aligned ones.

//======================================================================================================================
void* operator new(std::size_t size)
{
    return rojoRHI::bench::allocateOrThrow(size);
}

//======================================================================================================================
void* operator new[](std::size_t size)
{
    return rojoRHI::bench::allocateOrThrow(size);
}

//======================================================================================================================
void* operator new(std::size_t size, std::align_val_t alignment)
{
    return rojoRHI::bench::allocateAlignedOrThrow(size, alignment);
}

//======================================================================================================================
void* operator new[](std::size_t size, std::align_val_t alignment)
{
    return rojoRHI::bench::allocateAlignedOrThrow(size, alignment);
}

//======================================================================================================================
void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
    return rojoRHI::bench::allocate(size);
}

//======================================================================================================================
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
    return rojoRHI::bench::allocate(size);
}

//======================================================================================================================
void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
    return rojoRHI::bench::allocateAligned(size, alignment);
}

//======================================================================================================================
void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
    return rojoRHI::bench::allocateAligned(size, alignment);
}

//======================================================================================================================
void operator delete(void* block) noexcept
{
    std::free(block);
}

//======================================================================================================================
void operator delete[](void* block) noexcept
{
    std::free(block);
}

//======================================================================================================================
void operator delete(void* block, std::size_t) noexcept
{
    std::free(block);
}

//======================================================================================================================
void operator delete[](void* block, std::size_t) noexcept
{
    std::free(block);
}

//======================================================================================================================
void operator delete(void* block, std::align_val_t) noexcept
{
    _aligned_free(block);
}

//======================================================================================================================
void operator delete[](void* block, std::align_val_t) noexcept
{
    _aligned_free(block);
}

//======================================================================================================================
void operator delete(void* block, std::size_t, std::align_val_t) noexcept
{
    _aligned_free(block);
}

//======================================================================================================================
void operator delete[](void* block, std::size_t, std::align_val_t) noexcept
{
    _aligned_free(block);
}

//======================================================================================================================
void operator delete(void* block, const std::nothrow_t&) noexcept
{
    std::free(block);
}

//======================================================================================================================
void operator delete[](void* block, const std::nothrow_t&) noexcept
{
    std::free(block);
}

//======================================================================================================================
void operator delete(void* block, std::align_val_t, const std::nothrow_t&) noexcept
{
    _aligned_free(block);
}

//======================================================================================================================
void operator delete[](void* block, std::align_val_t, const std::nothrow_t&) noexcept
{
    _aligned_free(block);
}
