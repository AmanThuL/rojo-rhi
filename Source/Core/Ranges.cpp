//----------------------------------------------------------------------------------------------------------------------
/// @file Ranges.cpp
/// @brief Implements the reservation and release of caller-owned heap slot ranges.
//----------------------------------------------------------------------------------------------------------------------

#include "Core/Ranges.h"

#include "Base/Assert.h"

#include <format>
#include <optional>
#include <string_view>
#include <vector>

namespace rojoRHI::core {
    namespace {
        //==============================================================================================================
        Result<IndexRange> reserve(SlotAllocator& slots, uint32_t capacity, uint32_t count, IndexRange::Kind kind, std::string_view heap)
        {
            const std::optional<uint32_t> base = slots.allocateRange(capacity, count, 0);
            if (!base)
            {
                return std::unexpected(
                    Error{ ErrorCategory::OutOfMemory, std::format("the {} heap of {} slots has no contiguous run of {} free slots", heap, capacity, count) }
                );
            }
            return IndexRange{ *base, count, kind };
        }
    } // namespace

    //==================================================================================================================
    Result<IndexRange> reserveViews(DeviceCore& core, uint32_t count)
    {
        ROJORHI_VU(count > 0, "VU-rojo-texture-0032", "reserveViews is called with a count of zero");
        return reserve(core.views(), core.capabilities().maxViews, count, IndexRange::Kind::Views, "view");
    }

    //==================================================================================================================
    Result<IndexRange> reserveSamplers(DeviceCore& core, uint32_t count)
    {
        ROJORHI_VU(count > 0, "VU-rojo-texture-0035", "reserveSamplers is called with a count of zero");
        return reserve(core.samplers(), core.capabilities().maxSamplers, count, IndexRange::Kind::Samplers, "sampler");
    }

    //==================================================================================================================
    void releaseRange(DeviceCore& core, IndexRange range)
    {
        SlotAllocator& slots = range.kind == IndexRange::Kind::Views ? core.views() : core.samplers();
        const std::optional<std::vector<uint64_t>> owners = slots.releaseRange(range.base, range.count, [&core] { return core.releaseFrame(); });
        ROJORHI_VU(
            owners.has_value(),
            "VU-rojo-texture-0038",
            "the range of {} slots from {} was not returned by a reserve call, was modified, or was already released",
            range.count,
            range.base
        );
        if (!owners || range.kind != IndexRange::Kind::Views)
        {
            return;
        }
        for (const uint64_t owner : *owners)
        {
            if (owner != 0)
            {
                core.pool<Texture>().update(Texture{ owner }, [](TextureRecord& held) {
                    ROJORHI_INTERNAL_ASSERT(held.views > 0, "a texture counts fewer views than hold it");
                    --held.views;
                });
            }
        }
    }
} // namespace rojoRHI::core
