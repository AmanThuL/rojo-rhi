//----------------------------------------------------------------------------------------------------------------------
/// @file Tracker.cpp
/// @brief Implements the validation tracker's state of a device and the replay of submitted lists against it.
//----------------------------------------------------------------------------------------------------------------------

#include "Core/Tracker/Tracker.h"

#include "Base/Assert.h"
#include "Core/CommandLists.h"
#include "Core/DeviceCore.h"

#include <algorithm>
#include <format>
#include <iterator>
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

        //==============================================================================================================
        /// Reports `race`, a change on another queue to the mapping of tiles of `memory`, a description of a subresource or
        /// byte range that names its resource by label, that no fence orders before the use the list `list` makes of them on
        /// `queue` and that the CPU has not seen complete (VU-rojo-sparse-0013).
        void reportMappingRace(
            [[maybe_unused]] const std::optional<TileRace>& race,
            [[maybe_unused]] std::string_view memory,
            [[maybe_unused]] QueueType queue,
            [[maybe_unused]] std::string_view list
        )
        {
            ROJORHI_VU(
                !race.has_value(),
                "VU-rojo-sparse-0013",
                "the list \"{}\" on the {} queue uses tiles of {} whose mapping changed at position {} on the {} queue, and no fence orders "
                "the change before the use",
                list,
                queueName(queue),
                memory,
                race->position,
                queueName(race->queue)
            );
        }

        //==============================================================================================================
        /// Reports `race`, a use on another queue of tiles of `memory`, a description of a subresource or byte range that names
        /// its resource by label, whose mapping `Queue::mapTiles` on `queue` changes, when no fence orders the use before the
        /// change and the CPU has not seen it complete (VU-rojo-sparse-0013).
        void reportUseRace([[maybe_unused]] const std::optional<TileRace>& race, [[maybe_unused]] std::string_view memory, [[maybe_unused]] QueueType queue)
        {
            ROJORHI_VU(
                !race.has_value(),
                "VU-rojo-sparse-0013",
                "mapTiles on the {} queue changes the mapping of tiles of {}, which work on the {} queue used at position {}; no fence orders "
                "that work before the change, and the CPU has not seen it complete",
                queueName(queue),
                memory,
                queueName(race->queue),
                race->position
            );
        }

        //==============================================================================================================
        /// Returns the scope of `use`.
        Scope scopeOf(const UseEvent& use)
        {
            return Scope{ .stage = use.stage, .access = use.access };
        }

        //==============================================================================================================
        /// Returns the first mark of `marks`, a subresource's on one queue, that an event there must still follow: with no
        /// `source`, any mark, since only a discard follows one; with `source`, a discard's, a mark it does not cover and that
        /// is not at or below `fenced`, which a fence wait or the CPU's observation orders before the discard. Returns null
        /// when there is none.
        const AliasMark* uncovered(const AliasMarks& marks, uint64_t fenced, std::optional<Scope> source)
        {
            for (const AliasMarks::Range& range : marks.ranges())
            {
                const std::optional<AliasMark>& mark = range.state;
                if (mark.has_value() && !(source.has_value() && (mark->position <= fenced || sourceCovers(*source, mark->scope, true))))
                {
                    return &*mark;
                }
            }
            return nullptr;
        }

        //==============================================================================================================
        /// Reports `hazard`, found between a use on another queue and `later`, made by the list `list` on `queue` to `memory`,
        /// a description of the memory that names its resource by label, when it holds a pair no fence orders
        /// (VU-rojo-queue-0005).
        void reportQueueHazard(
            [[maybe_unused]] const std::optional<QueueHazard>& hazard,
            [[maybe_unused]] Scope later,
            [[maybe_unused]] std::string_view memory,
            [[maybe_unused]] QueueType queue,
            [[maybe_unused]] std::string_view list
        )
        {
            ROJORHI_VU(
                !hazard.has_value(),
                "VU-rojo-queue-0005",
                "the list \"{}\" uses {} at stage {:#x} access {:#x} on the {} queue after {} on the {} queue, and no fence orders the "
                "earlier use's submission before this one",
                list,
                memory,
                static_cast<uint32_t>(later.stage),
                static_cast<uint32_t>(later.access),
                queueName(queue),
                earlierUse(Hazard{ .earlier = hazard->earlier, .earlierTransition = hazard->earlierTransition }),
                queueName(hazard->queue)
            );
        }

        //==============================================================================================================
        /// Returns the stage and access of `scope` for reports.
        [[maybe_unused]] std::string scopeText(Scope scope)
        {
            return std::format("stage {:#x} access {:#x}", static_cast<uint32_t>(scope.stage), static_cast<uint32_t>(scope.access));
        }

        //==============================================================================================================
        /// Reports an indirect read, made under `rule` by the list `list` on `queue` of `memory`, a description of the bytes
        /// that names their buffer, when `missing`: no barrier since the last write of those bytes on the queue has a
        /// destination stage and access that include `IndirectArgs`. Each entry's identifier, and each of its arms, has its
        /// own site.
        void reportIndirect(
            UseRule rule,
            [[maybe_unused]] bool missing,
            [[maybe_unused]] std::string_view memory,
            [[maybe_unused]] QueueType queue,
            [[maybe_unused]] std::string_view list
        )
        {
            [[maybe_unused]] constexpr std::string_view kWhy =
                "no barrier since the last write of those bytes has a destination stage and access that include IndirectArgs";
            switch (rule)
            {
            case UseRule::Indirect0005Records:
                ROJORHI_VU(
                    !missing,
                    "VU-rojo-indirect-0005",
                    "drawIndirect in the list \"{}\" on the {} queue reads its records from {}, and {}",
                    list,
                    queueName(queue),
                    memory,
                    kWhy
                );
                break;
            case UseRule::Indirect0005Count:
                ROJORHI_VU(
                    !missing,
                    "VU-rojo-indirect-0005",
                    "drawIndirect in the list \"{}\" on the {} queue reads its count from {}, and {}",
                    list,
                    queueName(queue),
                    memory,
                    kWhy
                );
                break;
            case UseRule::Indirect0011Records:
                ROJORHI_VU(
                    !missing,
                    "VU-rojo-indirect-0011",
                    "drawIndexedIndirect in the list \"{}\" on the {} queue reads its records from {}, and {}",
                    list,
                    queueName(queue),
                    memory,
                    kWhy
                );
                break;
            case UseRule::Indirect0011Count:
                ROJORHI_VU(
                    !missing,
                    "VU-rojo-indirect-0011",
                    "drawIndexedIndirect in the list \"{}\" on the {} queue reads its count from {}, and {}",
                    list,
                    queueName(queue),
                    memory,
                    kWhy
                );
                break;
            case UseRule::Indirect0017Records:
                ROJORHI_VU(
                    !missing,
                    "VU-rojo-indirect-0017",
                    "drawMeshTasksIndirect in the list \"{}\" on the {} queue reads its records from {}, and {}",
                    list,
                    queueName(queue),
                    memory,
                    kWhy
                );
                break;
            case UseRule::Indirect0017Count:
                ROJORHI_VU(
                    !missing,
                    "VU-rojo-indirect-0017",
                    "drawMeshTasksIndirect in the list \"{}\" on the {} queue reads its count from {}, and {}",
                    list,
                    queueName(queue),
                    memory,
                    kWhy
                );
                break;
            case UseRule::Indirect0020Records:
                ROJORHI_VU(
                    !missing,
                    "VU-rojo-indirect-0020",
                    "dispatchIndirect in the list \"{}\" on the {} queue reads its record from {}, and {}",
                    list,
                    queueName(queue),
                    memory,
                    kWhy
                );
                break;
            default: break;
            }
        }

        //==============================================================================================================
        /// Returns true when a subresource held by `from` (none before its first use since a creation or an acquire) in
        /// `layout` is handed over to `queue` (VU-rojo-sync-0025): `from` is another queue and the layout keeps contents. An
        /// `Undefined` event, another resource's use of the memory, hands over nothing, and the layout rules judge the next
        /// event as they would on the queue.
        [[maybe_unused]] bool handedOver(std::optional<QueueType> from, Layout layout, QueueType queue)
        {
            return from.has_value() && *from != queue && layout != Layout::Undefined;
        }

        //==============================================================================================================
        /// Returns true when `layout` is one the queues `a` and `b` both accept at a handoff (VU-rojo-sync-0025): `General`
        /// when either is the Copy queue; `General`, `ShaderRead`, `ShaderWrite`, `CopySrc` or `CopyDst` between the Graphics
        /// and Compute queues.
        [[maybe_unused]] bool sharedLayout(Layout layout, QueueType a, QueueType b)
        {
            if (a == QueueType::Copy || b == QueueType::Copy)
            {
                return layout == Layout::General;
            }
            switch (layout)
            {
            case Layout::General:
            case Layout::ShaderRead:
            case Layout::ShaderWrite:
            case Layout::CopySrc:
            case Layout::CopyDst: return true;
            default: return false;
            }
        }
    } // namespace

    //==================================================================================================================
    TrackerLock Tracker::lock()
    {
        return TrackerLock(m_mutex);
    }

    //==================================================================================================================
    void Tracker::addTexture(
        Texture texture,
        std::string_view label,
        uint32_t mips,
        uint32_t layers,
        Layout initial,
        bool acquired,
        Heap heap,
        uint64_t heapOffset,
        uint64_t heapSize,
        bool sparse
    )
    {
        const size_t subresources = static_cast<size_t>(mips) * layers;
        TextureState state{ .label = std::string(label),
                            .mips = mips,
                            .layers = layers,
                            .layouts = std::vector<LayoutEvent>(subresources, LayoutEvent{ .layout = initial }),
                            .hazards = std::vector<UnitHazards>(subresources),
                            .acquired = acquired,
                            .heap = heap,
                            .heapOffset = heapOffset,
                            .heapSize = heapSize };
        if (sparse)
        {
            state.tileUses.resize(subresources);
            state.tileMaps.resize(subresources);
            state.newTiles.resize(subresources);
        }
        const std::lock_guard held(m_mutex);
        if (heap.bits != 0)
        {
            ROJORHI_INTERNAL_ASSERT(heapSize != 0, "a placed texture occupies no bytes of its heap");
            // The texture's first event on each queue follows the uses other resources already made of its bytes there.
            HeapState& placed = m_heaps[heap.bits];
            std::array<AliasMarks, 3> marks;
            for (const HeapSegments::Range& segment : placed.segments.isolate(heapOffset, heapOffset + heapSize))
            {
                for (size_t queue = 0; queue < marks.size(); ++queue)
                {
                    const std::optional<HeapUse>& last = segment.state[queue];
                    if (last.has_value())
                    {
                        markAlias(marks[queue], segment.begin, segment.end, last->owner, last->use.position, last->use.scope);
                    }
                }
            }
            placed.segments.merge(heapOffset, heapOffset + heapSize);
            placed.textures.push_back(texture.bits);
            state.aliases.assign(subresources, marks);
        }
        m_textures.insert_or_assign(texture.bits, std::move(state));
    }

    //==================================================================================================================
    void Tracker::removeTexture(Texture texture)
    {
        const std::lock_guard held(m_mutex);
        const auto found = m_textures.find(texture.bits);
        if (found == m_textures.end())
        {
            return;
        }
        const auto heap = m_heaps.find(found->second.heap.bits);
        if (heap != m_heaps.end())
        {
            std::erase(heap->second.textures, texture.bits);
        }
        m_textures.erase(found);
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
        return found->second.layouts[static_cast<size_t>(mip) * found->second.layers + layer].layout;
    }

    //==================================================================================================================
    void Tracker::addBuffer(Buffer buffer, std::string_view label, uint64_t size, Heap heap, uint64_t heapOffset, bool sparse)
    {
        BufferState state{
            .label = std::string(label), .size = size, .heap = heap, .heapOffset = heapOffset, .segments = ByteSegments(size), .sparse = sparse
        };
        const std::lock_guard held(m_mutex);
        if (heap.bits != 0)
        {
            m_heaps.try_emplace(heap.bits);
        }
        m_buffers.insert_or_assign(buffer.bits, std::move(state));
    }

    //==================================================================================================================
    void Tracker::removeBuffer(Buffer buffer)
    {
        const std::lock_guard held(m_mutex);
        m_buffers.erase(buffer.bits);
    }

    //==================================================================================================================
    void Tracker::removeHeap(Heap heap)
    {
        const std::lock_guard held(m_mutex);
        m_heaps.erase(heap.bits);
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
    std::optional<size_t> Tracker::fenceValueCount(Fence fence)
    {
        const std::lock_guard held(m_mutex);
        const auto found = m_fences.find(fence.bits);
        if (found == m_fences.end())
        {
            return std::nullopt;
        }
        return found->second.size();
    }

    //==================================================================================================================
    std::optional<uint64_t> Tracker::lastMapping(Texture texture, uint32_t mip, uint32_t layer, QueueType queue)
    {
        const std::lock_guard held(m_mutex);
        const auto found = m_textures.find(texture.bits);
        if (found == m_textures.end() || found->second.tileMaps.empty() || mip >= found->second.mips || layer >= found->second.layers)
        {
            return std::nullopt;
        }
        return found->second.tileMaps[static_cast<size_t>(mip) * found->second.layers + layer][static_cast<size_t>(queue)].latest(std::nullopt);
    }

    //==================================================================================================================
    std::optional<size_t> Tracker::aliasMarkCount(Texture texture)
    {
        const std::lock_guard held(m_mutex);
        const auto found = m_textures.find(texture.bits);
        if (found == m_textures.end())
        {
            return std::nullopt;
        }
        size_t count = 0;
        for (const std::array<AliasMarks, 3>& queues : found->second.aliases)
        {
            for (const AliasMarks& marks : queues)
            {
                count += static_cast<size_t>(std::ranges::count_if(marks.ranges(), [](const AliasMarks::Range& range) { return range.state.has_value(); }));
            }
        }
        return count;
    }

    //==================================================================================================================
    std::optional<size_t> Tracker::heapSegmentCount(Heap heap)
    {
        const std::lock_guard held(m_mutex);
        const auto found = m_heaps.find(heap.bits);
        if (found == m_heaps.end())
        {
            return std::nullopt;
        }
        return found->second.segments.ranges().size();
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
        const uint64_t fenced = orderedBefore(queue)[static_cast<size_t>(queue)];
        WriteOrder worst = WriteOrder::None;
        for (const ByteSegment& segment : state->segments.segments())
        {
            if (segment.end > offset && segment.begin < end)
            {
                const WriteOrder order = core::lastWriteOrder(segment.hazards.queues[static_cast<size_t>(queue)], use, fenced);
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
        // An image the list noted that the tracker no longer follows was presented since the list recorded it. Judging it first keeps the
        // events that name it from reaching a texture that is gone.
        for ([[maybe_unused]] const Texture image : log.images)
        {
            ROJORHI_VU(
                locate(held, image) != nullptr,
                "VU-rojo-present-0019",
                "the list \"{}\" references the swapchain image {:#x}, which was presented after the list recorded it",
                list,
                image.bits
            );
        }
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
    void Tracker::wait([[maybe_unused]] const TrackerLock& held, QueueType queue, Fence fence, uint64_t value)
    {
        ROJORHI_INTERNAL_ASSERT(held.owns_lock() && held.mutex() == &m_mutex, "a fence wait was ordered without the tracker's lock");
        const auto found = m_fences.find(fence.bits);
        if (found == m_fences.end())
        {
            // No submission signalled the fence, so the wait names a value at most its initial one, which orders nothing.
            return;
        }
        const std::optional<Clock> reached = found->second.reached(value);
        if (reached.has_value())
        {
            Clock& waited = m_waited[static_cast<size_t>(queue)];
            waited = join(waited, *reached);
        }
    }

    //==================================================================================================================
    void Tracker::submitted([[maybe_unused]] const TrackerLock& held, QueueType queue)
    {
        ROJORHI_INTERNAL_ASSERT(held.owns_lock() && held.mutex() == &m_mutex, "a submission was noted without the tracker's lock");
        const size_t index = static_cast<size_t>(queue);
        m_submitted[index] = ++m_positions[index];
    }

    //==================================================================================================================
    void Tracker::mapTiles(const TrackerLock& held, QueueType queue, std::span<const TileChange> changes)
    {
        // The change is a queue operation after every earlier submission to the queue: the waits of those submissions, and what
        // the CPU observed, order the work of other queues before it.
        const Clock ordered = orderedBefore(queue);
        for (const TileChange& change : changes)
        {
            if (change.texture.bits == 0)
            {
                const BufferState* buffer = locate(held, change.buffer);
                if (buffer == nullptr || !buffer->sparse)
                {
                    continue;
                }
                const std::optional<TileRace> race = findRace(buffer->byteUses, change.offset, change.offset + change.size, queue, ordered);
                reportUseRace(
                    race,
                    race.has_value() ? std::format("bytes {} to {} of the buffer \"{}\"", change.offset, change.offset + change.size, buffer->label)
                                     : std::string(),
                    queue
                );
                continue;
            }
            const TextureState* texture = locate(held, change.texture);
            if (texture == nullptr || texture->tileUses.empty())
            {
                continue;
            }
            for (uint32_t mip = change.range.baseMip; mip < change.range.baseMip + change.range.mipCount; ++mip)
            {
                for (uint32_t layer = change.range.baseLayer; layer < change.range.baseLayer + change.range.layerCount; ++layer)
                {
                    const std::optional<TileRace> race =
                        findRace(texture->tileUses[static_cast<size_t>(mip) * texture->layers + layer], change.box, queue, ordered);
                    reportUseRace(
                        race, race.has_value() ? std::format("mip {} layer {} of the texture \"{}\"", mip, layer, texture->label) : std::string(), queue
                    );
                }
            }
        }
        // Every change of the call is one queue operation: one position, which a later use on another queue must be ordered after.
        const size_t index = static_cast<size_t>(queue);
        const uint64_t position = ++m_positions[index];
        for (const TileChange& change : changes)
        {
            if (change.texture.bits == 0)
            {
                BufferState* buffer = locate(held, change.buffer);
                if (buffer != nullptr && buffer->sparse)
                {
                    buffer->byteMaps[index].note(change.offset, change.offset + change.size, position, m_host[index]);
                    // A mapped range's first use must follow a global barrier; an unmapped one has no first use to order.
                    if (change.unmap)
                    {
                        buffer->newBytes.clear(change.offset, change.offset + change.size);
                    }
                    else
                    {
                        buffer->newBytes.mark(change.offset, change.offset + change.size, queue, position);
                    }
                }
                continue;
            }
            TextureState* texture = locate(held, change.texture);
            if (texture == nullptr || texture->tileMaps.empty())
            {
                continue;
            }
            for (uint32_t mip = change.range.baseMip; mip < change.range.baseMip + change.range.mipCount; ++mip)
            {
                for (uint32_t layer = change.range.baseLayer; layer < change.range.baseLayer + change.range.layerCount; ++layer)
                {
                    const size_t subresource = static_cast<size_t>(mip) * texture->layers + layer;
                    texture->tileMaps[subresource][index].note(change.box, position, m_host[index]);
                    // A mapped tile's first event must discard its subresource; an unmapped one has no first use to order.
                    if (change.unmap)
                    {
                        texture->newTiles[subresource].clear(change.box);
                    }
                    else
                    {
                        texture->newTiles[subresource].mark(change.box);
                    }
                }
            }
        }
    }

    //==================================================================================================================
    void Tracker::signal(const TrackerLock& held, QueueType queue, Fence fence, uint64_t initial, uint64_t value)
    {
        FenceClocks& clocks = m_fences.try_emplace(fence.bits, initial).first->second;
        clocks.signal(value, runningClock(held, queue));
        // Values the CPU has already seen complete order nothing more than the host clock; one stays as their floor.
        clocks.prune(m_host);
    }

    //==================================================================================================================
    void Tracker::observe([[maybe_unused]] const TrackerLock& held, Fence fence, uint64_t value)
    {
        ROJORHI_INTERNAL_ASSERT(held.owns_lock() && held.mutex() == &m_mutex, "a fence was observed without the tracker's lock");
        const auto found = m_fences.find(fence.bits);
        if (found == m_fences.end())
        {
            return;
        }
        const std::optional<Clock> reached = found->second.reached(value);
        if (reached.has_value())
        {
            m_host = join(m_host, *reached);
        }
    }

    //==================================================================================================================
    void Tracker::endFrame(const TrackerLock& held, uint64_t frame)
    {
        [[maybe_unused]] const std::optional<QueueType> open = unjoined(held);
        ROJORHI_VU(
            !open.has_value(),
            "VU-rojo-device-0006",
            "endFrame closes frame {}, but the submission to the {} queue at position {} is not ordered before the frame's graphics-queue "
            "signal: no graphics submission of the frame waits, directly or through other fences, on a value signalled by or after it",
            frame,
            queueName(*open),
            m_submitted[static_cast<size_t>(*open)]
        );
        m_frames.insert_or_assign(frame, runningClock(held, QueueType::Graphics));
    }

    //==================================================================================================================
    void Tracker::beginFrame([[maybe_unused]] const TrackerLock& held, uint64_t retired)
    {
        ROJORHI_INTERNAL_ASSERT(held.owns_lock() && held.mutex() == &m_mutex, "a frame was begun without the tracker's lock");
        m_frameStart = m_positions;
        const auto found = m_frames.find(retired);
        if (retired == 0 || found == m_frames.end())
        {
            return;
        }
        m_host = join(m_host, found->second);
        m_frames.erase(m_frames.begin(), std::next(found));
    }

    //==================================================================================================================
    std::optional<QueueType> Tracker::unjoined(const TrackerLock& held) const
    {
        // The graphics submissions of the frame join a compute or copy submission through the values they waited on, which the
        // graphics running clock gathers; the queue's later submissions of the frame cover its earlier ones in queue order.
        const Clock graphics = runningClock(held, QueueType::Graphics);
        for (const QueueType queue : { QueueType::Compute, QueueType::Copy })
        {
            const size_t index = static_cast<size_t>(queue);
            if (m_submitted[index] > m_frameStart[index] && !covers(graphics, queue, m_submitted[index]))
            {
                return queue;
            }
        }
        return std::nullopt;
    }

    //==================================================================================================================
    std::optional<QueueType> Tracker::unjoined()
    {
        const TrackerLock held = lock();
        return unjoined(held);
    }

    //==================================================================================================================
    void Tracker::removeFence(Fence fence)
    {
        const std::lock_guard held(m_mutex);
        m_fences.erase(fence.bits);
    }

    //==================================================================================================================
    Clock Tracker::runningClock([[maybe_unused]] const TrackerLock& held, QueueType queue) const
    {
        ROJORHI_INTERNAL_ASSERT(held.owns_lock() && held.mutex() == &m_mutex, "a running clock was read without the tracker's lock");
        // Every submission's clock holds the queue's earlier waits, so the join of them is the waits' join with the queue's
        // own position advanced to its last command.
        Clock running = m_waited[static_cast<size_t>(queue)];
        running[static_cast<size_t>(queue)] = m_positions[static_cast<size_t>(queue)];
        return running;
    }

    //==================================================================================================================
    Clock Tracker::runningClock(QueueType queue)
    {
        const TrackerLock held = lock();
        return runningClock(held, queue);
    }

    //==================================================================================================================
    Clock Tracker::hostClock([[maybe_unused]] const TrackerLock& held) const
    {
        ROJORHI_INTERNAL_ASSERT(held.owns_lock() && held.mutex() == &m_mutex, "the host clock was read without the tracker's lock");
        return m_host;
    }

    //==================================================================================================================
    Clock Tracker::hostClock()
    {
        const TrackerLock held = lock();
        return hostClock(held);
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
    Tracker::HeapState* Tracker::locate([[maybe_unused]] const TrackerLock& held, Heap heap)
    {
        ROJORHI_INTERNAL_ASSERT(held.owns_lock() && held.mutex() == &m_mutex, "a heap was looked up without the tracker's lock");
        const auto found = m_heaps.find(heap.bits);
        return found == m_heaps.end() ? nullptr : &found->second;
    }

    //==================================================================================================================
    std::string Tracker::describe(const TrackerLock& held, HeapOwner owner)
    {
        if (owner.texture)
        {
            const TextureState* texture = locate(held, Texture{ owner.bits });
            return texture == nullptr ? std::string("a destroyed texture") : std::format("the texture \"{}\"", texture->label);
        }
        const BufferState* buffer = locate(held, Buffer{ owner.bits });
        return buffer == nullptr ? std::string("a destroyed buffer") : std::format("the buffer \"{}\"", buffer->label);
    }

    //==================================================================================================================
    std::optional<Layout> Tracker::layoutOn(const LayoutEvent& event, QueueType queue, const Clock& ordered)
    {
        if (event.queue.has_value() && *event.queue != queue && !covers(ordered, *event.queue, event.position))
        {
            return std::nullopt;
        }
        return event.layout;
    }

    //==================================================================================================================
    std::optional<QueueType> Tracker::holderOf(const LayoutEvent& event)
    {
        return event.queue.has_value() ? event.queue : event.lastUse;
    }

    //==================================================================================================================
    bool Tracker::newOnCopy(const TextureState& texture, const LayoutEvent& event)
    {
        // An aliasing event has a queue and initial data leaves ShaderRead, so this is a creation nothing has reached since.
        return event.layout == Layout::Undefined && !event.queue.has_value() && !event.lastUse.has_value() && texture.heap.bits == 0 &&
               texture.tileUses.empty() && !texture.acquired;
    }

    //==================================================================================================================
    Clock Tracker::orderedBefore(QueueType queue) const
    {
        return join(m_waited[static_cast<size_t>(queue)], m_host);
    }

    //==================================================================================================================
    void Tracker::noteHeapUse(
        const TrackerLock& held,
        HeapState& heap,
        HeapOwner owner,
        uint64_t begin,
        uint64_t end,
        QueueType queue,
        uint64_t position,
        Scope scope,
        bool transition
    )
    {
        for (HeapSegments::Range& segment : heap.segments.isolate(begin, end))
        {
            core::noteHeapUse(segment.state, queue, owner, position, scope, transition);
        }
        heap.segments.merge(begin, end);
        // Every other texture over these bytes loses its contents: its next event on the queue discards (VU-rojo-sync-0017).
        for (const uint64_t bits : heap.textures)
        {
            TextureState* texture = locate(held, Texture{ bits });
            const bool other = !(owner.texture && owner.bits == bits);
            if (other && texture != nullptr && texture->heapOffset < end && begin < texture->heapOffset + texture->heapSize)
            {
                // Only the texture's own bytes are marked, so a use of other bytes keeps an older use of these.
                const uint64_t from = std::max(begin, texture->heapOffset);
                const uint64_t to = std::min(end, texture->heapOffset + texture->heapSize);
                for (std::array<AliasMarks, 3>& marks : texture->aliases)
                {
                    markAlias(marks[static_cast<size_t>(queue)], from, to, owner, position, scope);
                }
                // The use is a layout event of every subresource: a queue it is ordered before sees them Undefined (T6).
                for (LayoutEvent& event : texture->layouts)
                {
                    event = LayoutEvent{ .layout = Layout::Undefined, .queue = queue, .position = position };
                }
            }
        }
    }

    //==================================================================================================================
    void Tracker::checkHeapQueues(
        [[maybe_unused]] const TrackerLock& held,
        const HeapState& heap,
        HeapOwner owner,
        [[maybe_unused]] std::string_view resource,
        uint64_t begin,
        uint64_t end,
        QueueType queue,
        Scope scope,
        [[maybe_unused]] std::string_view list,
        bool transition
    )
    {
        const Clock ordered = orderedBefore(queue);
        const bool writes = transition || hasWrite(scope.access);
        for (const HeapSegments::Range& segment : heap.segments.ranges())
        {
            if (segment.end <= begin || end <= segment.begin)
            {
                continue;
            }
            for (size_t index = 0; index < segment.state.size(); ++index)
            {
                const QueueType other = static_cast<QueueType>(index);
                const std::optional<HeapUse>& last = segment.state[index];
                // The resource's own uses on other queues are judged by its own hazard state.
                [[maybe_unused]] const bool unordered = other != queue && last.has_value() && !(last->owner == owner) &&
                                                        (writes || last->use.transition || hasWrite(last->use.scope.access)) &&
                                                        !covers(ordered, other, last->use.position);
                ROJORHI_VU(
                    !unordered,
                    "VU-rojo-queue-0005",
                    "the list \"{}\" uses {} at {} on the {} queue over heap bytes {} to {}, which {} used at {} on the {} queue, and no fence "
                    "orders that use's submission before this one",
                    list,
                    resource,
                    scopeText(scope),
                    queueName(queue),
                    std::max(begin, segment.begin),
                    std::min(end, segment.end),
                    describe(held, last->owner),
                    scopeText(last->use.scope),
                    queueName(other)
                );
            }
        }
    }

    //==================================================================================================================
    void Tracker::checkPresent(const TrackerLock& held, Texture image)
    {
        const TextureState* texture = locate(held, image);
        if (texture == nullptr)
        {
            return;
        }
        // The image is presented on the graphics queue, which sees the layout its own work or a fence-ordered event set.
        [[maybe_unused]] const Clock ordered = orderedBefore(QueueType::Graphics);
        for (size_t index = 0; index < texture->layouts.size(); ++index)
        {
            [[maybe_unused]] const LayoutEvent& event = texture->layouts[index];
            ROJORHI_VU(
                event.layout == Layout::Present && layoutOn(event, QueueType::Graphics, ordered).has_value(),
                "VU-rojo-present-0018",
                "the swapchain image \"{}\" is presented with subresource {} in layout {}{}, not Present",
                texture->label,
                index,
                static_cast<int>(event.layout),
                layoutOn(event, QueueType::Graphics, ordered).has_value() ? "" : " set on another queue that no fence orders before the present"
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
        if (texture->heap.bits != 0)
        {
            // Another resource used the memory since a subresource's last event on the queue, so its contents are undefined and
            // its first event must be a discard; that rule reports before the layout rules of every subresource.
            for (uint32_t mip = use.range.baseMip; mip < mipEnd; ++mip)
            {
                for (uint32_t layer = use.range.baseLayer; layer < layerEnd; ++layer)
                {
                    [[maybe_unused]] const AliasMark* mark =
                        uncovered(texture->aliases[static_cast<size_t>(mip) * texture->layers + layer][static_cast<size_t>(queue)], 0, std::nullopt);
                    ROJORHI_VU(
                        mark == nullptr,
                        "VU-rojo-sync-0017",
                        "the list \"{}\" uses mip {} layer {} of the placed texture \"{}\" on the {} queue first since {} used its memory at {}, "
                        "with no texture barrier from Undefined before",
                        list,
                        mip,
                        layer,
                        texture->label,
                        queueName(queue),
                        mark != nullptr ? describe(held, mark->owner) : std::string(),
                        mark != nullptr ? scopeText(mark->scope) : std::string()
                    );
                }
            }
        }
        const Clock ordered = orderedBefore(queue);
        for (uint32_t mip = use.range.baseMip; mip < mipEnd; ++mip)
        {
            for (uint32_t layer = use.range.baseLayer; layer < layerEnd; ++layer)
            {
                LayoutEvent& event = texture->layouts[static_cast<size_t>(mip) * texture->layers + layer];
                if (queue == QueueType::Copy && newOnCopy(*texture, event))
                {
                    // A Copy list finds a new texture in General, and its first use leaves it there.
                    event.layout = Layout::General;
                }
                const std::optional<Layout> visible = layoutOn(event, queue, ordered);
                if (!visible.has_value())
                {
                    // No fence orders the latest layout event before the use: the hazard rules report the race below.
                    continue;
                }
                const Layout current = *visible;
                // A layout held by another queue reaches this one only in a layout both queues accept; that rule reports first,
                // then the Copy queue's rule, then the entry's own rule, then the general one. A creation or an acquire is held
                // by the queue of the latest use since.
                [[maybe_unused]] const std::optional<QueueType> holder = holderOf(event);
                if (!event.queue.has_value())
                {
                    event.lastUse = queue;
                }
                ROJORHI_VU(
                    !handedOver(holder, current, queue) || sharedLayout(current, queue, *holder),
                    "VU-rojo-sync-0025",
                    "the list \"{}\" on the {} queue uses mip {} layer {} of the texture \"{}\", handed over from the {} queue in layout {}, which "
                    "the two queues do not share",
                    list,
                    queueName(queue),
                    mip,
                    layer,
                    texture->label,
                    queueName(holder.value_or(queue)),
                    static_cast<int>(current)
                );
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
                case UseRule::Cmd0072:
                    ROJORHI_VU(
                        holds(use.layouts, current),
                        "VU-rojo-cmd-0072",
                        "the list \"{}\" clears mip {} layer {} of the texture \"{}\" in layout {}, not CopyDst",
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
        if (!texture->tileUses.empty())
        {
            // A change to the mapping of the tiles on another queue reports before the hazards against that queue's uses.
            for (uint32_t mip = use.range.baseMip; mip < mipEnd; ++mip)
            {
                for (uint32_t layer = use.range.baseLayer; layer < layerEnd; ++layer)
                {
                    replayTileUse(*texture, static_cast<size_t>(mip) * texture->layers + layer, use.box, queue, list, position, ordered);
                }
            }
        }
        replayTextureHazards(*texture, use, queue, list, position);
        if (!texture->newTiles.empty())
        {
            // A newly mapped tile's first event is a discard of its subresource; the cross-queue rules above report first.
            for (uint32_t mip = use.range.baseMip; mip < mipEnd; ++mip)
            {
                for (uint32_t layer = use.range.baseLayer; layer < layerEnd; ++layer)
                {
                    ROJORHI_VU(
                        !texture->newTiles[static_cast<size_t>(mip) * texture->layers + layer].reaches(use.box),
                        "VU-rojo-sparse-0014",
                        "the list \"{}\" on the {} queue uses tiles of mip {} layer {} of the sparse texture \"{}\" that were mapped onto heap "
                        "memory since its last texture barrier from Undefined; the first use of a newly mapped tile is that discard",
                        list,
                        queueName(queue),
                        mip,
                        layer,
                        texture->label
                    );
                }
            }
        }
        if (texture->heap.bits != 0)
        {
            // Only the subresources the use names follow the marks before it; the others still owe their discard.
            for (uint32_t mip = use.range.baseMip; mip < mipEnd; ++mip)
            {
                for (uint32_t layer = use.range.baseLayer; layer < layerEnd; ++layer)
                {
                    followMarks(*texture, static_cast<size_t>(mip) * texture->layers + layer, queue, ordered);
                }
            }
            // A use of any subresource is a use of the texture's whole heap range: its subresources' bytes are not known.
            HeapState* heap = locate(held, texture->heap);
            ROJORHI_INTERNAL_ASSERT(heap != nullptr, "a placed texture is followed without its heap");
            const HeapOwner self{ .bits = use.texture.bits, .texture = true };
            const uint64_t begin = texture->heapOffset;
            const uint64_t end = texture->heapOffset + texture->heapSize;
            checkHeapQueues(held, *heap, self, std::format("the placed texture \"{}\"", texture->label), begin, end, queue, scopeOf(use), list);
            noteHeapUse(held, *heap, self, begin, end, queue, position, scopeOf(use));
        }
    }

    //==================================================================================================================
    void Tracker::followMarks(TextureState& texture, size_t index, QueueType queue, const Clock& ordered)
    {
        std::array<AliasMarks, 3>& marks = texture.aliases[index];
        marks[static_cast<size_t>(queue)] = AliasMarks{};
        for (size_t other = 0; other < marks.size(); ++other)
        {
            if (other != static_cast<size_t>(queue))
            {
                clearCovered(marks[other], texture.heapOffset, texture.heapOffset + texture.heapSize, static_cast<QueueType>(other), ordered);
            }
        }
    }

    //==================================================================================================================
    void Tracker::replayTextureHazards(TextureState& texture, const UseEvent& use, QueueType queue, std::string_view list, uint64_t position)
    {
        const Scope later{ .stage = use.stage, .access = use.access };
        const uint32_t mipEnd = use.range.baseMip + resolvedCount(use.range.baseMip, use.range.mipCount, texture.mips);
        const uint32_t layerEnd = use.range.baseLayer + resolvedCount(use.range.baseLayer, use.range.layerCount, texture.layers);
        const Clock ordered = orderedBefore(queue);
        const uint64_t fenced = ordered[static_cast<size_t>(queue)];
        for (uint32_t mip = use.range.baseMip; mip < mipEnd; ++mip)
        {
            for (uint32_t layer = use.range.baseLayer; layer < layerEnd; ++layer)
            {
                UnitHazards& unit = texture.hazards[static_cast<size_t>(mip) * texture.layers + layer];
                QueueHazards& state = unit.on(queue);
                const Hazard hazard = checkUse(state, position, later, fenced);
                const std::optional<QueueHazard> across = checkOtherQueues(unit, queue, later, ordered);
                if (hazard.kind != HazardKind::None || across.has_value())
                {
                    const std::string memory = std::format("mip {} layer {} of the texture \"{}\"", mip, layer, texture.label);
                    reportHazard(hazard, later, memory, queue, list);
                    reportQueueHazard(across, later, memory, queue, list);
                }
                recordUse(state, position, later, fenced);
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
        // The aliasing rule reports first, then the indirect entry's own rule, then the hazards.
        if (buffer->heap.bits != 0)
        {
            replayBufferAlias(held, *buffer, use, queue, list, position);
        }
        if (use.rule != UseRule::None)
        {
            replayIndirect(held, *buffer, use, queue, list);
        }
        const Scope later = scopeOf(use);
        const uint64_t end = rangeEnd(use.offset, use.size, buffer->size);
        const Clock ordered = orderedBefore(queue);
        const uint64_t fenced = ordered[static_cast<size_t>(queue)];
        if (buffer->sparse)
        {
            // A change to the mapping of the tiles on another queue reports before the hazards against that queue's uses.
            const std::optional<TileRace> race = findRace(buffer->byteMaps, use.offset, end, queue, ordered);
            reportMappingRace(
                race, race.has_value() ? std::format("bytes {} to {} of the buffer \"{}\"", use.offset, end, buffer->label) : std::string(), queue, list
            );
            buffer->byteUses[static_cast<size_t>(queue)].note(use.offset, end, position, m_host[static_cast<size_t>(queue)]);
        }
        for (ByteSegment& segment : buffer->segments.isolate(use.offset, end))
        {
            QueueHazards& state = segment.hazards.on(queue);
            const Hazard hazard = checkUse(state, position, later, fenced);
            const std::optional<QueueHazard> across = checkOtherQueues(segment.hazards, queue, later, ordered);
            if (hazard.kind != HazardKind::None || across.has_value())
            {
                const std::string memory = std::format("bytes {} to {} of the buffer \"{}\"", segment.begin, segment.end, buffer->label);
                reportHazard(hazard, later, memory, queue, list);
                reportQueueHazard(across, later, memory, queue, list);
            }
            recordUse(state, position, later, fenced);
        }
        buffer->segments.merge(use.offset, end, mergeFloor(queue));
        if (buffer->sparse)
        {
            // A newly mapped range's first use follows a global barrier on its queue; the cross-queue rules above report first.
            [[maybe_unused]] const std::optional<ByteMark> mark = buffer->newBytes.firstUse(use.offset, end, queue, later);
            ROJORHI_VU(
                !mark.has_value(),
                "VU-rojo-sparse-0014",
                "the list \"{}\" uses bytes {} to {} of the sparse buffer \"{}\" at {} on the {} queue first since mapTiles mapped them on the {} "
                "queue, and no global barrier on this queue since the mapping has a destination that covers the use",
                list,
                use.offset,
                end,
                buffer->label,
                scopeText(later),
                queueName(queue),
                queueName(mark->queue)
            );
        }
    }

    //==================================================================================================================
    void Tracker::replayBufferAlias(
        const TrackerLock& held, BufferState& buffer, const UseEvent& use, QueueType queue, [[maybe_unused]] std::string_view list, uint64_t position
    )
    {
        HeapState* heap = locate(held, buffer.heap);
        ROJORHI_INTERNAL_ASSERT(heap != nullptr, "a placed buffer is followed without its heap");
        const HeapOwner self{ .bits = use.buffer.bits, .texture = false };
        const Scope later = scopeOf(use);
        const uint64_t begin = buffer.heapOffset + use.offset;
        const uint64_t end = buffer.heapOffset + rangeEnd(use.offset, use.size, buffer.size);
        const Clock ordered = orderedBefore(queue);
        // The buffer's own bytes that pass against another resource's use: the aliasing chain orders the buffer's own earlier
        // uses before this one, and their contents are undefined, so their own hazard state on the queue starts afresh.
        std::vector<std::pair<uint64_t, uint64_t>> fresh;
        for (const HeapSegments::Range& segment : heap->segments.isolate(begin, end))
        {
            const std::optional<HeapUse>& other = segment.state[static_cast<size_t>(queue)];
            // A fence wait or the CPU's observation orders the other use before this one with full visibility, as the barrier
            // would.
            if (!other.has_value() || other->owner == self || covers(ordered, queue, other->use.position))
            {
                continue;
            }
            ROJORHI_VU(
                aliasOrdered(*other, later),
                "VU-rojo-sync-0009",
                "the list \"{}\" uses bytes {} to {} of the placed buffer \"{}\" at {} on the {} queue first since {} used those heap bytes at {}, "
                "and no global barrier since covers that use at its source and this one at its destination",
                list,
                segment.begin - buffer.heapOffset,
                segment.end - buffer.heapOffset,
                buffer.label,
                scopeText(later),
                queueName(queue),
                describe(held, other->owner),
                scopeText(other->use.scope)
            );
            fresh.emplace_back(segment.begin - buffer.heapOffset, segment.end - buffer.heapOffset);
        }
        for (const auto& [from, to] : fresh)
        {
            for (ByteSegment& segment : buffer.segments.isolate(from, to))
            {
                segment.hazards.on(queue) = QueueHazards{};
            }
            buffer.segments.merge(from, to, mergeFloor(queue));
        }
        checkHeapQueues(held, *heap, self, std::format("the placed buffer \"{}\"", buffer.label), begin, end, queue, later, list);
        noteHeapUse(held, *heap, self, begin, end, queue, position, later);
    }

    //==================================================================================================================
    void Tracker::replayTileUse(
        TextureState& texture,
        size_t index,
        const std::optional<TexelBox>& box,
        QueueType queue,
        [[maybe_unused]] std::string_view list,
        uint64_t position,
        const Clock& ordered
    )
    {
        if (texture.tileUses.empty())
        {
            return;
        }
        const std::optional<TileRace> race = findRace(texture.tileMaps[index], box, queue, ordered);
        reportMappingRace(
            race,
            race.has_value() ? std::format("mip {} layer {} of the texture \"{}\"", index / texture.layers, index % texture.layers, texture.label)
                             : std::string(),
            queue,
            list
        );
        texture.tileUses[index][static_cast<size_t>(queue)].note(box, position, m_host[static_cast<size_t>(queue)]);
    }

    //==================================================================================================================
    void Tracker::replayIndirect(const TrackerLock& held, const BufferState& buffer, const UseEvent& use, QueueType queue, std::string_view list)
    {
        const uint64_t end = rangeEnd(use.offset, use.size, buffer.size);
        const WriteOrder order = lastWriteOrder(held, queue, use.buffer, use.offset, use.size, scopeOf(use));
        reportIndirect(use.rule, order == WriteOrder::Missing, std::format("bytes {} to {} of the buffer \"{}\"", use.offset, end, buffer.label), queue, list);
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
        // A global barrier alone orders a placed buffer's first use after another resource's (VU-rojo-sync-0009).
        for (auto& [bits, heap] : m_heaps)
        {
            for (HeapSegments::Range& segment : heap.segments.ranges())
            {
                reachHeapUse(segment.state, queue, position, source, destination);
            }
        }
        // It alone orders a sparse buffer's first use of newly mapped bytes after the mapping (VU-rojo-sparse-0014).
        const Clock ordered = orderedBefore(queue);
        for (auto& [bits, buffer] : m_buffers)
        {
            if (buffer.sparse)
            {
                buffer.newBytes.reach(queue, ordered, destination);
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
        const Scope source{ .stage = barrier.srcStage, .access = barrier.srcAccess };
        const Clock ordered = orderedBefore(queue);
        const uint64_t fenced = ordered[static_cast<size_t>(queue)];
        // A barrier that changes the layout, or discards, writes the subresources it names.
        const bool transition = barrier.oldLayout != barrier.newLayout || barrier.oldLayout == Layout::Undefined;
        if (texture.heap.bits != 0)
        {
            // The first event since another resource used the memory discards, waiting for that use; the rule reports before
            // the layout and coverage rules of every subresource.
            for (uint32_t mip = barrier.range.baseMip; mip < mipEnd; ++mip)
            {
                for (uint32_t layer = barrier.range.baseLayer; layer < layerEnd; ++layer)
                {
                    const size_t index = static_cast<size_t>(mip) * texture.layers + layer;
                    std::array<AliasMarks, 3>& marks = texture.aliases[index];
                    const bool discards = barrier.oldLayout == Layout::Undefined;
                    // Each byte range's latest foreign use is waited for, so an older use of other bytes is not lost.
                    const bool pending = uncovered(marks[static_cast<size_t>(queue)], fenced, std::nullopt) != nullptr;
                    [[maybe_unused]] const AliasMark* mark =
                        uncovered(marks[static_cast<size_t>(queue)], fenced, discards ? std::optional<Scope>(source) : std::nullopt);
                    ROJORHI_VU(
                        mark == nullptr,
                        "VU-rojo-sync-0017",
                        "a texture barrier in the list \"{}\" on the {} queue moves mip {} layer {} of the placed texture \"{}\" from layout {} with source {} "
                        "first since {} used its memory at {}; it must start from Undefined with a source that covers that use",
                        list,
                        queueName(queue),
                        mip,
                        layer,
                        texture.label,
                        static_cast<int>(barrier.oldLayout),
                        scopeText(source),
                        mark != nullptr ? describe(held, mark->owner) : std::string(),
                        mark != nullptr ? scopeText(mark->scope) : std::string()
                    );
                    if (pending)
                    {
                        // The aliasing chain orders the texture's own earlier uses before the discard, and their contents are
                        // undefined, so its own hazard state on the queue starts afresh.
                        texture.hazards[index].on(queue) = QueueHazards{};
                    }
                    followMarks(texture, index, queue, ordered);
                }
            }
        }
        for (uint32_t mip = barrier.range.baseMip; mip < mipEnd; ++mip)
        {
            for (uint32_t layer = barrier.range.baseLayer; layer < layerEnd; ++layer)
            {
                const size_t index = static_cast<size_t>(mip) * texture.layers + layer;
                LayoutEvent& event = texture.layouts[index];
                if (queue == QueueType::Copy && newOnCopy(texture, event))
                {
                    // A Copy list finds a new texture in General, and its first barrier, which keeps General
                    // (VU-rojo-sync-0026), leaves it there. An acquired image no longer counts as acquired here, but on a Copy
                    // list its first barrier starts from General and VU-rojo-present-0014 has reported it above.
                    event.layout = Layout::General;
                }
                // With no fence ordering the latest layout event before the barrier, the race is reported below, not a layout.
                const std::optional<Layout> visible = layoutOn(event, queue, ordered);
                // A creation or an acquire is held by the queue of the latest use or barrier since, which hands it over.
                const std::optional<QueueType> holder = holderOf(event);
                if (!event.queue.has_value())
                {
                    event.lastUse = queue;
                }
                if (visible.has_value())
                {
                    if (handedOver(holder, *visible, queue))
                    {
                        // The first barrier of the queue the subresource is handed to reports the handoff rule before sync-0013;
                        // a discard takes nothing over, so the layout need not be one the two queues share.
                        ROJORHI_VU(
                            barrier.oldLayout == Layout::Undefined || sharedLayout(*visible, queue, *holder),
                            "VU-rojo-sync-0025",
                            "a texture barrier in the list \"{}\" on the {} queue takes mip {} layer {} of the texture \"{}\", handed over from the {} "
                            "queue in layout {}, which the two queues do not share",
                            list,
                            queueName(queue),
                            mip,
                            layer,
                            texture.label,
                            queueName(*holder),
                            static_cast<int>(*visible)
                        );
                        ROJORHI_VU(
                            barrier.oldLayout == Layout::Undefined || barrier.oldLayout == *visible,
                            "VU-rojo-sync-0025",
                            "the first texture barrier in the list \"{}\" on the {} queue on mip {} layer {} of the texture \"{}\", handed over from the "
                            "{} queue in layout {}, starts from layout {}; it must start from that layout or discard from Undefined",
                            list,
                            queueName(queue),
                            mip,
                            layer,
                            texture.label,
                            queueName(*holder),
                            static_cast<int>(*visible),
                            static_cast<int>(barrier.oldLayout)
                        );
                    }
                    ROJORHI_VU(
                        barrier.oldLayout == Layout::Undefined || barrier.oldLayout == *visible,
                        "VU-rojo-sync-0013",
                        "a texture barrier in the list \"{}\" starts mip {} layer {} of the texture \"{}\" from layout {}, but it is in layout {}",
                        list,
                        mip,
                        layer,
                        texture.label,
                        static_cast<int>(barrier.oldLayout),
                        static_cast<int>(*visible)
                    );
                }
                if (transition)
                {
                    event = LayoutEvent{ .layout = barrier.newLayout, .queue = queue, .position = position };
                }
                const Scope destination{ .stage = barrier.dstStage, .access = barrier.dstAccess };
                QueueHazards& state = texture.hazards[index].on(queue);
                if (transition)
                {
                    // The transition writes every tile of the subresource: a change to their mapping on another queue reports first.
                    replayTileUse(texture, index, std::nullopt, queue, list, position, ordered);
                    // The transition writes the subresource on its queue, so a use on another queue no fence orders before it races it.
                    [[maybe_unused]] const std::optional<QueueHazard> across = checkOtherQueues(texture.hazards[index], queue, destination, ordered, true);
                    ROJORHI_VU(
                        !across.has_value(),
                        "VU-rojo-queue-0005",
                        "a texture barrier in the list \"{}\" on the {} queue moves mip {} layer {} of the texture \"{}\" from layout {} to {} after {} on "
                        "the {} queue, and no fence orders the earlier use's submission before this one",
                        list,
                        queueName(queue),
                        mip,
                        layer,
                        texture.label,
                        static_cast<int>(barrier.oldLayout),
                        static_cast<int>(barrier.newLayout),
                        earlierUse(Hazard{ .earlier = across->earlier, .earlierTransition = across->earlierTransition }),
                        queueName(across->queue)
                    );
                }
                if (!texture.newTiles.empty())
                {
                    // A discard is the first event a newly mapped tile needs, and clears its marks; any other barrier reaches them.
                    if (barrier.oldLayout == Layout::Undefined)
                    {
                        texture.newTiles[index].clear(std::nullopt);
                    }
                    ROJORHI_VU(
                        !texture.newTiles[index].reaches(std::nullopt),
                        "VU-rojo-sparse-0014",
                        "a texture barrier in the list \"{}\" on the {} queue moves mip {} layer {} of the sparse texture \"{}\" from layout {}, "
                        "and tiles of it were mapped onto heap memory since its last texture barrier from Undefined; the first use of a newly "
                        "mapped tile is that discard",
                        list,
                        queueName(queue),
                        mip,
                        layer,
                        texture.label,
                        static_cast<int>(barrier.oldLayout)
                    );
                }
                if (barrier.oldLayout == barrier.newLayout)
                {
                    recordBarrier(state, position, source, destination);
                    continue;
                }
                // A transition stands between the uses around it as a write would, so its own source covers the uses before it.
                [[maybe_unused]] const Hazard hazard = checkTransition(state, position, source, barrier.oldLayout == Layout::Undefined, fenced);
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
        if (transition && texture.heap.bits != 0)
        {
            // A transition writes the texture's memory, compression metadata included: another resource's use of those bytes on
            // another queue must be ordered before it, and it is a heap use another resource's later use must follow.
            HeapState* heap = locate(held, texture.heap);
            ROJORHI_INTERNAL_ASSERT(heap != nullptr, "a placed texture is followed without its heap");
            const HeapOwner self{ .bits = barrier.texture.bits, .texture = true };
            const Scope destination{ .stage = barrier.dstStage, .access = barrier.dstAccess };
            const uint64_t begin = texture.heapOffset;
            const uint64_t end = texture.heapOffset + texture.heapSize;
            checkHeapQueues(held, *heap, self, std::format("the placed texture \"{}\"", texture.label), begin, end, queue, destination, list, true);
            noteHeapUse(held, *heap, self, begin, end, queue, position, destination, true);
        }
    }

    //==================================================================================================================
    TrackerLock lockTrackerChecked(DeviceCore& core)
    {
        return core.validation() ? core.tracker().lock() : TrackerLock{};
    }

    //==================================================================================================================
    void replayListsChecked(
        DeviceCore& core,
        QueueType type,
        std::span<const CommandList> lists,
        std::span<const FenceValue> wait,
        std::span<const FenceValue> signal,
        const TrackerLock& held
    )
    {
        if (!core.validation())
        {
            return;
        }
        Tracker& tracker = core.tracker();
        for (const FenceValue& entry : wait)
        {
            tracker.wait(held, type, entry.fence, entry.value);
        }
        for (const CommandList& handle : lists)
        {
            const CommandListCore& list = coreOf(handle);
            tracker.replay(held, list.trackerLog, type, list.label);
        }
        tracker.submitted(held, type);
        for (const FenceValue& entry : signal)
        {
            // Under the tracker's lock a submission takes the fences' pool lock, never the other way round.
            uint64_t initial = 0;
            core.pool<Fence>().update(entry.fence, [&](FenceRecord& record) { initial = record.initial; });
            tracker.signal(held, type, entry.fence, initial, entry.value);
        }
    }

    //==================================================================================================================
    void observeFenceChecked(DeviceCore& core, Fence fence, uint64_t value)
    {
        if (core.validation())
        {
            const TrackerLock held = core.tracker().lock();
            core.tracker().observe(held, fence, value);
        }
    }

    //==================================================================================================================
    void endFrameClockChecked(DeviceCore& core, uint64_t frame)
    {
        if (core.validation())
        {
            const TrackerLock held = core.tracker().lock();
            core.tracker().endFrame(held, frame);
        }
    }

    //==================================================================================================================
    void beginFrameClockChecked(DeviceCore& core, uint64_t retired)
    {
        if (core.validation())
        {
            const TrackerLock held = core.tracker().lock();
            core.tracker().beginFrame(held, retired);
        }
    }

    //==================================================================================================================
    void mapTileChangesChecked(DeviceCore& core, QueueType queue, std::span<const TileChange> changes)
    {
        if (core.validation())
        {
            const TrackerLock held = core.tracker().lock();
            core.tracker().mapTiles(held, queue, changes);
        }
    }

    //==================================================================================================================
    void forgetFenceChecked(DeviceCore& core, Fence fence)
    {
        if (core.validation())
        {
            core.tracker().removeFence(fence);
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
    void trackTextureChecked(
        DeviceCore& core,
        Texture texture,
        std::string_view label,
        uint32_t mips,
        uint32_t layers,
        Layout initial,
        bool acquired,
        Heap heap,
        uint64_t heapOffset,
        uint64_t heapSize,
        bool sparse
    )
    {
        if (core.validation())
        {
            core.tracker().addTexture(texture, label, mips, layers, initial, acquired, heap, heapOffset, heapSize, sparse);
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
    void trackBufferChecked(DeviceCore& core, Buffer buffer, std::string_view label, uint64_t size, Heap heap, uint64_t heapOffset, bool sparse)
    {
        if (core.validation())
        {
            core.tracker().addBuffer(buffer, label, size, heap, heapOffset, sparse);
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

    //==================================================================================================================
    void forgetHeapChecked(DeviceCore& core, Heap heap)
    {
        if (core.validation())
        {
            core.tracker().removeHeap(heap);
        }
    }
} // namespace rojoRHI::core
