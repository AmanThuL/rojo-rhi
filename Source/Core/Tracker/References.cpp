//----------------------------------------------------------------------------------------------------------------------
/// @file References.cpp
/// @brief Implements the counting of the objects unsubmitted command lists reference and the destroy check that reads the counts.
//----------------------------------------------------------------------------------------------------------------------

#include "Core/Tracker/References.h"

#include "Base/Assert.h"

#include <string>

namespace rojoRHI::core {
    namespace {
        //==============================================================================================================
        /// Returns the count of `refs` that `kind` names.
        [[maybe_unused]] uint32_t& countOf(ListReferences& refs, RefKind kind)
        {
            switch (kind)
            {
            case RefKind::Root: return refs.roots;
            case RefKind::Addr: return refs.addrs;
            case RefKind::Handle: break;
            }
            return refs.others;
        }

        //==============================================================================================================
        /// Adds `delta` to the count of `kind` in the record `handle` names, under the pool's lock. Returns false when the handle
        /// is not live, or when it is a frame-arena page named by address: frame data resolves to no caller buffer. `image` is
        /// set when the object is a swapchain image.
        template<typename Handle> [[maybe_unused]] bool adjust(DeviceCore& core, Handle handle, RefKind kind, int delta, bool& image)
        {
            bool applied = false;
            core.pool<Handle>().update(handle, [&](auto& record) {
                if constexpr (std::is_same_v<Handle, Buffer>)
                {
                    if (delta > 0 && kind != RefKind::Handle && record.arenaFrame != 0)
                    {
                        return;
                    }
                }
                if constexpr (std::is_same_v<Handle, Texture>)
                {
                    image = record.swapchainImage;
                }
                uint32_t& count = countOf(record.refs, kind);
                count = static_cast<uint32_t>(static_cast<int64_t>(count) + delta);
                applied = true;
            });
            return applied;
        }

        //==============================================================================================================
        /// Applies `delta` to the object whose handle is `bits`, by the type tag in its top bits.
        [[maybe_unused]] bool adjustBits(DeviceCore& core, uint64_t bits, RefKind kind, int delta, bool& image)
        {
            const auto tag = static_cast<uint8_t>(bits >> 56);
            if (tag == handleTypeTag<Buffer>())
            {
                return adjust(core, Buffer{ bits }, kind, delta, image);
            }
            if (tag == handleTypeTag<Texture>())
            {
                return adjust(core, Texture{ bits }, kind, delta, image);
            }
            if (tag == handleTypeTag<Pipeline>())
            {
                return adjust(core, Pipeline{ bits }, kind, delta, image);
            }
            if (tag == handleTypeTag<Upscaler>())
            {
                return adjust(core, Upscaler{ bits }, kind, delta, image);
            }
            return false; // A type no recorded command names.
        }

        //==============================================================================================================
        /// Returns the set of `list` that holds the references of `kind`.
        [[maybe_unused]] std::unordered_set<uint64_t>& setOf(TrackerLog& log, RefKind kind)
        {
            switch (kind)
            {
            case RefKind::Root: return log.rootRefs;
            case RefKind::Addr: return log.addrRefs;
            case RefKind::Handle: break;
            }
            return log.otherRefs;
        }

        //==============================================================================================================
        /// Removes the counts `bits` hold of `kind` and empties the set.
        [[maybe_unused]] void releaseSet(DeviceCore& core, std::unordered_set<uint64_t>& set, RefKind kind)
        {
            for (const uint64_t bits : set)
            {
                bool image = false;
                adjustBits(core, bits, kind, -1, image);
            }
            set.clear();
        }
    } // namespace

    //==================================================================================================================
    void noteReferenceChecked([[maybe_unused]] CommandListCore& list, [[maybe_unused]] RefKind kind, [[maybe_unused]] uint64_t handleBits)
    {
#ifndef NDEBUG
        if (!list.device->validation() || handleBits == 0)
        {
            return;
        }
        std::unordered_set<uint64_t>& set = setOf(list.trackerLog, kind);
        if (set.contains(handleBits))
        {
            return;
        }
        bool image = false;
        if (!adjustBits(*list.device, handleBits, kind, 1, image))
        {
            return;
        }
        set.insert(handleBits);
        if (image)
        {
            list.trackerLog.images.push_back(Texture{ handleBits });
        }
#endif
    }

    //==================================================================================================================
    void releaseReferencesChecked([[maybe_unused]] CommandListCore& list)
    {
#ifndef NDEBUG
        if (list.device == nullptr)
        {
            return;
        }
        releaseSet(*list.device, list.trackerLog.rootRefs, RefKind::Root);
        releaseSet(*list.device, list.trackerLog.addrRefs, RefKind::Addr);
        releaseSet(*list.device, list.trackerLog.otherRefs, RefKind::Handle);
#endif
    }

    //==================================================================================================================
    void releaseSubmittedChecked([[maybe_unused]] std::span<const CommandList> lists)
    {
#ifndef NDEBUG
        for (const CommandList& list : lists)
        {
            releaseReferencesChecked(coreOf(list));
        }
#endif
    }

    //==================================================================================================================
    template<typename Handle> void checkUnreferencedChecked([[maybe_unused]] DeviceCore& core, [[maybe_unused]] Handle handle)
    {
#ifndef NDEBUG
        if (!core.validation())
        {
            return;
        }
        // The read here and the release below are separate lock holds; a race with a thread recording a reference is misuse.
        ListReferences refs;
        std::string label;
        // A handle that is not live is left to the destroy, which reports VU-rojo-core-0001 to 0003.
        core.pool<Handle>().update(handle, [&](const auto& record) {
            if constexpr (requires { record.refs; })
            {
                refs = record.refs;
                label = record.label;
            }
        });
        ROJORHI_VU(
            refs.roots == 0,
            "VU-rojo-abi-0003",
            "the buffer \"{}\" is destroyed while {} unsubmitted lists use an address in it as their root",
            label,
            refs.roots
        );
        ROJORHI_VU(
            refs.addrs == 0, "VU-rojo-buffer-0019", "the buffer \"{}\" is destroyed while {} unsubmitted lists were passed an address in it", label, refs.addrs
        );
        ROJORHI_VU(refs.others == 0, "VU-rojo-device-0007", "the object \"{}\" is destroyed while {} unsubmitted lists reference it", label, refs.others);
#endif
    }

    template void checkUnreferencedChecked<Buffer>(DeviceCore&, Buffer);
    template void checkUnreferencedChecked<Texture>(DeviceCore&, Texture);
    template void checkUnreferencedChecked<Pipeline>(DeviceCore&, Pipeline);
    template void checkUnreferencedChecked<ShaderLibrary>(DeviceCore&, ShaderLibrary);
    template void checkUnreferencedChecked<Heap>(DeviceCore&, Heap);
    template void checkUnreferencedChecked<Fence>(DeviceCore&, Fence);
    template void checkUnreferencedChecked<Upscaler>(DeviceCore&, Upscaler);
} // namespace rojoRHI::core
