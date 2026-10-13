//----------------------------------------------------------------------------------------------------------------------
/// @file Tracker.cpp
/// @brief Implements the validation tracker's state of a device and the replay of submitted lists against it.
//----------------------------------------------------------------------------------------------------------------------

#include "Core/Tracker/Tracker.h"

#include "Base/Assert.h"
#include "Core/CommandLists.h"
#include "Core/DeviceCore.h"

#include <type_traits>
#include <utility>
#include <variant>

namespace rojoRHI::core {
    namespace {
        //==============================================================================================================
        /// Returns the number of mips or layers from `base` that `count` names in a run of `total`; `count` is `kAllMips`
        /// (equal to `kAllLayers`) for the rest of the run. The recording checks keep the run inside the texture.
        uint32_t resolvedCount(uint32_t base, uint32_t count, uint32_t total)
        {
            return count == kAllMips ? total - base : count;
        }
    } // namespace

    //==================================================================================================================
    TrackerLock Tracker::lock()
    {
        return TrackerLock(m_mutex);
    }

    //==================================================================================================================
    void Tracker::addTexture(Texture texture, std::string_view label, uint32_t mips, uint32_t layers, Layout initial, bool acquired)
    {
        TextureState state{ .label = std::string(label),
                            .mips = mips,
                            .layers = layers,
                            .layouts = std::vector<Layout>(static_cast<size_t>(mips) * layers, initial),
                            .acquired = acquired };
        const std::lock_guard held(m_mutex);
        m_textures.insert_or_assign(texture.bits, std::move(state));
    }

    //==================================================================================================================
    void Tracker::removeTexture(Texture texture)
    {
        const std::lock_guard held(m_mutex);
        m_textures.erase(texture.bits);
    }

    //==================================================================================================================
    std::optional<Layout> Tracker::layoutOf(Texture texture, uint32_t mip, uint32_t layer)
    {
        const std::lock_guard held(m_mutex);
        const auto found = m_textures.find(texture.bits);
        if (found == m_textures.end() || mip >= found->second.mips || layer >= found->second.layers)
        {
            return std::nullopt;
        }
        return found->second.layouts[static_cast<size_t>(mip) * found->second.layers + layer];
    }

    //==================================================================================================================
    void Tracker::replay([[maybe_unused]] const TrackerLock& held, const TrackerLog& log, QueueType queue, std::string_view list)
    {
        ROJORHI_INTERNAL_ASSERT(held.owns_lock() && held.mutex() == &m_mutex, "a list was replayed without the tracker's lock");
        for (const TrackerEvent& event : log.events)
        {
            std::visit(
                [&](const auto& record) {
                    using Record = std::decay_t<decltype(record)>;
                    if constexpr (std::is_same_v<Record, UseEvent>)
                    {
                        replayUse(held, record, queue, list);
                    }
                    else if constexpr (std::is_same_v<Record, GlobalBarrier>)
                    {
                        replayGlobal(record, queue, list);
                    }
                    else if constexpr (std::is_same_v<Record, BufferBarrier>)
                    {
                        replayBuffer(record, queue, list);
                    }
                    else
                    {
                        static_assert(std::is_same_v<Record, TextureBarrier>, "every kind of tracker event has a replay");
                        replayTexture(record, queue, list);
                    }
                },
                event
            );
        }
    }

    //==================================================================================================================
    Tracker::TextureState* Tracker::locate([[maybe_unused]] const TrackerLock& held, Texture texture)
    {
        ROJORHI_INTERNAL_ASSERT(held.owns_lock() && held.mutex() == &m_mutex, "a texture was looked up without the tracker's lock");
        const auto found = m_textures.find(texture.bits);
        return found == m_textures.end() ? nullptr : &found->second;
    }

    //==================================================================================================================
    void Tracker::checkPresent(const TrackerLock& held, Texture image)
    {
        const TextureState* texture = locate(held, image);
        if (texture == nullptr)
        {
            return;
        }
        for (size_t index = 0; index < texture->layouts.size(); ++index)
        {
            ROJORHI_VU(
                texture->layouts[index] == Layout::Present,
                "VU-rojo-present-0018",
                "the swapchain image \"{}\" is presented with subresource {} in layout {}, not Present",
                texture->label,
                index,
                static_cast<int>(texture->layouts[index])
            );
        }
    }

    //==================================================================================================================
    void Tracker::replayUse(const TrackerLock& held, const UseEvent& use, [[maybe_unused]] QueueType queue, [[maybe_unused]] std::string_view list)
    {
        // A buffer has no layout, and the attachment rules belong to the pass checks.
        if (use.texture.bits == 0 || use.rule == UseRule::None || use.rule == UseRule::Cmd0007 || use.rule == UseRule::Cmd0010)
        {
            return;
        }
        const TextureState* texture = locate(held, use.texture);
        if (texture == nullptr)
        {
            // The texture was destroyed after the list recorded the use; the lifetime rules, not the layouts, judge that.
            return;
        }
        const uint32_t mipEnd = use.range.baseMip + resolvedCount(use.range.baseMip, use.range.mipCount, texture->mips);
        const uint32_t layerEnd = use.range.baseLayer + resolvedCount(use.range.baseLayer, use.range.layerCount, texture->layers);
        for (uint32_t mip = use.range.baseMip; mip < mipEnd; ++mip)
        {
            for (uint32_t layer = use.range.baseLayer; layer < layerEnd; ++layer)
            {
                const Layout current = texture->layouts[static_cast<size_t>(mip) * texture->layers + layer];
                // The Copy queue's rule reports first, then the entry's own rule, then the general one (the design's rule map).
                ROJORHI_VU(
                    !use.copyQueue || current == Layout::General,
                    "VU-rojo-sync-0006",
                    "the list \"{}\" runs on the Copy queue and uses mip {} layer {} of the texture \"{}\" in layout {}, not General",
                    list,
                    mip,
                    layer,
                    texture->label,
                    static_cast<int>(current)
                );
                if (holds(use.layouts, current))
                {
                    continue;
                }
                switch (use.rule)
                {
                case UseRule::Cmd0054:
                    if (use.access == Access::CopyRead)
                    {
                        ROJORHI_VU(
                            false,
                            "VU-rojo-cmd-0054",
                            "the list \"{}\" copies from mip {} layer {} of the texture \"{}\" in layout {}, not CopySrc or General",
                            list,
                            mip,
                            layer,
                            texture->label,
                            static_cast<int>(current)
                        );
                    }
                    else
                    {
                        ROJORHI_VU(
                            false,
                            "VU-rojo-cmd-0054",
                            "the list \"{}\" copies to mip {} layer {} of the texture \"{}\" in layout {}, not CopyDst or General",
                            list,
                            mip,
                            layer,
                            texture->label,
                            static_cast<int>(current)
                        );
                    }
                    break;
                case UseRule::Cmd0067:
                    ROJORHI_VU(
                        false,
                        "VU-rojo-cmd-0067",
                        "the list \"{}\" copies a buffer to mip {} layer {} of the texture \"{}\" in layout {}, not CopyDst or General",
                        list,
                        mip,
                        layer,
                        texture->label,
                        static_cast<int>(current)
                    );
                    break;
                case UseRule::Cmd0069:
                    ROJORHI_VU(
                        false,
                        "VU-rojo-cmd-0069",
                        "the list \"{}\" copies mip {} layer {} of the texture \"{}\" in layout {} to a buffer, not CopySrc or General",
                        list,
                        mip,
                        layer,
                        texture->label,
                        static_cast<int>(current)
                    );
                    break;
                case UseRule::Cmd0070:
                    ROJORHI_VU(
                        false,
                        "VU-rojo-cmd-0070",
                        "the list \"{}\" clears mip {} layer {} of the texture \"{}\" in layout {}, not CopyDst or General",
                        list,
                        mip,
                        layer,
                        texture->label,
                        static_cast<int>(current)
                    );
                    break;
                case UseRule::Upscale0016:
                    ROJORHI_VU(
                        false,
                        "VU-rojo-upscale-0016",
                        "the list \"{}\" upscales with {} mip {} layer {} of the texture \"{}\" in layout {}, which the upscale does not permit",
                        list,
                        use.access == Access::ShaderWrite ? "the output," : "an input,",
                        mip,
                        layer,
                        texture->label,
                        static_cast<int>(current)
                    );
                    break;
                default:
                    ROJORHI_VU(
                        false,
                        "VU-rojo-sync-0004",
                        "the list \"{}\" uses mip {} layer {} of the texture \"{}\" in layout {}, which does not permit the use",
                        list,
                        mip,
                        layer,
                        texture->label,
                        static_cast<int>(current)
                    );
                    break;
                }
            }
        }
    }

    //==================================================================================================================
    void Tracker::replayGlobal([[maybe_unused]] const GlobalBarrier& barrier, [[maybe_unused]] QueueType queue, [[maybe_unused]] std::string_view list)
    {
        // A global barrier moves no layout; the hazard rules that read it are listed in Tools/vu_pending.txt.
    }

    //==================================================================================================================
    void Tracker::replayBuffer([[maybe_unused]] const BufferBarrier& barrier, [[maybe_unused]] QueueType queue, [[maybe_unused]] std::string_view list)
    {
        // Buffers have no layout; the hazard rules that read a buffer barrier are listed in Tools/vu_pending.txt.
    }

    //==================================================================================================================
    void Tracker::replayTexture(const TextureBarrier& barrier, [[maybe_unused]] QueueType queue, [[maybe_unused]] std::string_view list)
    {
        const auto found = m_textures.find(barrier.texture.bits);
        if (found == m_textures.end())
        {
            // The texture was destroyed after the list recorded the barrier; the lifetime rules, not the layouts, judge that.
            return;
        }
        TextureState& texture = found->second;
        if (texture.acquired)
        {
            // The first barrier on an acquired image reports its own rule before VU-rojo-sync-0013, which it also breaks.
            ROJORHI_VU(
                barrier.oldLayout == Layout::Undefined,
                "VU-rojo-present-0014",
                "the first texture barrier on the swapchain image \"{}\" since its acquire, in the list \"{}\", starts from layout {}, not Undefined",
                texture.label,
                list,
                static_cast<int>(barrier.oldLayout)
            );
            texture.acquired = false;
        }
        const uint32_t mipEnd = barrier.range.baseMip + resolvedCount(barrier.range.baseMip, barrier.range.mipCount, texture.mips);
        const uint32_t layerEnd = barrier.range.baseLayer + resolvedCount(barrier.range.baseLayer, barrier.range.layerCount, texture.layers);
        for (uint32_t mip = barrier.range.baseMip; mip < mipEnd; ++mip)
        {
            for (uint32_t layer = barrier.range.baseLayer; layer < layerEnd; ++layer)
            {
                Layout& current = texture.layouts[static_cast<size_t>(mip) * texture.layers + layer];
                ROJORHI_VU(
                    barrier.oldLayout == Layout::Undefined || barrier.oldLayout == current,
                    "VU-rojo-sync-0013",
                    "a texture barrier in the list \"{}\" starts mip {} layer {} of the texture \"{}\" from layout {}, but it is in layout {}",
                    list,
                    mip,
                    layer,
                    texture.label,
                    static_cast<int>(barrier.oldLayout),
                    static_cast<int>(current)
                );
                current = barrier.newLayout;
            }
        }
    }

    //==================================================================================================================
    TrackerLock lockTrackerChecked(DeviceCore& core)
    {
        return core.validation() ? core.tracker().lock() : TrackerLock{};
    }

    //==================================================================================================================
    void replayListsChecked(DeviceCore& core, QueueType type, std::span<const CommandList> lists, const TrackerLock& held)
    {
        if (!core.validation())
        {
            return;
        }
        for (const CommandList& handle : lists)
        {
            const CommandListCore& list = coreOf(handle);
            core.tracker().replay(held, list.trackerLog, type, list.label);
        }
    }

    //==================================================================================================================
    void checkPresentLayoutChecked(DeviceCore& core, Texture image)
    {
        if (core.validation())
        {
            const TrackerLock held = core.tracker().lock();
            core.tracker().checkPresent(held, image);
        }
    }

    //==================================================================================================================
    void trackTextureChecked(DeviceCore& core, Texture texture, std::string_view label, uint32_t mips, uint32_t layers, Layout initial, bool acquired)
    {
        if (core.validation())
        {
            core.tracker().addTexture(texture, label, mips, layers, initial, acquired);
        }
    }

    //==================================================================================================================
    void forgetTextureChecked(DeviceCore& core, Texture texture)
    {
        if (core.validation())
        {
            core.tracker().removeTexture(texture);
        }
    }
} // namespace rojoRHI::core
