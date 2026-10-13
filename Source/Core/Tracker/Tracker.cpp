//----------------------------------------------------------------------------------------------------------------------
/// @file Tracker.cpp
/// @brief Implements the validation tracker's state of a device and the replay of submitted lists against it.
//----------------------------------------------------------------------------------------------------------------------

#include "Core/Tracker/Tracker.h"

#include "Base/Assert.h"
#include "Core/CommandLists.h"
#include "Core/DeviceCore.h"

#include <format>
#include <string>
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

        //==============================================================================================================
        /// Returns the byte past the range of `size` bytes from `offset` in a buffer of `total` bytes; `kWholeSize` runs to the
        /// end. The recording checks keep the range inside the buffer.
        uint64_t rangeEnd(uint64_t offset, uint64_t size, uint64_t total)
        {
            return size == kWholeSize ? total : offset + size;
        }

        //==============================================================================================================
        /// Returns the name of `queue` for reports.
        [[maybe_unused]] std::string_view queueName(QueueType queue)
        {
            switch (queue)
            {
            case QueueType::Compute: return "Compute";
            case QueueType::Copy: return "Copy";
            default: return "Graphics";
            }
        }

        //==============================================================================================================
        /// Returns the earlier use of `hazard` for reports: a use, or a transition.
        [[maybe_unused]] std::string earlierUse(const Hazard& hazard)
        {
            return std::format(
                "{} stage {:#x} access {:#x}",
                hazard.earlierTransition ? "a layout transition to" : "a use at",
                static_cast<uint32_t>(hazard.earlier.stage),
                static_cast<uint32_t>(hazard.earlier.access)
            );
        }

        //==============================================================================================================
        /// Reports `hazard`, found between an earlier use and `later`, made by the list `list` on `queue` to `memory`, a
        /// description of the memory that names its resource by label: VU-rojo-sync-0021 when no barrier lies between the two,
        /// otherwise VU-rojo-sync-0024.
        void reportHazard(
            [[maybe_unused]] const Hazard& hazard,
            [[maybe_unused]] Scope later,
            [[maybe_unused]] std::string_view memory,
            [[maybe_unused]] QueueType queue,
            [[maybe_unused]] std::string_view list
        )
        {
            ROJORHI_VU(
                hazard.kind != HazardKind::NoBarrier,
                "VU-rojo-sync-0021",
                "the list \"{}\" uses {} at stage {:#x} access {:#x} on the {} queue after {}, with no barrier between them",
                list,
                memory,
                static_cast<uint32_t>(later.stage),
                static_cast<uint32_t>(later.access),
                queueName(queue),
                earlierUse(hazard)
            );
            ROJORHI_VU(
                hazard.kind != HazardKind::Unordered,
                "VU-rojo-sync-0024",
                "the list \"{}\" uses {} at stage {:#x} access {:#x} on the {} queue after {}, and no barrier between them covers the "
                "earlier use at its source and the later one at its destination",
                list,
                memory,
                static_cast<uint32_t>(later.stage),
                static_cast<uint32_t>(later.access),
                queueName(queue),
                earlierUse(hazard)
            );
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
                            .hazards = std::vector<UnitHazards>(static_cast<size_t>(mips) * layers),
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
    void Tracker::addBuffer(Buffer buffer, std::string_view label, uint64_t size, Heap heap, uint64_t heapOffset)
    {
        BufferState state{ .label = std::string(label), .size = size, .heap = heap, .heapOffset = heapOffset, .segments = ByteSegments(size) };
        const std::lock_guard held(m_mutex);
        m_buffers.insert_or_assign(buffer.bits, std::move(state));
    }

    //==================================================================================================================
    void Tracker::removeBuffer(Buffer buffer)
    {
        const std::lock_guard held(m_mutex);
        m_buffers.erase(buffer.bits);
    }

    //==================================================================================================================
    std::optional<size_t> Tracker::segmentCount(Buffer buffer)
    {
        const std::lock_guard held(m_mutex);
        const auto found = m_buffers.find(buffer.bits);
        if (found == m_buffers.end())
        {
            return std::nullopt;
        }
        return found->second.segments.segments().size();
    }

    //==================================================================================================================
    WriteOrder Tracker::lastWriteOrder(const TrackerLock& held, QueueType queue, Buffer buffer, uint64_t offset, uint64_t size, Scope use)
    {
        const BufferState* state = locate(held, buffer);
        if (state == nullptr)
        {
            return WriteOrder::None;
        }
        const uint64_t end = rangeEnd(offset, size, state->size);
        WriteOrder worst = WriteOrder::None;
        for (const ByteSegment& segment : state->segments.segments())
        {
            if (segment.end > offset && segment.begin < end)
            {
                const WriteOrder order = core::lastWriteOrder(segment.hazards.queues[static_cast<size_t>(queue)], use);
                worst = order > worst ? order : worst;
            }
        }
        return worst;
    }

    //==================================================================================================================
    void Tracker::replay([[maybe_unused]] const TrackerLock& held, const TrackerLog& log, QueueType queue, std::string_view list)
    {
        ROJORHI_INTERNAL_ASSERT(held.owns_lock() && held.mutex() == &m_mutex, "a list was replayed without the tracker's lock");
        ROJORHI_INTERNAL_ASSERT(log.commands.size() == log.events.size(), "a tracker event was logged without its command");
        uint64_t& position = m_positions[static_cast<size_t>(queue)];
        for (size_t index = 0; index < log.events.size(); ++index)
        {
            // Each command takes the next position on the queue; the events of one command, and the first event of each list,
            // are told apart by the command ordinal.
            if (index == 0 || log.commands[index] != log.commands[index - 1])
            {
                ++position;
            }
            std::visit(
                [&](const auto& record) {
                    using Record = std::decay_t<decltype(record)>;
                    if constexpr (std::is_same_v<Record, UseEvent>)
                    {
                        replayUse(held, record, queue, list, position);
                    }
                    else if constexpr (std::is_same_v<Record, GlobalBarrier>)
                    {
                        replayGlobal(record, queue, position);
                    }
                    else if constexpr (std::is_same_v<Record, BufferBarrier>)
                    {
                        replayBuffer(held, record, queue, position);
                    }
                    else
                    {
                        static_assert(std::is_same_v<Record, TextureBarrier>, "every kind of tracker event has a replay");
                        replayTexture(held, record, queue, list, position);
                    }
                },
                log.events[index]
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
    std::array<uint64_t, 3> Tracker::mergeFloor(QueueType queue) const
    {
        // Every later command on another queue takes a position above that queue's last; on `queue` the current command's
        // own uses are still to be told apart from its later uses.
        std::array<uint64_t, 3> floor = m_positions;
        for (size_t index = 0; index < floor.size(); ++index)
        {
            floor[index] += index == static_cast<size_t>(queue) ? 0 : 1;
        }
        return floor;
    }

    //==================================================================================================================
    Tracker::BufferState* Tracker::locate([[maybe_unused]] const TrackerLock& held, Buffer buffer)
    {
        ROJORHI_INTERNAL_ASSERT(held.owns_lock() && held.mutex() == &m_mutex, "a buffer was looked up without the tracker's lock");
        const auto found = m_buffers.find(buffer.bits);
        return found == m_buffers.end() ? nullptr : &found->second;
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
    void Tracker::replayUse(const TrackerLock& held, const UseEvent& use, QueueType queue, std::string_view list, uint64_t position)
    {
        // A buffer has no layout, only hazards.
        if (use.texture.bits == 0)
        {
            replayBufferUse(held, use, queue, list, position);
            return;
        }
        TextureState* texture = locate(held, use.texture);
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
                // The Copy queue's rule reports first, then the entry's own rule, then the general one.
                ROJORHI_VU(
                    queue != QueueType::Copy || current == Layout::General,
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
                            holds(use.layouts, current),
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
                            holds(use.layouts, current),
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
                        holds(use.layouts, current),
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
                        holds(use.layouts, current),
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
                        holds(use.layouts, current),
                        "VU-rojo-cmd-0070",
                        "the list \"{}\" clears mip {} layer {} of the texture \"{}\" in layout {}, not CopyDst or General",
                        list,
                        mip,
                        layer,
                        texture->label,
                        static_cast<int>(current)
                    );
                    break;
                case UseRule::Cmd0007:
                    if (use.stage == Stage::Resolve)
                    {
                        ROJORHI_VU(
                            holds(use.layouts, current),
                            "VU-rojo-cmd-0007",
                            "the list \"{}\" resolves to mip {} layer {} of the texture \"{}\" in layout {}, not ColorAttachment",
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
                            holds(use.layouts, current),
                            "VU-rojo-cmd-0007",
                            "the list \"{}\" renders to mip {} layer {} of the texture \"{}\" in layout {}, not ColorAttachment",
                            list,
                            mip,
                            layer,
                            texture->label,
                            static_cast<int>(current)
                        );
                    }
                    break;
                case UseRule::Cmd0010:
                    ROJORHI_VU(
                        holds(use.layouts, current),
                        "VU-rojo-cmd-0010",
                        "the list \"{}\" uses mip {} layer {} of the texture \"{}\" in layout {} as its depth attachment, not DepthStencilWrite",
                        list,
                        mip,
                        layer,
                        texture->label,
                        static_cast<int>(current)
                    );
                    break;
                case UseRule::Upscale0016:
                    ROJORHI_VU(
                        holds(use.layouts, current),
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
                        holds(use.layouts, current),
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
        replayTextureHazards(*texture, use, queue, list, position);
    }

    //==================================================================================================================
    void Tracker::replayTextureHazards(TextureState& texture, const UseEvent& use, QueueType queue, std::string_view list, uint64_t position)
    {
        const Scope later{ .stage = use.stage, .access = use.access };
        const uint32_t mipEnd = use.range.baseMip + resolvedCount(use.range.baseMip, use.range.mipCount, texture.mips);
        const uint32_t layerEnd = use.range.baseLayer + resolvedCount(use.range.baseLayer, use.range.layerCount, texture.layers);
        for (uint32_t mip = use.range.baseMip; mip < mipEnd; ++mip)
        {
            for (uint32_t layer = use.range.baseLayer; layer < layerEnd; ++layer)
            {
                QueueHazards& state = texture.hazards[static_cast<size_t>(mip) * texture.layers + layer].on(queue);
                const Hazard hazard = checkUse(state, position, later);
                if (hazard.kind != HazardKind::None)
                {
                    reportHazard(hazard, later, std::format("mip {} layer {} of the texture \"{}\"", mip, layer, texture.label), queue, list);
                }
                recordUse(state, position, later);
            }
        }
    }

    //==================================================================================================================
    void Tracker::replayBufferUse(const TrackerLock& held, const UseEvent& use, QueueType queue, std::string_view list, uint64_t position)
    {
        BufferState* buffer = locate(held, use.buffer);
        if (buffer == nullptr)
        {
            // A frame-arena page is not followed, and a buffer destroyed after the list recorded the use is the lifetime
            // rules' to judge.
            return;
        }
        const Scope later{ .stage = use.stage, .access = use.access };
        const uint64_t end = rangeEnd(use.offset, use.size, buffer->size);
        for (ByteSegment& segment : buffer->segments.isolate(use.offset, end))
        {
            QueueHazards& state = segment.hazards.on(queue);
            const Hazard hazard = checkUse(state, position, later);
            if (hazard.kind != HazardKind::None)
            {
                reportHazard(hazard, later, std::format("bytes {} to {} of the buffer \"{}\"", segment.begin, segment.end, buffer->label), queue, list);
            }
            recordUse(state, position, later);
        }
        buffer->segments.merge(use.offset, end, mergeFloor(queue));
    }

    //==================================================================================================================
    void Tracker::replayGlobal(const GlobalBarrier& barrier, QueueType queue, uint64_t position)
    {
        const Scope source{ .stage = barrier.srcStage, .access = barrier.srcAccess };
        const Scope destination{ .stage = barrier.dstStage, .access = barrier.dstAccess };
        for (auto& [bits, texture] : m_textures)
        {
            for (UnitHazards& unit : texture.hazards)
            {
                recordBarrier(unit.on(queue), position, source, destination);
            }
        }
        for (auto& [bits, buffer] : m_buffers)
        {
            for (ByteSegment& segment : buffer.segments.segments())
            {
                recordBarrier(segment.hazards.on(queue), position, source, destination);
            }
        }
    }

    //==================================================================================================================
    void Tracker::replayBuffer(const TrackerLock& held, const BufferBarrier& barrier, QueueType queue, uint64_t position)
    {
        BufferState* buffer = locate(held, barrier.buffer);
        if (buffer == nullptr)
        {
            return;
        }
        const Scope source{ .stage = barrier.srcStage, .access = barrier.srcAccess };
        const Scope destination{ .stage = barrier.dstStage, .access = barrier.dstAccess };
        // The barrier orders exactly the range it names (chapter 09).
        const uint64_t end = rangeEnd(barrier.offset, barrier.size, buffer->size);
        for (ByteSegment& segment : buffer->segments.isolate(barrier.offset, end))
        {
            recordBarrier(segment.hazards.on(queue), position, source, destination);
        }
        buffer->segments.merge(barrier.offset, end, mergeFloor(queue));
    }

    //==================================================================================================================
    void Tracker::replayTexture(
        const TrackerLock& held, const TextureBarrier& barrier, QueueType queue, [[maybe_unused]] std::string_view list, uint64_t position
    )
    {
        TextureState* found = locate(held, barrier.texture);
        if (found == nullptr)
        {
            // The texture was destroyed after the list recorded the barrier; the lifetime rules, not the layouts, judge that.
            return;
        }
        TextureState& texture = *found;
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
                const Scope source{ .stage = barrier.srcStage, .access = barrier.srcAccess };
                const Scope destination{ .stage = barrier.dstStage, .access = barrier.dstAccess };
                QueueHazards& state = texture.hazards[static_cast<size_t>(mip) * texture.layers + layer].on(queue);
                if (barrier.oldLayout == barrier.newLayout)
                {
                    recordBarrier(state, position, source, destination);
                    continue;
                }
                // A transition stands between the uses around it as a write would, so its own source covers the uses before it.
                [[maybe_unused]] const Hazard hazard = checkTransition(state, position, source, barrier.oldLayout == Layout::Undefined);
                ROJORHI_VU(
                    hazard.kind == HazardKind::None,
                    "VU-rojo-sync-0024",
                    "a texture barrier in the list \"{}\" on the {} queue moves mip {} layer {} of the texture \"{}\" from layout {} to {} with "
                    "source stage {:#x} access {:#x}, which does not cover {}",
                    list,
                    queueName(queue),
                    mip,
                    layer,
                    texture.label,
                    static_cast<int>(barrier.oldLayout),
                    static_cast<int>(barrier.newLayout),
                    static_cast<uint32_t>(source.stage),
                    static_cast<uint32_t>(source.access),
                    earlierUse(hazard)
                );
                recordTransition(state, position, destination);
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

    //==================================================================================================================
    void trackBufferChecked(DeviceCore& core, Buffer buffer, std::string_view label, uint64_t size, Heap heap, uint64_t heapOffset)
    {
        if (core.validation())
        {
            core.tracker().addBuffer(buffer, label, size, heap, heapOffset);
        }
    }

    //==================================================================================================================
    void forgetBufferChecked(DeviceCore& core, Buffer buffer)
    {
        if (core.validation())
        {
            core.tracker().removeBuffer(buffer);
        }
    }
} // namespace rojoRHI::core
