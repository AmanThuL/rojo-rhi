//----------------------------------------------------------------------------------------------------------------------
/// @file Samplers.cpp
/// @brief Implements the checks of the sampler rules of chapter 04.
//----------------------------------------------------------------------------------------------------------------------

#include "Core/Samplers.h"

#include "Base/Assert.h"
#include "Core/Label.h"

#include <algorithm>
#include <format>
#include <utility>

namespace rojoRHI::core {
    namespace {
        //==============================================================================================================
        /// The largest `maxAnisotropy` a sampler may have: VU-rojo-texture-0029 allows 1 to 16 whatever a device reports, so a
        /// larger limit never widens it.
        [[maybe_unused]] uint32_t anisotropyBound(const DeviceCore& core)
        {
            return std::min(core.capabilities().limits.maxSamplerAnisotropy, 16u);
        }

        //==============================================================================================================
        [[maybe_unused]] bool anisotropyInRange(const DeviceCore& core, const SamplerDesc& desc)
        {
            return desc.maxAnisotropy >= 1 && desc.maxAnisotropy <= anisotropyBound(core);
        }

        //==============================================================================================================
        [[maybe_unused]] bool anisotropyFiltersLinear(const SamplerDesc& desc)
        {
            return desc.maxAnisotropy <= 1 || (desc.filter == SamplerFilter::Linear && desc.mipFilter == SamplerFilter::Linear);
        }
    } // namespace

    //==================================================================================================================
    Result<SamplerIndex> createSampler(DeviceCore& core, const SamplerDesc& desc)
    {
        ROJORHI_VU(anisotropyInRange(core, desc), "VU-rojo-texture-0029", "maxAnisotropy {} is not in 1 to {}", desc.maxAnisotropy, anisotropyBound(core));
        ROJORHI_VU(
            anisotropyFiltersLinear(desc),
            "VU-rojo-texture-0030",
            "maxAnisotropy {} is above 1 but the filter or the mip filter is not Linear",
            desc.maxAnisotropy
        );
        (void)resolveLabel(desc.label, "rojorhi.sampler.unnamed");
        const std::optional<uint32_t> slot = core.samplers().allocate(core.capabilities().maxSamplers, 0);
        if (!slot)
        {
            return std::unexpected(Error{ ErrorCategory::OutOfMemory, std::format("the sampler heap of {} slots is full", core.capabilities().maxSamplers) });
        }
        DeviceCounts::add(core.counts().descriptorWrites);
        DeviceCounts::add(core.counts().nativeCreateCalls);
        return static_cast<SamplerIndex>(*slot);
    }

    //==================================================================================================================
    void destroySampler(DeviceCore& core, SamplerIndex sampler)
    {
        [[maybe_unused]] const std::optional<uint64_t> owner = core.samplers().release(std::to_underlying(sampler), [&core] { return core.releaseFrame(); });
        ROJORHI_VU(
            owner.has_value(),
            "VU-rojo-device-0010",
            "the sampler index {} was not returned by createSampler or was already destroyed",
            std::to_underlying(sampler)
        );
    }

    //==================================================================================================================
    void checkSamplerWrite(DeviceCore& core, [[maybe_unused]] IndexRange range, [[maybe_unused]] uint32_t index, const SamplerDesc& desc)
    {
        ROJORHI_VU(
            range.kind == IndexRange::Kind::Samplers && index < range.count,
            "VU-rojo-texture-0036",
            "writeSampler names slot {} of a range of {} {} slots",
            index,
            range.count,
            range.kind == IndexRange::Kind::Samplers ? "sampler" : "view"
        );
        ROJORHI_VU(
            anisotropyInRange(core, desc) && anisotropyFiltersLinear(desc),
            "VU-rojo-texture-0037",
            "the sampler descriptor breaks a SamplerDesc rule: maxAnisotropy {} must be in 1 to {} and above 1 only with Linear filters",
            desc.maxAnisotropy,
            anisotropyBound(core)
        );
        ROJORHI_VU(
            core.samplers().holdsRange(range.base, range.count),
            "VU-rojo-texture-0031",
            "writeSampler names the range of {} slots from {}, which a reserve call did not return or which was released",
            range.count,
            range.base
        );
        (void)resolveLabel(desc.label, "rojorhi.sampler.unnamed");
        DeviceCounts::add(core.counts().descriptorWrites);
    }
} // namespace rojoRHI::core
