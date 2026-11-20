//----------------------------------------------------------------------------------------------------------------------
/// @file Records.h
/// @brief Declares the record each handle type's pool stores and the table of hooks a backend registers to release them.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "Core/ScopeRecord.h"

#include <atomic>
#include <compare>
#include <cstdint>
#include <map>
#include <string>
#include <type_traits>
#include <vector>

namespace rojoRHI::core {
    // In tile units. The mip tail of a layer is the key with `mip` set to `kTailMip` and the rest zero; a buffer tile
    // is the key with its index in `x`.
    struct TileKey
    {
        static constexpr uint32_t kTailMip = UINT32_MAX;

        uint32_t mip = 0;
        uint32_t layer = 0;
        uint32_t x = 0;
        uint32_t y = 0;
        uint32_t z = 0;

        friend auto operator<=>(const TileKey&, const TileKey&) = default;
    };

    using TileMap = std::map<TileKey, Heap>;

    // Unsubmitted lists that reference an object, each counted once, apart by how the list names it. Only Debug builds
    // on a validating device write it, under the pool's lock (VU-rojo-device-0007).
    struct ListReferences
    {
        uint32_t roots = 0; // VU-rojo-abi-0003; buffers only.
        uint32_t addrs = 0; // VU-rojo-buffer-0019; buffers only.
        uint32_t others = 0;
    };

    struct BufferRecord
    {
        std::string label;
        uint64_t size = 0; // Bytes.
        uint64_t initialBytes = 0;
        Memory memory = Memory::Gpu;
        BufferUsage usage = BufferUsage::None;
        Heap heap = {}; // Null for a dedicated allocation.
        uint64_t heapOffset = 0;
        // A frame-arena page's latest claiming frame; zero otherwise and in Release, since only Debug-tier code reads
        // it (VU-rojo-buffer-0020, the tracker's references). It changes under the pool's lock when a list claims the
        // page.
        uint64_t arenaFrame = 0;
        ListReferences refs = {};
        TileMap mapped = {}; // Sparse buffers only (VU-rojo-sparse-0012).
        void* native = nullptr;
    };

    struct TextureRecord
    {
        std::string label;
        TextureType type = TextureType::Tex2D;
        Format format = Format::Undefined;
        Extent3D extent = {}; // Texels of mip 0.
        uint32_t mips = 1;
        uint32_t layers = 1; // Faces count as layers for cube types.
        uint32_t samples = 1;
        TextureUsage usage = TextureUsage::None;
        Heap heap = {}; // Null for a dedicated allocation.
        uint64_t heapOffset = 0;
        uint64_t heapSize = 0; // The placement requirement.
        uint64_t initialBytes = 0;
        bool initialData = false;    // Starts in `Layout::ShaderRead`.
        uint64_t views = 0;          // View slots holding a view of this texture.
        bool swapchainImage = false; // Returned by `Swapchain::acquire` (VU-rojo-present-0012, 0013, VU-rojo-sync-0005).
        ListReferences refs = {};
        TileMap mapped = {}; // Sparse textures only.
        // Measured by the backend at creation; `bytes` zero when the core computes it from the registered tile extents.
        // Set before `allocate` and never changed while the texture lives: Release reads it without the pool's lock
        // (ADR 0041).
        TileShape tiles = {};
        void* native = nullptr;
    };

    enum class ShaderStage : uint8_t
    {
        Vertex,
        Fragment,
        Task,
        Mesh,
        Compute
    };

    struct ShaderEntryRecord
    {
        std::string name;
        ShaderStage stage = ShaderStage::Vertex;
    };

    struct ShaderLibraryRecord
    {
        std::string label;
        std::vector<ShaderEntryRecord> entries;
        void* native = nullptr;
    };

    enum class PipelineKind : uint8_t
    {
        Graphics,
        Compute
    };

    struct PipelineRecord
    {
        std::string label;
        PipelineKind kind = PipelineKind::Graphics;
        uint32_t stages = 0;                                 // Bit `1 << ShaderStage` per stage present.
        PrimitiveClass primitive = PrimitiveClass::Triangle; // Meaningful for a vertex pipeline only.
        std::vector<Format> colors;
        Format depthFormat = Format::Undefined; // `Undefined` for none.
        uint32_t samples = 1;
        bool depthBias = false;
        uint32_t blendMask = 0; // Bit `i` is set when color target `i` blends.
        bool depthWrite = false;
        bool stencilWrite = false; // Some stencil face operation is not `Keep`.
        ListReferences refs = {};
        void* native = nullptr;
    };

    struct HeapRecord
    {
        std::string label;
        uint64_t size = 0; // At least the size requested.
        Memory memory = Memory::Gpu;
        uint64_t placedResources = 0; // Resources created in the heap and not yet destroyed.
        uint64_t mappedTiles = 0;     // Tiles of sparse resources currently mapped from the heap.
        void* native = nullptr;
    };

    struct FenceRecord
    {
        std::string label;
        uint64_t initial = 0;
        uint64_t submitted = 0; // `initial` until a signal is submitted (VU-rojo-queue-0003).
        uint64_t completed = 0; // A backend raises it as work completes.
        void* native = nullptr;
    };

    // Updated without a lock; 0 means none, since frames are numbered from one. It copies by value so a record can be
    // moved into a pool. Only Debug builds touch it (VU-rojo-upscale-0008).
    struct FrameStamp
    {
        std::atomic<uint64_t> frame{ 0 };

        FrameStamp() = default;

        FrameStamp(const FrameStamp& other) : frame(other.frame.load(std::memory_order_relaxed))
        {
        }

        FrameStamp& operator=(const FrameStamp& other)
        {
            frame.store(other.frame.load(std::memory_order_relaxed), std::memory_order_relaxed);
            return *this;
        }
    };

    struct UpscalerRecord
    {
        std::string label;
        UpscalerKind kind = UpscalerKind::MetalFX; // Never `Auto`.
        Extent2D input = {};                       // Allocated extent of every input.
        Extent2D output = {};
        Format color = Format::RGBA16Float;
        Format depth = Format::D32Float;
        Format motion = Format::RG16Float;
        Format outputFormat = Format::RGBA16Float;
        UpscalerFlags flags = UpscalerFlags::None;
        FrameStamp lastFrame; // The frame of the latest upscale.
        ListReferences refs = {};
        void* native = nullptr;
    };

    template<typename Handle> struct RecordOf;

    template<> struct RecordOf<Buffer>
    {
        using Type = BufferRecord;
    };
    template<> struct RecordOf<Texture>
    {
        using Type = TextureRecord;
    };
    template<> struct RecordOf<Pipeline>
    {
        using Type = PipelineRecord;
    };
    template<> struct RecordOf<ShaderLibrary>
    {
        using Type = ShaderLibraryRecord;
    };
    template<> struct RecordOf<Heap>
    {
        using Type = HeapRecord;
    };
    template<> struct RecordOf<Fence>
    {
        using Type = FenceRecord;
    };
    template<> struct RecordOf<Upscaler>
    {
        using Type = UpscalerRecord;
    };

    template<typename Handle> using RecordFor = typename RecordOf<Handle>::Type;

    template<typename Record> using RetireFn = void (*)(void* context, Record& record);

    // A null entry means the backend holds nothing for that type. A hook runs under its pool's lock and must not call
    // the core's pools. Each record's `native` is the backend's payload; the core never reads it.
    struct RetireHooks
    {
        void* context = nullptr; // The backend's device state.
        RetireFn<BufferRecord> buffer = nullptr;
        RetireFn<TextureRecord> texture = nullptr;
        RetireFn<PipelineRecord> pipeline = nullptr;
        RetireFn<ShaderLibraryRecord> shaderLibrary = nullptr;
        RetireFn<HeapRecord> heap = nullptr;
        RetireFn<FenceRecord> fence = nullptr;
        RetireFn<UpscalerRecord> upscaler = nullptr;
        // Called when `beginFrame` reuses `slot`, after the slot's pools retired, so the backend can return what it
        // pooled for that slot.
        void (*frameSlot)(void* context, uint32_t slot) = nullptr;
        // Once per non-null payload of a view slot whose release or replacement retired, or, at device destruction,
        // still held or waiting.
        void (*view)(void* context, void* native) = nullptr;
        // As `view`, for sampler slots.
        void (*sampler)(void* context, void* native) = nullptr;
        // Blocks until frame `frame`'s work is complete; `beginFrame` calls it, with the frame whose slot it reuses,
        // before it retires anything. Null when the backend completes work at submission.
        void (*waitFrame)(void* context, uint64_t frame) = nullptr;
        // Fills the durations of the scopes of the frame `beginFrame` retires, once per queue type that has scopes. It
        // runs after the frame wait, under the scope log's lock, before the slot's arena is rewound and before
        // `frameSlot`; it must not call the core, and its spans are valid only during the call. Null leaves every
        // duration zero.
        ScopeTimesFn scopeTimes = nullptr;

        template<typename Handle> RetireFn<RecordFor<Handle>> forType() const
        {
            if constexpr (std::is_same_v<Handle, Buffer>)
            {
                return buffer;
            }
            else if constexpr (std::is_same_v<Handle, Texture>)
            {
                return texture;
            }
            else if constexpr (std::is_same_v<Handle, Pipeline>)
            {
                return pipeline;
            }
            else if constexpr (std::is_same_v<Handle, ShaderLibrary>)
            {
                return shaderLibrary;
            }
            else if constexpr (std::is_same_v<Handle, Heap>)
            {
                return heap;
            }
            else if constexpr (std::is_same_v<Handle, Fence>)
            {
                return fence;
            }
            else
            {
                static_assert(std::is_same_v<Handle, Upscaler>, "Handle is not a pooled handle type");
                return upscaler;
            }
        }
    };
} // namespace rojoRHI::core
