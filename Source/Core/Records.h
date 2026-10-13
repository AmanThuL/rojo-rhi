//----------------------------------------------------------------------------------------------------------------------
/// @file Records.h
/// @brief Declares the record each handle type's pool stores and the table of hooks a backend registers to release them.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/BufferUsage.h>
#include <rojoRHI/Format.h>
#include <rojoRHI/Geometry.h>
#include <rojoRHI/Handles.h>
#include <rojoRHI/Memory.h>
#include <rojoRHI/PrimitiveClass.h>
#include <rojoRHI/TextureType.h>
#include <rojoRHI/TextureUsage.h>
#include <rojoRHI/UpscalerFlags.h>
#include <rojoRHI/UpscalerKind.h>

#include <atomic>
#include <compare>
#include <cstdint>
#include <map>
#include <string>
#include <type_traits>
#include <vector>

namespace rojoRHI::core {
    /// Names one tile of a sparse resource. A texture tile is the tile at (`x`, `y`, `z`) of level `mip` in layer `layer`, in
    /// tile units; the mip tail of a layer is the key with `mip` set to `kTailMip` and the rest zero. A buffer tile is the key
    /// with its index in `x`.
    struct TileKey
    {
        /// The `mip` of a mip tail key.
        static constexpr uint32_t kTailMip = UINT32_MAX;

        uint32_t mip = 0;   ///< Mip level, or `kTailMip`.
        uint32_t layer = 0; ///< Array layer.
        uint32_t x = 0;     ///< Tile column, or a buffer tile's index.
        uint32_t y = 0;     ///< Tile row.
        uint32_t z = 0;     ///< Tile slice.

        /// Orders keys so a record can keep them in a map.
        friend auto operator<=>(const TileKey&, const TileKey&) = default;
    };

    /// The tiles of a sparse resource that are mapped, each with the heap that supplies it.
    using TileMap = std::map<TileKey, Heap>;

    /// What the core keeps about one live Buffer: its description; later parts add its state.
    struct BufferRecord
    {
        std::string label;                     ///< The label the object was created with.
        uint64_t size = 0;                     ///< Bytes.
        uint64_t initialBytes = 0;             ///< Length of the initial data the buffer was created with.
        Memory memory = Memory::Gpu;           ///< The memory class.
        BufferUsage usage = BufferUsage::None; ///< The usage flags.
        Heap heap = {};                        ///< The heap the buffer is placed in; null for a dedicated allocation.
        /// For a frame-arena page, the frame of the list that claimed it last; zero for any other buffer. It changes under
        /// the pool's lock when a list claims the page (VU-rojo-buffer-0020).
        uint64_t arenaFrame = 0;
        TileMap mapped = {};    ///< The mapped tiles of a Sparse buffer (VU-rojo-sparse-0012); empty for any other buffer.
        void* native = nullptr; ///< The backend's payload for the object; the core never reads it.
    };

    /// What the core keeps about one live Texture: its description and how many view slots hold a view of it; later parts add
    /// its state.
    struct TextureRecord
    {
        std::string label;                       ///< The label the object was created with.
        TextureType type = TextureType::Tex2D;   ///< The dimensionality.
        Format format = Format::Undefined;       ///< The storage format.
        Extent3D extent = {};                    ///< Texels of mip 0.
        uint32_t mips = 1;                       ///< Mip levels.
        uint32_t layers = 1;                     ///< Array layers; faces for cube types.
        uint32_t samples = 1;                    ///< Samples per texel.
        TextureUsage usage = TextureUsage::None; ///< The usage flags.
        Heap heap = {};                          ///< The heap the texture is placed in; null for a dedicated allocation.
        uint64_t initialBytes = 0;               ///< Bytes of initial data the texture was created with.
        bool initialData = false;                ///< Created with initial data, so it starts in `Layout::ShaderRead`.
        uint64_t views = 0;                      ///< View slots holding a view of this texture (VU-rojo-texture-0028).
        bool swapchainImage = false;             ///< True for an image `Swapchain::acquire` returned (VU-rojo-present-0012, 0013, VU-rojo-sync-0005).
        TileMap mapped = {};                     ///< The mapped tiles of a Sparse texture; empty for any other texture.
        void* native = nullptr;                  ///< The backend's payload for the object; the core never reads it.
    };

    /// The shader stage an entry point of a library is compiled for.
    enum class ShaderStage : uint8_t
    {
        Vertex,   ///< A vertex entry.
        Fragment, ///< A fragment entry.
        Task,     ///< A mesh-pipeline task entry.
        Mesh,     ///< A mesh entry.
        Compute   ///< A compute entry.
    };

    /// One entry point a shader library holds; the backend reads it from the blob.
    struct ShaderEntryRecord
    {
        std::string name;                        ///< The entry point's name.
        ShaderStage stage = ShaderStage::Vertex; ///< The stage it is compiled for.
    };

    /// What the core keeps about one live ShaderLibrary: its entry points, which pipeline creation checks against.
    struct ShaderLibraryRecord
    {
        std::string label;                      ///< The label the object was created with.
        std::vector<ShaderEntryRecord> entries; ///< Every entry point, filled by the backend that parsed the blob.
        void* native = nullptr;                 ///< The backend's payload for the object; the core never reads it.
    };

    /// Whether a pipeline draws or dispatches.
    enum class PipelineKind : uint8_t
    {
        Graphics, ///< Created from a `GraphicsPipelineDesc`.
        Compute   ///< Created from a `ComputePipelineDesc`.
    };

    /// What the core keeps about one live Pipeline: the fields render-pass and draw checks compare against.
    struct PipelineRecord
    {
        std::string label;                                   ///< The label the object was created with.
        PipelineKind kind = PipelineKind::Graphics;          ///< Graphics or compute.
        uint32_t stages = 0;                                 ///< Bit `1 << ShaderStage` for each stage present.
        PrimitiveClass primitive = PrimitiveClass::Triangle; ///< The topology class; meaningful for a vertex pipeline.
        std::vector<Format> colors;                          ///< The format of each color target, in order.
        Format depthFormat = Format::Undefined;              ///< The depth attachment format; `Undefined` for none.
        uint32_t samples = 1;                                ///< Samples per texel of every attachment.
        bool depthBias = false;                              ///< `RasterState::depthBias` of the descriptor.
        void* native = nullptr;                              ///< The backend's payload for the object; the core never reads it.
    };

    /// What the core keeps about one live Heap: its description, how many live resources are placed in it and how many tiles are
    /// mapped from it.
    struct HeapRecord
    {
        std::string label;            ///< The label the object was created with.
        uint64_t size = 0;            ///< Usable bytes, at least the size requested.
        Memory memory = Memory::Gpu;  ///< The memory class of every resource placed inside.
        uint64_t placedResources = 0; ///< Resources created in the heap and not yet destroyed (VU-rojo-device-0008).
        uint64_t mappedTiles = 0;     ///< Tiles of sparse resources currently mapped from the heap (VU-rojo-device-0009).
        void* native = nullptr;       ///< The backend's payload for the object; the core never reads it.
    };

    /// What the core keeps about one live Fence: the timeline values its submission rules compare against.
    struct FenceRecord
    {
        std::string label;      ///< The label the object was created with.
        uint64_t initial = 0;   ///< The value the fence was created with.
        uint64_t submitted = 0; ///< The greatest value submitted for signal, or `initial` when none was (VU-rojo-queue-0003).
        uint64_t completed = 0; ///< The greatest value the fence has reached; a backend raises it as work completes.
        void* native = nullptr; ///< The backend's payload for the object; the core never reads it.
    };

    /// The number of the frame an upscaler last upscaled in, updated without a lock; 0 means none, because frames are numbered
    /// from one. It copies by value so a record can be moved into a pool. Only Debug builds touch it (VU-rojo-upscale-0008).
    struct FrameStamp
    {
        std::atomic<uint64_t> frame{ 0 }; ///< The frame number.

        FrameStamp() = default;

        /// Copies the stamp's current value.
        FrameStamp(const FrameStamp& other) : frame(other.frame.load(std::memory_order_relaxed))
        {
        }

        /// Copies the stamp's current value.
        FrameStamp& operator=(const FrameStamp& other)
        {
            frame.store(other.frame.load(std::memory_order_relaxed), std::memory_order_relaxed);
            return *this;
        }
    };

    /// What the core keeps about one live Upscaler: its description with the kind resolved, and the frame it last upscaled in.
    struct UpscalerRecord
    {
        std::string label;                         ///< The label the object was created with.
        UpscalerKind kind = UpscalerKind::MetalFX; ///< The kind created; never `Auto`.
        Extent2D input = {};                       ///< Allocated extent of the color, depth, motion and reactive inputs.
        Extent2D output = {};                      ///< Extent of the reconstructed output.
        Format color = Format::RGBA16Float;        ///< Format of the color input.
        Format depth = Format::D32Float;           ///< Format of the depth input.
        Format motion = Format::RG16Float;         ///< Format of the motion input.
        Format outputFormat = Format::RGBA16Float; ///< Format of the output.
        UpscalerFlags flags = UpscalerFlags::None; ///< Creation-time behaviour.
        FrameStamp lastFrame;                      ///< The frame of the latest upscale (VU-rojo-upscale-0008).
        void* native = nullptr;                    ///< The backend's payload for the object; the core never reads it.
    };

    /// Maps a handle type to the record its pool stores.
    template<typename Handle> struct RecordOf;

    /// Records of Buffer handles.
    template<> struct RecordOf<Buffer>
    {
        using Type = BufferRecord; ///< The record type.
    };
    /// Records of Texture handles.
    template<> struct RecordOf<Texture>
    {
        using Type = TextureRecord; ///< The record type.
    };
    /// Records of Pipeline handles.
    template<> struct RecordOf<Pipeline>
    {
        using Type = PipelineRecord; ///< The record type.
    };
    /// Records of ShaderLibrary handles.
    template<> struct RecordOf<ShaderLibrary>
    {
        using Type = ShaderLibraryRecord; ///< The record type.
    };
    /// Records of Heap handles.
    template<> struct RecordOf<Heap>
    {
        using Type = HeapRecord; ///< The record type.
    };
    /// Records of Fence handles.
    template<> struct RecordOf<Fence>
    {
        using Type = FenceRecord; ///< The record type.
    };
    /// Records of Upscaler handles.
    template<> struct RecordOf<Upscaler>
    {
        using Type = UpscalerRecord; ///< The record type.
    };

    /// The record type of `Handle`.
    template<typename Handle> using RecordFor = typename RecordOf<Handle>::Type;

    /// A function that releases the native payload of one retiring record; it receives the backend's `context`.
    template<typename Record> using RetireFn = void (*)(void* context, Record& record);

    /// The functions a backend registers so the core can hand it every record that leaves a pool. A null entry means the
    /// backend holds nothing for that type. A hook runs under its pool's lock and must not call the core's pools.
    struct RetireHooks
    {
        void* context = nullptr;                               ///< Passed to every hook; the backend's device state.
        RetireFn<BufferRecord> buffer = nullptr;               ///< Releases a retiring Buffer.
        RetireFn<TextureRecord> texture = nullptr;             ///< Releases a retiring Texture.
        RetireFn<PipelineRecord> pipeline = nullptr;           ///< Releases a retiring Pipeline.
        RetireFn<ShaderLibraryRecord> shaderLibrary = nullptr; ///< Releases a retiring ShaderLibrary.
        RetireFn<HeapRecord> heap = nullptr;                   ///< Releases a retiring Heap.
        RetireFn<FenceRecord> fence = nullptr;                 ///< Releases a retiring Fence.
        RetireFn<UpscalerRecord> upscaler = nullptr;           ///< Releases a retiring Upscaler.
        /// Called when `beginFrame` reuses frame slot `slot`, after the slot's pools retired: the backend returns what it
        /// pooled for that slot, such as its command lists. Null when the backend pools nothing per slot.
        void (*frameSlot)(void* context, uint32_t slot) = nullptr;

        /// Returns the hook for `Handle`, which may be null.
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
