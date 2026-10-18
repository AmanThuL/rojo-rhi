//----------------------------------------------------------------------------------------------------------------------
/// @file AllocationCounter.cpp
/// @brief Implements the allocation counter by wrapping the function table of malloc zone 0.
//----------------------------------------------------------------------------------------------------------------------
#include "Host/AllocationCounter.h"

#include <mach/mach.h>
#include <malloc/malloc.h>
#include <pthread.h>

#include <atomic>
#include <cstdlib>
#include <format>

// libmalloc exports its zone list without declaring it. Zone 0 is the zone malloc, calloc, realloc, posix_memalign and
// operator new dispatch to; malloc_default_zone() returns a forwarding zone whose own table those calls bypass.
extern "C" malloc_zone_t** malloc_zones;
extern "C" unsigned malloc_num_zones;

namespace rojoRHI::bench {
    namespace {
        // The zone versions that add a table entry, from <malloc/malloc.h>.
        constexpr unsigned kMemalignVersion = 5;
        constexpr unsigned kOptionsVersion = 15;
        constexpr unsigned kTypedVersion = 16;

        std::atomic<uint64_t> allocations{ 0 };
        // Set on a thread while it runs batch_malloc, which counts the blocks it returns: zone 0 allocates each block
        // through its own table, and those entries must not count the block again. A thread-specific slot is read
        // without allocating, unlike a thread_local, whose first use on a thread allocates.
        pthread_key_t batchKey{};

        // The zone's own entries, called by the forwarders. Each is written once, before its forwarder is installed.
        struct ZoneEntries
        {
            decltype(malloc_zone_t::malloc) malloc = nullptr;
            decltype(malloc_zone_t::calloc) calloc = nullptr;
            decltype(malloc_zone_t::valloc) valloc = nullptr;
            decltype(malloc_zone_t::realloc) realloc = nullptr;
            decltype(malloc_zone_t::batch_malloc) batchMalloc = nullptr;
            decltype(malloc_zone_t::memalign) memalign = nullptr;
            decltype(malloc_zone_t::malloc_with_options) mallocWithOptions = nullptr;
            decltype(malloc_zone_t::malloc_type_malloc) typedMalloc = nullptr;
            decltype(malloc_zone_t::malloc_type_calloc) typedCalloc = nullptr;
            decltype(malloc_zone_t::malloc_type_realloc) typedRealloc = nullptr;
            decltype(malloc_zone_t::malloc_type_memalign) typedMemalign = nullptr;
            decltype(malloc_zone_t::malloc_type_malloc_with_options) typedMallocWithOptions = nullptr;
        };
        ZoneEntries original;

        //==============================================================================================================
        void countOne()
        {
            if (pthread_getspecific(batchKey) == nullptr)
            {
                allocations.fetch_add(1, std::memory_order_relaxed);
            }
        }

        //==============================================================================================================
        void* countedMalloc(malloc_zone_t* zone, size_t size)
        {
            countOne();
            return original.malloc(zone, size);
        }

        //==============================================================================================================
        void* countedCalloc(malloc_zone_t* zone, size_t count, size_t size)
        {
            countOne();
            return original.calloc(zone, count, size);
        }

        //==============================================================================================================
        void* countedValloc(malloc_zone_t* zone, size_t size)
        {
            countOne();
            return original.valloc(zone, size);
        }

        //==============================================================================================================
        void* countedRealloc(malloc_zone_t* zone, void* block, size_t size)
        {
            countOne();
            return original.realloc(zone, block, size);
        }

        //==============================================================================================================
        unsigned countedBatchMalloc(malloc_zone_t* zone, size_t size, void** results, unsigned requested)
        {
            pthread_setspecific(batchKey, &batchKey);
            const unsigned returned = original.batchMalloc(zone, size, results, requested);
            pthread_setspecific(batchKey, nullptr);
            allocations.fetch_add(returned, std::memory_order_relaxed);
            return returned;
        }

        //==============================================================================================================
        void* countedMemalign(malloc_zone_t* zone, size_t alignment, size_t size)
        {
            countOne();
            return original.memalign(zone, alignment, size);
        }

        //==============================================================================================================
        void* countedMallocWithOptions(malloc_zone_t* zone, size_t alignment, size_t size, uint64_t options)
        {
            countOne();
            return original.mallocWithOptions(zone, alignment, size, options);
        }

        //==============================================================================================================
        void* countedTypedMalloc(malloc_zone_t* zone, size_t size, malloc_type_id_t type)
        {
            countOne();
            return original.typedMalloc(zone, size, type);
        }

        //==============================================================================================================
        void* countedTypedCalloc(malloc_zone_t* zone, size_t count, size_t size, malloc_type_id_t type)
        {
            countOne();
            return original.typedCalloc(zone, count, size, type);
        }

        //==============================================================================================================
        void* countedTypedRealloc(malloc_zone_t* zone, void* block, size_t size, malloc_type_id_t type)
        {
            countOne();
            return original.typedRealloc(zone, block, size, type);
        }

        //==============================================================================================================
        void* countedTypedMemalign(malloc_zone_t* zone, size_t alignment, size_t size, malloc_type_id_t type)
        {
            countOne();
            return original.typedMemalign(zone, alignment, size, type);
        }

        //==============================================================================================================
        void* countedTypedMallocWithOptions(malloc_zone_t* zone, size_t alignment, size_t size, uint64_t options, malloc_type_id_t type)
        {
            countOne();
            return original.typedMallocWithOptions(zone, alignment, size, options, type);
        }

        //==============================================================================================================
        // Replaces each entry the zone's version defines and the zone fills in; an absent entry stays absent.
        void wrapEntries(malloc_zone_t& zone)
        {
            original.malloc = zone.malloc;
            zone.malloc = countedMalloc;
            original.calloc = zone.calloc;
            zone.calloc = countedCalloc;
            original.valloc = zone.valloc;
            zone.valloc = countedValloc;
            original.realloc = zone.realloc;
            zone.realloc = countedRealloc;
            if (zone.batch_malloc != nullptr)
            {
                original.batchMalloc = zone.batch_malloc;
                zone.batch_malloc = countedBatchMalloc;
            }
            if (zone.version >= kMemalignVersion && zone.memalign != nullptr)
            {
                original.memalign = zone.memalign;
                zone.memalign = countedMemalign;
            }
            if (zone.version >= kOptionsVersion && zone.malloc_with_options != nullptr)
            {
                original.mallocWithOptions = zone.malloc_with_options;
                zone.malloc_with_options = countedMallocWithOptions;
            }
            if (zone.version >= kTypedVersion)
            {
                if (zone.malloc_type_malloc != nullptr)
                {
                    original.typedMalloc = zone.malloc_type_malloc;
                    zone.malloc_type_malloc = countedTypedMalloc;
                }
                if (zone.malloc_type_calloc != nullptr)
                {
                    original.typedCalloc = zone.malloc_type_calloc;
                    zone.malloc_type_calloc = countedTypedCalloc;
                }
                if (zone.malloc_type_realloc != nullptr)
                {
                    original.typedRealloc = zone.malloc_type_realloc;
                    zone.malloc_type_realloc = countedTypedRealloc;
                }
                if (zone.malloc_type_memalign != nullptr)
                {
                    original.typedMemalign = zone.malloc_type_memalign;
                    zone.malloc_type_memalign = countedTypedMemalign;
                }
                if (zone.malloc_type_malloc_with_options != nullptr)
                {
                    original.typedMallocWithOptions = zone.malloc_type_malloc_with_options;
                    zone.malloc_type_malloc_with_options = countedTypedMallocWithOptions;
                }
            }
        }

        //==============================================================================================================
        std::expected<void, std::string> install()
        {
            if (malloc_num_zones == 0 || malloc_zones == nullptr || malloc_zones[0] == nullptr)
            {
                return std::unexpected("allocation counter: libmalloc reports no zone 0");
            }
            malloc_zone_t* zone = malloc_zones[0];
            const int keyResult = pthread_key_create(&batchKey, nullptr);
            if (keyResult != 0)
            {
                return std::unexpected(std::format("allocation counter: cannot create the batch key ({})", keyResult));
            }

            // libmalloc keeps the zone table read-only; read its protection, open the pages for writing, restore it.
            const vm_address_t first = trunc_page(reinterpret_cast<vm_address_t>(zone));
            const vm_address_t last = round_page(reinterpret_cast<vm_address_t>(zone) + sizeof(malloc_zone_t));
            vm_address_t region = first;
            vm_size_t regionSize = 0;
            vm_region_basic_info_data_64_t info{};
            mach_msg_type_number_t infoCount = VM_REGION_BASIC_INFO_COUNT_64;
            mach_port_t objectName = MACH_PORT_NULL;
            kern_return_t result = vm_region_64(
                mach_task_self(), &region, &regionSize, VM_REGION_BASIC_INFO_64, reinterpret_cast<vm_region_info_t>(&info), &infoCount, &objectName
            );
            if (result != KERN_SUCCESS || region > first || region + regionSize < last)
            {
                return std::unexpected(std::format("allocation counter: cannot read the protection of zone 0 ({})", result));
            }
            result = vm_protect(mach_task_self(), first, last - first, false, VM_PROT_READ | VM_PROT_WRITE);
            if (result != KERN_SUCCESS)
            {
                return std::unexpected(std::format("allocation counter: cannot make zone 0 writable ({})", result));
            }
            wrapEntries(*zone);
            result = vm_protect(mach_task_self(), first, last - first, false, info.protection);
            if (result != KERN_SUCCESS)
            {
                return std::unexpected(std::format("allocation counter: cannot restore the protection of zone 0 ({})", result));
            }

            // malloc reaches zone 0 only while no other zone stands in front of it: MallocStackLogging, for one, sends
            // every call elsewhere and the count would stay at zero. One block proves the forwarders see malloc. The
            // volatile store keeps the compiler from eliding the pair.
            const uint64_t before = allocations.load(std::memory_order_relaxed);
            void* volatile probe = std::malloc(16);
            const uint64_t counted = allocations.load(std::memory_order_relaxed) - before;
            std::free(probe);
            if (counted != 1)
            {
                return std::unexpected(
                    std::format(
                        "allocation counter: one malloc counted {} times after wrapping zone 0, so malloc does not reach the zone's "
                        "table (MallocStackLogging or another zone in front of it); allocation counts would be wrong",
                        counted
                    )
                );
            }
            return {};
        }
    } // namespace

    //==================================================================================================================
    std::expected<void, std::string> installAllocationCounter()
    {
        static const std::expected<void, std::string> installed = install();
        return installed;
    }

    //==================================================================================================================
    uint64_t allocationCount()
    {
        return allocations.load(std::memory_order_relaxed);
    }
} // namespace rojoRHI::bench
