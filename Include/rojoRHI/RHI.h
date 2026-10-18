//----------------------------------------------------------------------------------------------------------------------
/// @file RHI.h
/// @brief Declares the whole v2 public interface, in sections that follow the chapters of the API reference.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

//======================================================================================================================
// Version - docs/spec/01-fundamentals-part2.md

/// Major version of the interface; a breaking change after 1.0.0 bumps it.
#define ROJORHI_VERSION_MAJOR 0
/// Minor version of the interface; a breaking change before 1.0.0 bumps it.
#define ROJORHI_VERSION_MINOR 1
/// Patch version of the interface; a compatible fix bumps it.
#define ROJORHI_VERSION_PATCH 0

//======================================================================================================================
// 01. Fundamentals - docs/spec/01-fundamentals*.md

namespace rojoRHI {
    /// The stable failure category a caller branches on; enumerators are only ever appended.
    enum class ErrorCategory : uint8_t
    {
        Unsupported,       ///< The hardware, driver or build lacks a required feature.
        OutOfMemory,       ///< A device or host allocation failed.
        DriverFailure,     ///< The driver refused a well-formed request.
        ShaderAbiMismatch, ///< A shader library carries another ROJORHI_SHADER_ABI_VERSION.
    };

    /// A failure of a creation entry. It owns its message string and is not trivially copyable.
    struct Error
    {
        /// Stable failure category; stable across versions.
        ErrorCategory category;
        /// Human-readable context, including the native error text; its wording may change in any release.
        std::string message;
    };

    /// The result of a creation entry: the created value or an Error. `Result<void>` carries no value. An entry that
    /// returns an error has created nothing and leaked nothing.
    template<class T> using Result = std::expected<T, Error>;

    /// A handle naming one buffer: a 64-bit identifier of slot index, generation and type tag whose bit positions
    /// are not part of the interface. The all-zero value is the null handle. It is meaningful only to the device that
    /// created it.
    struct Buffer
    {
        uint64_t bits = 0; ///< Opaque identifier; copy, compare and hash it, but do not decode it.

        /// Compares two handles by their bits.
        friend bool operator==(Buffer, Buffer) = default;
    };

    /// A handle naming one texture: a 64-bit identifier of slot index, generation and type tag whose bit positions
    /// are not part of the interface. The all-zero value is the null handle. It is meaningful only to the device that
    /// created it.
    struct Texture
    {
        uint64_t bits = 0; ///< Opaque identifier; copy, compare and hash it, but do not decode it.

        /// Compares two handles by their bits.
        friend bool operator==(Texture, Texture) = default;
    };

    /// A handle naming one pipeline: a 64-bit identifier of slot index, generation and type tag whose bit positions
    /// are not part of the interface. The all-zero value is the null handle. It is meaningful only to the device that
    /// created it.
    struct Pipeline
    {
        uint64_t bits = 0; ///< Opaque identifier; copy, compare and hash it, but do not decode it.

        /// Compares two handles by their bits.
        friend bool operator==(Pipeline, Pipeline) = default;
    };

    /// A handle naming one shader library: a 64-bit identifier of slot index, generation and type tag whose bit positions
    /// are not part of the interface. The all-zero value is the null handle. It is meaningful only to the device that
    /// created it.
    struct ShaderLibrary
    {
        uint64_t bits = 0; ///< Opaque identifier; copy, compare and hash it, but do not decode it.

        /// Compares two handles by their bits.
        friend bool operator==(ShaderLibrary, ShaderLibrary) = default;
    };

    /// A handle naming one heap: a 64-bit identifier of slot index, generation and type tag whose bit positions
    /// are not part of the interface. The all-zero value is the null handle. It is meaningful only to the device that
    /// created it.
    struct Heap
    {
        uint64_t bits = 0; ///< Opaque identifier; copy, compare and hash it, but do not decode it.

        /// Compares two handles by their bits.
        friend bool operator==(Heap, Heap) = default;
    };

    /// A handle naming one fence: a 64-bit identifier of slot index, generation and type tag whose bit positions
    /// are not part of the interface. The all-zero value is the null handle. It is meaningful only to the device that
    /// created it.
    struct Fence
    {
        uint64_t bits = 0; ///< Opaque identifier; copy, compare and hash it, but do not decode it.

        /// Compares two handles by their bits.
        friend bool operator==(Fence, Fence) = default;
    };

    /// A handle naming one upscaler: a 64-bit identifier of slot index, generation and type tag whose bit positions
    /// are not part of the interface. The all-zero value is the null handle. It is meaningful only to the device that
    /// created it.
    struct Upscaler
    {
        uint64_t bits = 0; ///< Opaque identifier; copy, compare and hash it, but do not decode it.

        /// Compares two handles by their bits.
        friend bool operator==(Upscaler, Upscaler) = default;
    };

    /// A width and height in texels or pixels, as the using entry states; the zero default lets an entry give `{}` a
    /// meaning of its own.
    struct Extent2D
    {
        uint32_t width = 0;  ///< Width.
        uint32_t height = 0; ///< Height.
    };

    /// A width, height and depth in texels; each member defaults to one, so a two-dimensional extent names only width
    /// and height.
    struct Extent3D
    {
        uint32_t width = 1;  ///< Width.
        uint32_t height = 1; ///< Height.
        uint32_t depth = 1;  ///< Depth.
    };

    /// A texel position inside one mip level; offsets are never negative.
    struct Offset3D
    {
        uint32_t x = 0; ///< Horizontal position.
        uint32_t y = 0; ///< Vertical position.
        uint32_t z = 0; ///< Depth position.
    };

    /// A rectangle in framebuffer pixels with its origin at the top-left.
    struct Rect2D
    {
        uint32_t x = 0;      ///< Left edge.
        uint32_t y = 0;      ///< Top edge.
        uint32_t width = 0;  ///< Width.
        uint32_t height = 0; ///< Height.
    };

    class Device;

    /// A presentation surface and its images. It wraps one backend pointer: copies refer to the same swapchain, and a
    /// default-constructed one refers to none (VU-rojo-core-0004). It lives from `Device::createSwapchain` to
    /// `Device::destroy`; using it after that is undefined behaviour no tier detects. It is used only with the device that
    /// created it (VU-rojo-core-0005). One thread at a time uses a swapchain, ordered with the `Queue::present` that consumes
    /// it.
    class Swapchain
    {
      public:
        /// Sets the extent of every image to `extent` and replaces the images; the next `acquire` returns an image of the new
        /// extent. Presentation setup outside the frame loop: no frame is open (VU-rojo-present-0007), no image is acquired
        /// and not yet presented (VU-rojo-present-0008), and both dimensions are above zero (VU-rojo-present-0009).
        void resize(Extent2D extent);

        /// Returns the next image, labelled after the swapchain, with undefined contents, inside an open frame
        /// (VU-rojo-present-0010) with no image of this swapchain acquired and not yet presented (VU-rojo-present-0011). It
        /// returns a null texture when no image became free within one second. The image is used only as a color attachment
        /// and in texture barriers (VU-rojo-present-0012), is never destroyed by the caller (VU-rojo-present-0013), and its
        /// first barrier names `Layout::Undefined` as the old layout (VU-rojo-present-0014). Its handle is stale after
        /// `Queue::present`. Thread class swapchain.
        Texture acquire();

      private:
        friend class Device;
        friend struct SwapchainImpl;
        struct SwapchainImpl* m_impl = nullptr;
    };

    /// Classifies the urgency of a diagnostic message.
    enum class MessageSeverity : uint8_t
    {
        Info,    ///< Routine progress or configuration detail.
        Warning, ///< A recoverable condition worth reporting.
        Error,   ///< A failure, including the text of a violated valid-usage rule.
    };

    /// Receives one diagnostic message; `message` is valid only for the duration of the call and `user` is the pointer
    /// given to setMessageCallback. It may run on any thread, so it must be thread-safe, and it must return normally. It
    /// calls no RojoRHI entry point (VU-rojo-core-0008).
    using MessageCallback = void (*)(MessageSeverity severity, std::string_view message, void* user);

    /// Installs the process-wide message sink; `nullptr` restores the default, which writes each message to stderr.
    /// Free-threaded. The new callback receives every message emitted after the call returns. A previously installed
    /// callback may still be running on another thread when this returns, so its `user` must outlive that call.
    void setMessageCallback(MessageCallback callback, void* user);
} // namespace rojoRHI

//======================================================================================================================
// 02. Device - docs/spec/02-device.md

namespace rojoRHI {
    /// Describes the device `createDevice` opens.
    struct DeviceDesc
    {
        /// Runs the validation tracker for the device's lifetime. The tracker exists only in non-Release builds; with this set a
        /// Release build's `createDevice` returns `ErrorCategory::Unsupported`.
        bool validation = false;
        /// Prepares programmatic GPU capture before the device exists, on backends whose capture tool must be loaded first.
        bool enableCapture = false;
        /// Frame slots, which bound how many frames the CPU records ahead of the GPU: 2 or 3 (VU-rojo-device-0001).
        uint32_t framesInFlight = 3;
        /// A blob from `Device::pipelineCacheData`, read during `createDevice`; empty for none. A blob from another driver,
        /// device or RojoRHI version is ignored.
        std::span<const std::byte> pipelineCache;
        /// Names the device in captures and messages; "rojorhi.device.unnamed" when empty.
        std::string_view label;
    };
} // namespace rojoRHI

//======================================================================================================================
// 03. Memory and buffers - docs/spec/03-memory-and-buffers*.md

namespace rojoRHI {
    /// The memory class of a buffer or heap, named by intent rather than by backend heap type. It fixes a buffer's copy
    /// roles: `Gpu` is a copy source and destination, `Upload` a copy source, `Readback` a copy destination.
    enum class Memory : uint8_t
    {
        Gpu,      ///< Device-local; not CPU-visible.
        Upload,   ///< CPU writes, GPU reads; persistently mapped.
        Readback, ///< GPU writes, CPU reads; persistently mapped.
    };

    /// What a backend must know about a buffer at creation. Every `Gpu` and `Upload` buffer is shader-readable by address,
    /// and copy roles follow the memory class, so only these four flags exist.
    enum class BufferUsage : uint32_t
    {
        None = 0,               ///< No flag.
        Index = 1u << 0,        ///< Index buffer of an indexed draw.
        IndirectArgs = 1u << 1, ///< Arguments of an indirect draw or dispatch.
        ShaderWrite = 1u << 2,  ///< Written by shaders through an `Addr`; a `Memory::Gpu` buffer only.
        Sparse = 1u << 3,       ///< Reserved: no backing memory until tiles are mapped; a `Memory::Gpu` buffer only.
    };

    /// Returns the union of two usage sets.
    constexpr BufferUsage operator|(BufferUsage a, BufferUsage b)
    {
        return static_cast<BufferUsage>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
    }

    /// Returns the flags both usage sets contain.
    constexpr BufferUsage operator&(BufferUsage a, BufferUsage b)
    {
        return static_cast<BufferUsage>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
    }

    /// Describes a buffer for `Device::createBuffer`. `size` is greater than zero and at most `Limits::maxBufferSize`
    /// (VU-rojo-buffer-0006).
    struct BufferDesc
    {
        /// Bytes.
        uint64_t size = 0;
        /// Memory class.
        Memory memory = Memory::Gpu;
        /// Index, IndirectArgs, ShaderWrite and Sparse.
        BufferUsage usage = BufferUsage::None;
        /// Debug label; empty selects a `rojorhi.`-prefixed default.
        std::string_view label;
    };

    /// An 8-byte value naming buffer memory, with a backend-defined encoding; the value-initialized `Addr{}` is the
    /// null address on every backend.
    struct alignas(8) Addr
    {
        /// First word of the encoding.
        uint32_t lo;
        /// Second word of the encoding.
        uint32_t hi;
    };

    /// Returns the address `offset` bytes further into the buffer `base` names. The result lies less than
    /// `Limits::maxAddrOffset` bytes from the buffer's start (VU-rojo-buffer-0011).
    Addr operator+(Addr base, uint64_t offset);

    /// Describes a placement heap for `Device::createHeap`: one device allocation that buffers and textures are placed inside
    /// at explicit offsets. It tracks no hazards: placed resources whose byte ranges overlap alias, and the caller orders
    /// them. A backend may round `size` up; the usable size is never less than `size`. `size` is greater than zero
    /// (VU-rojo-heap-0001).
    struct HeapDesc
    {
        /// Bytes.
        uint64_t size = 0;
        /// Memory class of every resource placed inside.
        Memory memory = Memory::Gpu;
        /// Debug label; empty selects a `rojorhi.`-prefixed default.
        std::string_view label;
    };

    /// The answer of `Device::placementRequirements`: the bytes a resource occupies in a heap and the alignment its offset
    /// must satisfy. The same descriptor gives the same value on one device.
    struct PlacementRequirements
    {
        /// Bytes the resource occupies in a heap.
        uint64_t size = 0;
        /// Power of two the heap offset is a multiple of.
        uint64_t alignment = 0;
    };

    /// Passed to `Device::createBuffer` and `Device::createTexture` to create the resource inside a heap. With a null `heap`
    /// the resource is a dedicated allocation. Otherwise, with `req` the `PlacementRequirements` of the descriptor, `offset`
    /// plus `req.size` is at most the heap's size (VU-rojo-heap-0003), `offset` is a multiple of `req.alignment`
    /// (VU-rojo-heap-0004), the resource's memory class equals the heap's and a texture goes only in a `Memory::Gpu` heap
    /// (VU-rojo-heap-0005), and the resource is not `Sparse` (VU-rojo-heap-0006). The heap outlives the resource
    /// (VU-rojo-device-0008).
    struct Placement
    {
        /// The heap to place the resource in; null selects a dedicated allocation.
        Heap heap = {};
        /// Byte offset inside `heap`.
        uint64_t offset = 0;
    };
} // namespace rojoRHI

//======================================================================================================================
// 15. Formats and limits: Format - docs/spec/15-formats-and-limits*.md
// Placed before 04 because textures name it.

namespace rojoRHI {
    /// Names the storage layout and transfer function of a texture, a texture view, an attachment or a pipeline target. New
    /// formats are appended in later API versions; existing values never change.
    enum class Format : uint32_t
    {
        Undefined,       ///< No format.
        BGRA8Unorm,      ///< 8-bit normalized BGRA color.
        RGBA8Unorm,      ///< 8-bit normalized RGBA color.
        RGBA8Unorm_sRGB, ///< 8-bit RGBA color, sRGB transfer.
        RGBA16Float,     ///< Half-precision RGBA, the scene-linear color format.
        RG16Float,       ///< Half-precision two-channel data.
        R8Unorm,         ///< 8-bit normalized single channel.
        BC1Unorm,        ///< BC1 block-compressed linear color.
        BC1Unorm_sRGB,   ///< BC1 block-compressed color, sRGB transfer.
        D32Float,        ///< 32-bit floating-point depth.
        R16Float,        ///< Half-precision single channel.
        R32Float,        ///< Full-precision single channel.
        D32FloatS8,      ///< 32-bit floating-point depth with 8-bit stencil.
        RGB10A2Unorm,    ///< 10-bit normalized RGB with 2-bit alpha, the HDR10 presentation format.
    };
} // namespace rojoRHI

//======================================================================================================================
// 06. Pipelines: CompareOp - docs/spec/06-pipelines*.md
// Placed before 04 because samplers name it.

namespace rojoRHI {
    /// The one comparison function of the interface: a test passes when `incoming op stored` holds, or always (`Always`) or
    /// never (`Never`).
    enum class CompareOp : uint8_t
    {
        Never,          ///< The test never passes.
        Less,           ///< Passes when incoming is less than stored.
        Equal,          ///< Passes when incoming equals stored.
        LessOrEqual,    ///< Passes when incoming is less than or equal to stored.
        Greater,        ///< Passes when incoming is greater than stored.
        NotEqual,       ///< Passes when incoming differs from stored.
        GreaterOrEqual, ///< Passes when incoming is greater than or equal to stored.
        Always,         ///< The test always passes.
    };
} // namespace rojoRHI

//======================================================================================================================
// 04. Textures and heaps - docs/spec/04-textures-and-heaps*.md

namespace rojoRHI {
    /// The dimensionality of a texture or of the view a shader fetches. For `Cube` and `CubeArray`, array layers count faces,
    /// six per cube, in the order +X, -X, +Y, -Y, +Z, -Z. Multisampling is a property of `TextureDesc::samples`, not a type.
    enum class TextureType : uint8_t
    {
        Tex1D,      ///< One-dimensional.
        Tex1DArray, ///< Array of one-dimensional layers.
        Tex2D,      ///< Two-dimensional.
        Tex2DArray, ///< Array of two-dimensional layers.
        Tex3D,      ///< Three-dimensional.
        Cube,       ///< Six square faces.
        CubeArray,  ///< A multiple of six square faces.
    };

    /// What a backend must know about a texture at creation. Every texture is a copy source and destination without a flag.
    enum class TextureUsage : uint32_t
    {
        None = 0,               ///< No flag.
        Sampled = 1u << 0,      ///< Read through a sampled view.
        Storage = 1u << 1,      ///< Read and written through a storage view.
        RenderTarget = 1u << 2, ///< Color attachment.
        DepthStencil = 1u << 3, ///< Depth-stencil attachment.
        Sparse = 1u << 4,       ///< Reserved: no backing memory until tiles are mapped.
    };

    /// Returns the union of two usage sets.
    constexpr TextureUsage operator|(TextureUsage a, TextureUsage b)
    {
        return static_cast<TextureUsage>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
    }

    /// Returns the flags both usage sets contain.
    constexpr TextureUsage operator&(TextureUsage a, TextureUsage b)
    {
        return static_cast<TextureUsage>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
    }

    /// Describes a texture for `Device::createTexture` and `Device::placementRequirements`. The format is not `Undefined`
    /// (VU-rojo-texture-0001); every extent component is at least 1 and within the limit for the type (VU-rojo-texture-0002),
    /// and the type fixes the height, depth, width and layers (VU-rojo-texture-0003 to 0005), which are at most
    /// `Limits::maxTextureLayers` (VU-rojo-texture-0039). `mips` is within the extent
    /// (VU-rojo-texture-0006). `samples` is supported for the format and constrains the type, mips and usage
    /// (VU-rojo-texture-0007, 0008). The usage is not empty, is supported for the format and combines only as
    /// VU-rojo-texture-0009 to 0012 allow; a block-compressed format has a level-0 width and height that are multiples of 4
    /// (VU-rojo-format-0004).
    struct TextureDesc
    {
        /// Dimensionality; `Tex1D`, `Tex2D`, `Tex3D`, `Cube` and the array forms.
        TextureType type = TextureType::Tex2D;
        /// Storage format.
        Format format = Format::Undefined;
        /// Texels of mip 0.
        Extent3D extent;
        /// Mip levels.
        uint32_t mips = 1;
        /// Array layers; for cube types, faces.
        uint32_t layers = 1;
        /// Samples per texel.
        uint32_t samples = 1;
        /// Sampled, Storage, RenderTarget, DepthStencil and Sparse.
        TextureUsage usage = TextureUsage::None;
        /// Debug label; empty selects a `rojorhi.`-prefixed default.
        std::string_view label;
    };

    /// The initial contents of one subresource for `Device::createTexture`. Rows of a block-compressed format are rows of
    /// blocks, counted with rounding up. When `bytes` is not empty, `bytesPerRow` is at least the packed row size and a
    /// multiple of the texel or block size (VU-rojo-texture-0013), `bytesPerImage` is at least `bytesPerRow` times the rows of
    /// one slice for a `Tex3D` mip and 0 otherwise (VU-rojo-texture-0014), and `bytes` holds the bytes the strides imply for
    /// the subresource (VU-rojo-texture-0015).
    struct TextureData
    {
        /// The texels; empty leaves the subresource undefined.
        std::span<const std::byte> bytes;
        /// Distance in bytes between rows of texels or compressed blocks.
        uint64_t bytesPerRow = 0;
        /// Distance in bytes between depth slices of a `Tex3D` mip; 0 otherwise.
        uint64_t bytesPerImage = 0;
    };

    /// A `mipCount` that names every mip from `baseMip` through the last.
    inline constexpr uint32_t kAllMips = ~0u;

    /// A `layerCount` that names every layer from `baseLayer` through the last.
    inline constexpr uint32_t kAllLayers = ~0u;

    /// Names the mips and array layers a view or a texture barrier covers; the default covers the whole texture, and layers of
    /// cube types are faces. Neither count is 0 (VU-rojo-texture-0016), and the range lies inside the texture
    /// (VU-rojo-texture-0017).
    struct SubresourceRange
    {
        /// First mip.
        uint32_t baseMip = 0;
        /// Mips covered, or `kAllMips`.
        uint32_t mipCount = kAllMips;
        /// First layer.
        uint32_t baseLayer = 0;
        /// Layers covered, or `kAllLayers`.
        uint32_t layerCount = kAllLayers;
    };

    /// The plane of a depth-stencil format a view selects, so the stencil of `D32FloatS8` can be sampled.
    enum class TextureAspect : uint8_t
    {
        Color,   ///< The color of a color format.
        Depth,   ///< The depth plane.
        Stencil, ///< The stencil plane.
    };

    /// Describes how a shader sees a texture through a view index, for `Device::createView`. `usage` is exactly `Sampled` or
    /// `Storage` and the texture has it (VU-rojo-texture-0022); `type` is compatible with the texture's (VU-rojo-texture-0023)
    /// and fixes the layers covered (VU-rojo-texture-0024); `format` is `Undefined`, the texture's format or its sRGB or
    /// linear counterpart (VU-rojo-texture-0025) and supports `usage` (VU-rojo-format-0003); a `Storage` view covers one mip
    /// of a single-sample texture (VU-rojo-texture-0026); `aspect` fits the texture's format (VU-rojo-texture-0027).
    struct TextureViewDesc
    {
        /// Dimensionality the shader fetches.
        TextureType type = TextureType::Tex2D;
        /// `Undefined` selects the texture's own format.
        Format format = Format::Undefined;
        /// Mips and layers exposed.
        SubresourceRange range;
        /// Exactly `Sampled` or `Storage`.
        TextureUsage usage = TextureUsage::Sampled;
        /// Plane of a depth-stencil format.
        TextureAspect aspect = TextureAspect::Color;
    };

    /// A 32-bit index into the global view heap, stored in shader data with `std::to_underlying`.
    enum class ViewIndex : uint32_t
    {
    };

    /// How a sampler picks or blends texels.
    enum class SamplerFilter : uint8_t
    {
        Nearest, ///< The nearest texel or mip level.
        Linear,  ///< A linear blend.
    };

    /// How a sampler treats coordinates outside the texture.
    enum class SamplerAddress : uint8_t
    {
        Repeat,       ///< The coordinate wraps.
        MirrorRepeat, ///< The coordinate wraps, mirrored on every other repeat.
        ClampToEdge,  ///< The coordinate clamps to the edge texel.
    };

    /// Describes a sampler, for `Device::createSampler` and `Device::writeSampler`. `maxAnisotropy` is between 1 and 16
    /// (VU-rojo-texture-0029) and is above 1 only when `filter` and `mipFilter` are `Linear` (VU-rojo-texture-0030).
    struct SamplerDesc
    {
        /// Minification and magnification.
        SamplerFilter filter = SamplerFilter::Linear;
        /// Between mip levels.
        SamplerFilter mipFilter = SamplerFilter::Linear;
        /// Every axis.
        SamplerAddress address = SamplerAddress::Repeat;
        /// 1 disables anisotropic filtering.
        uint32_t maxAnisotropy = 1;
        /// True makes a comparison sampler, fetched only for depth comparisons.
        bool compareEnable = false;
        /// Used only when `compareEnable` is true.
        CompareOp compare = CompareOp::LessOrEqual;
        /// Empty selects a `rojorhi.`-prefixed default.
        std::string_view label;
    };

    /// A 32-bit index into the global sampler heap, stored in shader data with `std::to_underlying`.
    enum class SamplerIndex : uint32_t
    {
    };

    /// A caller-owned contiguous range of heap slots. Slot `base + i` for `i` below `count` is the view index or sampler index
    /// a shader fetches. A range passed to RojoRHI is a value `Device::reserveViews` or `Device::reserveSamplers` returned,
    /// unmodified, that has not been released (VU-rojo-texture-0031).
    struct IndexRange
    {
        /// The heap a range lives in.
        enum class Kind : uint8_t
        {
            Views,    ///< The view heap.
            Samplers, ///< The sampler heap.
        };

        /// First slot of the range.
        uint32_t base = 0;
        /// Slots in the range.
        uint32_t count = 0;
        /// Heap the range lives in, set by the reserving call.
        Kind kind = Kind::Views;
    };
} // namespace rojoRHI

//======================================================================================================================
// 05. Shader ABI - docs/spec/05-shader-abi*.md
// Its C++ entries, setRoot and pushRoot, are members of CommandList in the object handles section.

//======================================================================================================================
// 06. Pipelines - docs/spec/06-pipelines*.md

namespace rojoRHI {
    /// Names one entry point of a loaded shader library. A default-constructed entry (null `library`) means the stage is
    /// absent. When the stage is present, `library` is live and `name` is not empty (VU-rojo-pipeline-0001) and names an entry
    /// point of `library` whose shader stage is the stage of the descriptor field holding the entry (VU-rojo-pipeline-0002).
    /// `Device::createPipeline` copies `name`, so it need not outlive the call.
    struct ShaderEntry
    {
        /// The library holding the entry point; null when the stage is absent.
        ShaderLibrary library;
        /// The entry point's name inside `library`.
        std::string_view name;
    };

    /// A factor that scales a source or destination term of a blend equation.
    enum class BlendFactor : uint8_t
    {
        Zero,             ///< 0.
        One,              ///< 1.
        SrcColor,         ///< The fragment's color.
        OneMinusSrcColor, ///< 1 minus the fragment's color.
        SrcAlpha,         ///< The fragment's alpha.
        OneMinusSrcAlpha, ///< 1 minus the fragment's alpha.
        DstColor,         ///< The attachment's color.
        OneMinusDstColor, ///< 1 minus the attachment's color.
        DstAlpha,         ///< The attachment's alpha.
        OneMinusDstAlpha, ///< 1 minus the attachment's alpha.
        SrcAlphaSaturate, ///< The smaller of the fragment's alpha and 1 minus the attachment's alpha.
        Constant,         ///< The blend constants set on the command list.
        OneMinusConstant, ///< 1 minus the blend constants.
    };

    /// How the two scaled terms of a blend equation combine.
    enum class BlendOp : uint8_t
    {
        Add,             ///< Source plus destination.
        Subtract,        ///< Source minus destination.
        ReverseSubtract, ///< Destination minus source.
        Min,             ///< The smaller of the two; the factors are ignored.
        Max,             ///< The larger of the two; the factors are ignored.
    };

    /// Fixed-function blending for one color target, baked into the pipeline. With `enable` false the fragment output is
    /// written unchanged. Dual-source blending and logic operations are not part of 0.1.0.
    struct BlendState
    {
        /// True turns blending on; only for a format that reports `FormatCapabilities::blend` (VU-rojo-format-0003).
        bool enable = false;
        /// Factor of the fragment's color.
        BlendFactor srcColor = BlendFactor::One;
        /// Factor of the attachment's color.
        BlendFactor dstColor = BlendFactor::Zero;
        /// Combines the color terms.
        BlendOp colorOp = BlendOp::Add;
        /// Factor of the fragment's alpha.
        BlendFactor srcAlpha = BlendFactor::One;
        /// Factor of the attachment's alpha.
        BlendFactor dstAlpha = BlendFactor::Zero;
        /// Combines the alpha terms.
        BlendOp alphaOp = BlendOp::Add;
    };

    /// The channels a color target writes; a bit set.
    enum class ColorWriteMask : uint8_t
    {
        None = 0, ///< No channel.
        R = 1,    ///< Red.
        G = 2,    ///< Green.
        B = 4,    ///< Blue.
        A = 8,    ///< Alpha.
        All = 15, ///< Every channel.
    };

    /// Returns the union of two channel sets.
    constexpr ColorWriteMask operator|(ColorWriteMask a, ColorWriteMask b)
    {
        return static_cast<ColorWriteMask>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
    }

    /// Returns the channels both sets contain.
    constexpr ColorWriteMask operator&(ColorWriteMask a, ColorWriteMask b)
    {
        return static_cast<ColorWriteMask>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
    }

    /// One color output of a graphics pipeline: the attachment format, its blend state and the channels written. Entry `i` of
    /// `GraphicsPipelineDesc::colors` matches color attachment `i` of the render pass. `format` is not `Undefined`
    /// (VU-rojo-format-0002) and reports `FormatCapabilities::colorAttachment` (VU-rojo-format-0003).
    struct ColorTarget
    {
        /// The attachment format.
        Format format = Format::Undefined;
        /// Blending of this target.
        BlendState blend;
        /// The channels written.
        ColorWriteMask writeMask = ColorWriteMask::All;
    };

    /// Which faces are discarded.
    enum class CullMode : uint8_t
    {
        None,  ///< No face is discarded.
        Front, ///< Front faces are discarded.
        Back,  ///< Back faces are discarded.
    };

    /// Which winding order is a front face.
    enum class FrontFace : uint8_t
    {
        CounterClockwise, ///< Counter-clockwise triangles are front faces.
        Clockwise,        ///< Clockwise triangles are front faces.
    };

    /// How triangles are filled.
    enum class FillMode : uint8_t
    {
        Solid,     ///< Filled.
        Wireframe, ///< Edges only.
    };

    /// Rasterizer state baked into a graphics pipeline. With `depthBias` false, depth-bias values set on the command list are
    /// ignored; with it true they apply.
    struct RasterState
    {
        /// Faces discarded.
        CullMode cull = CullMode::None;
        /// Winding order of a front face.
        FrontFace frontFace = FrontFace::CounterClockwise;
        /// Triangle fill.
        FillMode fill = FillMode::Solid;
        /// False clamps fragment depth instead of clipping.
        bool depthClip = true;
        /// Enables the values of `CommandList::setDepthBias`.
        bool depthBias = false;
        /// Conservative rasterization; true only when `Capabilities::conservativeRaster` is (VU-rojo-pipeline-0006).
        bool conservative = false;
    };

    /// What a stencil test does to the stored stencil value.
    enum class StencilOp : uint8_t
    {
        Keep,           ///< Leaves the value.
        Zero,           ///< Sets the value to 0.
        Replace,        ///< Sets the value to the reference of `CommandList::setStencilReference`.
        IncrementClamp, ///< Adds 1, saturating at the maximum.
        DecrementClamp, ///< Subtracts 1, saturating at 0.
        Invert,         ///< Inverts the bits.
        IncrementWrap,  ///< Adds 1, wrapping to 0.
        DecrementWrap,  ///< Subtracts 1, wrapping to the maximum.
    };

    /// The stencil operations and comparison of one face orientation.
    struct StencilFaceState
    {
        /// Applied when the stencil test fails.
        StencilOp fail = StencilOp::Keep;
        /// Applied when the stencil test passes and the depth test fails.
        StencilOp depthFail = StencilOp::Keep;
        /// Applied when both tests pass.
        StencilOp pass = StencilOp::Keep;
        /// The stencil comparison.
        CompareOp compare = CompareOp::Always;
    };

    /// Depth test, depth write, compare and stencil operations, baked into the pipeline. `depthWrite` is true only when
    /// `depthTest` is (VU-rojo-pipeline-0007). Depth bounds testing is not part of 0.1.0.
    struct DepthStencilState
    {
        /// Enables the depth test.
        bool depthTest = false;
        /// Writes passing depth values.
        bool depthWrite = false;
        /// The depth comparison; reverse Z, matching clear depth 0.
        CompareOp depthCompare = CompareOp::GreaterOrEqual;
        /// Enables the stencil test.
        bool stencilTest = false;
        /// Bits of the stencil value the comparison reads.
        uint8_t stencilReadMask = 0xFF;
        /// Bits of the stencil value a write changes.
        uint8_t stencilWriteMask = 0xFF;
        /// Operations for front faces.
        StencilFaceState front;
        /// Operations for back faces.
        StencilFaceState back;
    };

    /// The topology class baked into a graphics pipeline with a vertex stage; the exact topology within the class is dynamic
    /// (`CommandList::setPrimitiveTopology`). A mesh pipeline ignores the class: its mesh entry point declares its output
    /// topology.
    enum class PrimitiveClass : uint8_t
    {
        Point,    ///< Points.
        Line,     ///< Lines and line strips.
        Triangle, ///< Triangles and triangle strips.
    };

    /// Describes a vertex pipeline (`vertex`, optional `fragment`) or a mesh pipeline (optional `task`, `mesh`, optional
    /// `fragment`), for `Device::createPipeline`; spans and strings are copied. Exactly one of `vertex` and `mesh` is present
    /// (VU-rojo-pipeline-0008) and `task` only with `mesh` (VU-rojo-pipeline-0009); `colors` holds at most
    /// `Limits::maxColorAttachments` entries (VU-rojo-pipeline-0010); `depthFormat` is `Undefined` or a depth format that
    /// supplies the components the depth-stencil state tests (VU-rojo-pipeline-0011); every attachment format supports
    /// `samples` (VU-rojo-format-0003).
    struct GraphicsPipelineDesc
    {
        /// The vertex stage, or absent for a mesh pipeline.
        ShaderEntry vertex;
        /// The optional task stage of a mesh pipeline.
        ShaderEntry task;
        /// The mesh stage, or absent for a vertex pipeline.
        ShaderEntry mesh;
        /// The fragment stage; absent for depth-only pipelines.
        ShaderEntry fragment;
        /// The topology class; the exact topology is dynamic.
        PrimitiveClass primitive = PrimitiveClass::Triangle;
        /// Rasterizer state.
        RasterState raster;
        /// Depth and stencil state.
        DepthStencilState depthStencil;
        /// The color outputs; entry `i` matches color attachment `i`.
        std::span<const ColorTarget> colors;
        /// The depth attachment format; `Undefined` means no depth attachment.
        Format depthFormat = Format::Undefined;
        /// Samples per texel of every attachment.
        uint32_t samples = 1;
        /// Empty selects a `rojorhi.`-prefixed default.
        std::string_view label;
    };

    /// Describes a compute pipeline, for `Device::createPipeline`. The threadgroup size comes from the entry's
    /// `[numthreads]` attribute. `compute` is present (VU-rojo-pipeline-0013).
    struct ComputePipelineDesc
    {
        /// The compute stage.
        ShaderEntry compute;
        /// Empty selects a `rojorhi.`-prefixed default.
        std::string_view label;
    };

    /// An opaque blob of compiled pipelines: the bytes `Device::pipelineCacheData` returns, passed back through
    /// `DeviceDesc::pipelineCache`. An empty span means no cache. A blob from another backend, device, driver or RojoRHI
    /// version is discarded as a cache miss and reported through the message callback; it is never an error. The bytes are read
    /// during `createDevice`, and the caller keeps them valid and unchanged until it returns.
    using PipelineCache = std::span<const std::byte>;

    /// The single viewport, in framebuffer pixels with the origin at the top-left and `y` growing downwards; clip-space `+y`
    /// points up on every backend. `width` and `height` are greater than zero (VU-rojo-pipeline-0016) and `minDepth` and
    /// `maxDepth` lie in [0, 1] (VU-rojo-pipeline-0017).
    struct Viewport
    {
        float x = 0.0f;        ///< Left edge.
        float y = 0.0f;        ///< Top edge.
        float width = 0.0f;    ///< Width.
        float height = 0.0f;   ///< Height.
        float minDepth = 0.0f; ///< Depth mapped from clip-space zero.
        float maxDepth = 1.0f; ///< Depth mapped from clip-space one.
    };

    /// The exact topology of the draws of a vertex pipeline; it belongs to the `PrimitiveClass` of the bound pipeline
    /// (VU-rojo-pipeline-0023). Primitive restart is not part of 0.1.0.
    enum class PrimitiveTopology : uint8_t
    {
        PointList,     ///< Points; class `Point`.
        LineList,      ///< Lines; class `Line`.
        LineStrip,     ///< Line strips; class `Line`.
        TriangleList,  ///< Triangles; class `Triangle`.
        TriangleStrip, ///< Triangle strips; class `Triangle`.
    };
} // namespace rojoRHI

//======================================================================================================================
// 07. Command recording - docs/spec/07-command-recording*.md

namespace rojoRHI {
    /// The queue a command list is recorded for and submitted to. `Graphics` accepts every command, `Compute` accepts compute,
    /// copy, fill, clear and scope commands, and `Copy` accepts copy and scope commands.
    enum class QueueType : uint8_t
    {
        Graphics, ///< The graphics queue.
        Compute,  ///< The compute queue.
        Copy      ///< The copy queue.
    };

    /// What an attachment holds when a render pass begins.
    enum class LoadOp : uint8_t
    {
        Load,     ///< The attachment's earlier contents.
        Clear,    ///< The attachment's clear value.
        DontCare, ///< Undefined contents the pass must overwrite.
    };

    /// Whether an attachment's rendered contents are kept when the render pass ends. With `DontCare` the contents are undefined
    /// afterwards; a multisample resolve still happens.
    enum class StoreOp : uint8_t
    {
        Store,    ///< Keep the contents.
        DontCare, ///< The contents are undefined afterwards.
    };

    /// The value a color attachment is cleared to; an integer format receives the components converted to integers.
    struct ClearColor
    {
        float r = 0.0f; ///< Red.
        float g = 0.0f; ///< Green.
        float b = 0.0f; ///< Blue.
        float a = 0.0f; ///< Alpha.
    };

    /// One color attachment: a single mip and array layer (a cube face counts as a layer) of `texture`. `clear` is used when
    /// `load` is `Clear`. A non-null `resolve` receives the average of the samples at the end of the pass, written to its mip 0
    /// and layer 0. `texture` was created with `TextureUsage::RenderTarget`, is not a 3D texture, and `mip` and `layer` lie
    /// within it (VU-rojo-cmd-0005); `resolve` obeys VU-rojo-cmd-0071 and its format reports `FormatCapabilities::resolve`
    /// (VU-rojo-format-0003); both are in `Layout::ColorAttachment` at `beginRenderPass` (VU-rojo-cmd-0007).
    struct ColorAttachment
    {
        /// The attachment texture.
        Texture texture;
        /// The mip rendered to.
        uint32_t mip = 0;
        /// The array layer rendered to.
        uint32_t layer = 0;
        /// What the attachment holds at the start of the pass.
        LoadOp load = LoadOp::Load;
        /// Whether the contents are kept at the end of the pass.
        StoreOp store = StoreOp::Store;
        /// The clear value.
        ClearColor clear;
        /// The single-sample texture that receives the resolved samples, or null.
        Texture resolve = {};
    };

    /// The depth and stencil attachment of a render pass; a null `texture` means none. `load`, `store` and `clearDepth` govern
    /// depth; the stencil fields govern stencil and are ignored for formats without one. A non-null `texture` was created with
    /// `TextureUsage::DepthStencil` in a depth or depth-stencil format, and `mip` and `layer` lie within it (VU-rojo-cmd-0008);
    /// `clearDepth` lies in [0, 1] (VU-rojo-cmd-0009); the subresource is in `Layout::DepthStencilWrite` at `beginRenderPass`
    /// (VU-rojo-cmd-0010).
    struct DepthAttachment
    {
        /// The attachment texture, or null for none.
        Texture texture;
        /// The mip rendered to.
        uint32_t mip = 0;
        /// The array layer rendered to.
        uint32_t layer = 0;
        /// What depth holds at the start of the pass.
        LoadOp load = LoadOp::Load;
        /// Whether depth is kept at the end of the pass.
        StoreOp store = StoreOp::Store;
        /// The depth clear value.
        float clearDepth = 0.0f;
        /// What stencil holds at the start of the pass.
        LoadOp stencilLoad = LoadOp::DontCare;
        /// Whether stencil is kept at the end of the pass.
        StoreOp stencilStore = StoreOp::DontCare;
        /// The stencil clear value.
        uint8_t clearStencil = 0;
    };

    /// The attachments of one render pass, the area it renders to and the label of the scope the pass forms; the span and label
    /// are copied. Load and store operations may affect the whole attachment subresource, not only `area`. `colors` holds at
    /// most `Limits::maxColorAttachments` entries and the pass has at least one attachment (VU-rojo-cmd-0011); every attachment
    /// has the same extent at its mip and every attachment other than a resolve target the same sample count (VU-rojo-cmd-0012);
    /// `area` is not empty and lies within the attachments' extent (VU-rojo-cmd-0013); no subresource appears twice
    /// (VU-rojo-cmd-0014); the color formats fit `Limits::maxColorBytesPerSample` (VU-rojo-cmd-0015).
    struct RenderPassDesc
    {
        /// The color attachments, in the order the pipeline's color targets name them.
        std::span<const ColorAttachment> colors;
        /// The depth and stencil attachment; a null texture means none.
        DepthAttachment depth;
        /// The rectangle rendered to, in pixels.
        Rect2D area;
        /// The label of the pass's scope.
        std::string_view label;
    };

    /// The width of each index in an index buffer: 16 or 32 bits.
    enum class IndexType : uint8_t
    {
        UInt16, ///< 16-bit indices.
        UInt32, ///< 32-bit indices.
    };

    /// How texel data is laid out in a buffer for `CommandList::copyBufferToTexture` and `CommandList::copyTextureToBuffer`.
    /// Depth slices follow each other, then layers, each `rowPitch * rowsPerImage` bytes apart. `offset` is a multiple of 512
    /// (VU-rojo-cmd-0034); `rowPitch` is a multiple of 256 and at least one row of the region's texel blocks
    /// (VU-rojo-cmd-0035); `rowsPerImage` is zero or at least the region's height in texel blocks (VU-rojo-cmd-0036); when the
    /// region spans more than one image, the distance between images is a multiple of 512 (VU-rojo-cmd-0037).
    struct BufferTextureLayout
    {
        /// Bytes from the buffer start to the first texel block.
        uint64_t offset = 0;
        /// Bytes between rows of texel blocks.
        uint32_t rowPitch = 0;
        /// Rows of blocks between depth slices and layers; 0 is the region height.
        uint32_t rowsPerImage = 0;
    };

    /// A box of texels in one mip of a range of array layers. `mip` is below the texture's mip count, `layers` is at least one
    /// and `layer + layers` is at most its layer count (VU-rojo-cmd-0038); `extent` is non-zero and `offset + extent` lies
    /// within the mip (VU-rojo-cmd-0039); for a block-compressed format `offset` is a multiple of the block size and `extent`
    /// is a multiple of it or reaches the mip's edge (VU-rojo-cmd-0040); the format has no stencil component
    /// (VU-rojo-cmd-0041).
    struct TextureRegion
    {
        /// Mip level.
        uint32_t mip = 0;
        /// First array layer.
        uint32_t layer = 0;
        /// Number of array layers.
        uint32_t layers = 1;
        /// First texel.
        Offset3D offset;
        /// Size in texels.
        Extent3D extent;
    };

    /// The value `CommandList::clearTexture` writes: `color` for color formats, `depth` and `stencil` for depth and
    /// depth-stencil formats. `depth` lies in [0, 1] (VU-rojo-cmd-0059).
    struct ClearValue
    {
        /// Written to a color format.
        ClearColor color;
        /// Written to a depth plane.
        float depth = 0.0f;
        /// Written to a stencil plane.
        uint8_t stencil = 0;
    };
} // namespace rojoRHI

//======================================================================================================================
// 08. GPU-driven rendering - docs/spec/08-gpu-driven.md

namespace rojoRHI {
    /// One non-indexed draw read by `CommandList::drawIndirect`; 20 bytes, 4-byte aligned.
    struct DrawIndirectArgs
    {
        /// Caller-defined value that `drawId()` returns in every invocation of this draw.
        uint32_t drawId;
        /// Vertices per instance; zero makes the record a no-op.
        uint32_t vertexCount;
        /// Instances to draw.
        uint32_t instanceCount;
        /// First vertex drawn.
        uint32_t firstVertex;
        /// First instance index.
        uint32_t firstInstance;
    };

    /// One indexed draw read by `CommandList::drawIndexedIndirect`; 24 bytes, 4-byte aligned.
    struct DrawIndexedIndirectArgs
    {
        /// Caller-defined value that `drawId()` returns in every invocation of this draw.
        uint32_t drawId;
        /// Indices per instance.
        uint32_t indexCount;
        /// Instances to draw.
        uint32_t instanceCount;
        /// First element read from the index buffer.
        uint32_t firstIndex;
        /// Signed value added to each fetched index.
        int32_t baseVertex;
        /// First instance index.
        uint32_t firstInstance;
    };

    /// One mesh-task draw read by `CommandList::drawMeshTasksIndirect`; 16 bytes, 4-byte aligned.
    struct DrawMeshTasksIndirectArgs
    {
        /// Caller-defined value that `drawId()` returns in every invocation of this draw.
        uint32_t drawId;
        /// Threadgroup count in x; any zero count makes the record a no-op.
        uint32_t x;
        /// Threadgroup count in y.
        uint32_t y;
        /// Threadgroup count in z.
        uint32_t z;
    };

    /// One compute dispatch read by `CommandList::dispatchIndirect`; 12 bytes, 4-byte aligned.
    struct DispatchIndirectArgs
    {
        /// Threadgroup count in x; any zero count makes the dispatch a no-op.
        uint32_t x;
        /// Threadgroup count in y.
        uint32_t y;
        /// Threadgroup count in z.
        uint32_t z;
    };
} // namespace rojoRHI

//======================================================================================================================
// 09. Synchronization - docs/spec/09-synchronization*.md

namespace rojoRHI {
    /// A bit set of pipeline stages, modelled on D3D12 enhanced barriers and Vulkan synchronization2. As a source `None` means no
    /// earlier work, as a destination no later work. A combined bit lowers to the union of its members' mappings.
    enum class Stage : uint32_t
    {
        None = 0,                  ///< No stage.
        IndirectArgs = 1u << 0,    ///< Reading indirect argument records and draw counts.
        IndexInput = 1u << 1,      ///< Fetching indices for indexed draws.
        VertexShader = 1u << 2,    ///< Vertex shader execution.
        TaskShader = 1u << 3,      ///< Task shader execution.
        MeshShader = 1u << 4,      ///< Mesh shader execution.
        FragmentShader = 1u << 5,  ///< Fragment shader execution.
        ComputeShader = 1u << 6,   ///< Compute shader execution.
        ColorAttachment = 1u << 7, ///< Color attachment loads, blending and stores, including load-operation clears.
        DepthStencil = 1u << 8,    ///< Depth and stencil tests, writes and load-operation clears.
        Copy = 1u << 9,            ///< Every copy command, `fillBuffer` and `clearTexture`.
        Resolve = 1u << 10,        ///< The multisample resolve at the end of a render pass.
        /// Every stage a render pass and its draws use.
        AllGraphics = IndirectArgs | IndexInput | VertexShader | TaskShader | MeshShader | FragmentShader | ColorAttachment | DepthStencil | Resolve,
        All = (1u << 11) - 1, ///< Every stage.
    };

    /// Returns the union of two stage sets.
    constexpr Stage operator|(Stage a, Stage b)
    {
        return static_cast<Stage>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
    }

    /// Returns the stages both sets contain.
    constexpr Stage operator&(Stage a, Stage b)
    {
        return static_cast<Stage>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
    }

    /// Returns the complement of a stage set within the 32-bit mask.
    constexpr Stage operator~(Stage a)
    {
        return static_cast<Stage>(~static_cast<uint32_t>(a));
    }

    /// A bit set of memory accesses; each bit is performed at the stages named after it. `AllGraphics` and `All` perform every
    /// access their stages perform.
    enum class Access : uint32_t
    {
        None = 0,                       ///< No memory access; the side orders execution only.
        IndirectArgs = 1u << 0,         ///< Reads of argument records and counts, at `Stage::IndirectArgs`.
        IndexRead = 1u << 1,            ///< Index fetch, at `Stage::IndexInput`.
        ShaderRead = 1u << 2,           ///< Shader reads through an `Addr`, a view or a root block, at any shader stage.
        ShaderWrite = 1u << 3,          ///< Shader writes and atomics through an `Addr` or a `Storage` view, at any shader stage.
        ColorAttachmentRead = 1u << 4,  ///< Attachment loads and blending, at `Stage::ColorAttachment`.
        ColorAttachmentWrite = 1u << 5, ///< Attachment stores and clears, at `Stage::ColorAttachment`.
        DepthStencilRead = 1u << 6,     ///< Depth and stencil tests, at `Stage::DepthStencil`.
        DepthStencilWrite = 1u << 7,    ///< Depth and stencil writes and clears, at `Stage::DepthStencil`.
        CopyRead = 1u << 8,             ///< Copy sources, at `Stage::Copy`.
        CopyWrite = 1u << 9,            ///< Copy, fill and clear destinations, at `Stage::Copy`.
        Resolve = 1u << 10,             ///< Writes to a resolve target, at `Stage::Resolve`.
    };

    /// Returns the union of two access sets.
    constexpr Access operator|(Access a, Access b)
    {
        return static_cast<Access>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
    }

    /// Returns the accesses both sets contain.
    constexpr Access operator&(Access a, Access b)
    {
        return static_cast<Access>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
    }

    /// Returns the complement of an access set within the 32-bit mask.
    constexpr Access operator~(Access a)
    {
        return static_cast<Access>(~static_cast<uint32_t>(a));
    }

    /// The layout of a texture subresource: the D3D12 enhanced-barrier core, mapped onto Vulkan image layouts and ignored by
    /// Metal 4. Buffers have no layout. The caller states both the layout a subresource is in and the one it moves to; a new
    /// texture starts `Undefined`, or `ShaderRead` when created with initial data.
    enum class Layout : uint8_t
    {
        Undefined,         ///< Contents are undefined; valid only before a barrier, which discards the contents.
        General,           ///< Shader reads, copy reads and copy writes; the layout of a cross-queue handoff.
        ShaderRead,        ///< Shader reads through `Sampled` views.
        ShaderWrite,       ///< Shader reads and writes through `Storage` views.
        ColorAttachment,   ///< Color attachment use, and the target of a render-pass resolve.
        DepthStencilWrite, ///< Depth-stencil attachment use with or without writes.
        DepthStencilRead,  ///< Shader reads of a depth-stencil texture through `Sampled` views.
        CopySrc,           ///< Copy source.
        CopyDst,           ///< Copy, fill and clear destination.
        Present,           ///< Handed to presentation; permits no other use.
    };

    /// A memory dependency over all memory, with no resource named and no layout changed. `srcStage` and `dstStage` are not both
    /// `Stage::None` (VU-rojo-sync-0008); the sides obey VU-rojo-sync-0001 to 0003.
    struct GlobalBarrier
    {
        /// Stages of earlier work the barrier waits for.
        Stage srcStage = Stage::None;
        /// Accesses of earlier work the barrier makes visible.
        Access srcAccess = Access::None;
        /// Stages of later work that wait.
        Stage dstStage = Stage::None;
        /// Accesses of later work that see the source writes.
        Access dstAccess = Access::None;
    };

    /// A `BufferBarrier::size` that extends the range to the end of the buffer.
    inline constexpr uint64_t kWholeSize = ~uint64_t{ 0 };

    /// A memory dependency scoped to one range of one buffer. `offset` is below the buffer's size and `size` is `kWholeSize` or
    /// above zero with `offset + size` within the buffer (VU-rojo-sync-0010); the accesses are buffer accesses
    /// (VU-rojo-sync-0011); the sides obey VU-rojo-sync-0001 to 0003.
    struct BufferBarrier
    {
        /// The buffer whose memory the barrier orders.
        Buffer buffer;
        /// First byte of the range.
        uint64_t offset = 0;
        /// Bytes of the range, or `kWholeSize`.
        uint64_t size = kWholeSize;
        /// Stages of earlier work the barrier waits for.
        Stage srcStage = Stage::None;
        /// Accesses of earlier work the barrier makes visible.
        Access srcAccess = Access::None;
        /// Stages of later work that wait.
        Stage dstStage = Stage::None;
        /// Accesses of later work that see the source writes.
        Access dstAccess = Access::None;
    };

    /// A memory dependency and layout transition for a range of one texture. `range` obeys VU-rojo-texture-0016, 0017 and 0041;
    /// `newLayout` is not `Undefined` (VU-rojo-sync-0014) and is allowed by the texture's usage (VU-rojo-sync-0015); the
    /// accesses are permitted by their layouts (VU-rojo-sync-0016); the sides obey VU-rojo-sync-0001 to 0003. `oldLayout` is
    /// checked by the validation tracker (VU-rojo-sync-0013).
    struct TextureBarrier
    {
        /// The texture whose subresources the barrier orders.
        Texture texture;
        /// The mips and layers covered.
        SubresourceRange range;
        /// Stages of earlier work the barrier waits for.
        Stage srcStage = Stage::None;
        /// Accesses of earlier work the barrier makes visible.
        Access srcAccess = Access::None;
        /// Stages of later work that wait.
        Stage dstStage = Stage::None;
        /// Accesses of later work that see the source writes.
        Access dstAccess = Access::None;
        /// The layout every covered subresource is in; `Undefined` discards the contents.
        Layout oldLayout = Layout::Undefined;
        /// The layout every covered subresource is in after the barrier.
        Layout newLayout = Layout::Undefined;
    };
} // namespace rojoRHI

//======================================================================================================================
// 10. Queues, fences and frames - docs/spec/10-queues-and-frames.md

namespace rojoRHI {
    /// A point on a fence's timeline, used as a wait or a signal by `Queue::submit` and as a target by `Device::wait`.
    struct FenceValue
    {
        Fence fence;    ///< A fence created by `Device::createFence`.
        uint64_t value; ///< The timeline value.
    };
} // namespace rojoRHI

//======================================================================================================================
// 11. Presentation - docs/spec/11-presentation.md

namespace rojoRHI {
    /// How the display interprets the values written to a swapchain image. Each color space admits one format: `SRGB` takes
    /// `Format::BGRA8Unorm`, `ExtendedLinear` takes `Format::RGBA16Float` and `HDR10` takes `Format::RGB10A2Unorm`
    /// (VU-rojo-present-0003).
    enum class ColorSpace : uint8_t
    {
        SRGB,           ///< Standard dynamic range, sRGB primaries and transfer; the shader writes sRGB-encoded values.
        ExtendedLinear, ///< Extended dynamic range: linear, sRGB primaries, values above 1.0 brighter than SDR white.
        HDR10,          ///< BT.2020 primaries, SMPTE ST 2084 (PQ) transfer.
    };

    /// When a presented image reaches the display. A mode the surface or display cannot provide makes
    /// `Device::createSwapchain` return an error; `Fifo` is always available.
    enum class PresentMode : uint8_t
    {
        Fifo,      ///< Each image is shown for at least one vertical blank, in present order; never tears.
        Mailbox,   ///< Shown at the next vertical blank; a newer present replaces a queued one; does not tear.
        Immediate, ///< Shown as soon as possible; may tear.
    };

    /// Describes a swapchain for `Device::createSwapchain`: the consumer-owned surface its images are displayed on and how
    /// they are shown. `nativeSurface` is not null (VU-rojo-present-0001) and is the surface type of the compiled backend
    /// (VU-rojo-present-0002); `format` is the one `colorSpace` admits (VU-rojo-present-0003); `imageCount` is 2 or 3
    /// (VU-rojo-present-0004); `maxLatency` is at least 1 and at most `imageCount` (VU-rojo-present-0005).
    struct SwapchainDesc
    {
        /// The surface: a `CA::MetalLayer*` on Metal 4, an `HWND` on D3D12.
        void* nativeSurface = nullptr;
        /// Format of every image; forms a pair with `colorSpace`.
        Format format = Format::BGRA8Unorm;
        /// How the display interprets image values.
        ColorSpace colorSpace = ColorSpace::SRGB;
        /// Images the swapchain rotates through.
        uint32_t imageCount = 3;
        /// When a presented image reaches the display.
        PresentMode mode = PresentMode::Fifo;
        /// Most presents of this swapchain that may be queued and not yet displayed; `acquire` waits while it is reached.
        uint32_t maxLatency = 2;
        /// Debug label; empty selects `rojorhi.swapchain.unnamed`.
        std::string_view label;
    };
} // namespace rojoRHI

//======================================================================================================================
// 12. Upscaling - docs/spec/12-upscaling.md

namespace rojoRHI {
    /// Selects an upscaler implementation. Requesting a kind that is not compiled in, or that the device or driver refuses,
    /// is an error at creation, never a silent substitution.
    enum class UpscalerKind : uint8_t
    {
        Auto,    ///< The backend's preferred available kind.
        MetalFX, ///< Apple MetalFX temporal scaler.
        DLSS,    ///< NVIDIA DLSS super resolution.
        FSR,     ///< AMD FidelityFX Super Resolution, temporal.
        XeSS,    ///< Intel XeSS super resolution.
    };

    /// A bit set fixed at creation by `UpscalerDesc::flags`.
    enum class UpscalerFlags : uint32_t
    {
        None = 0,                ///< No flag.
        HDR = 1u << 0,           ///< Color input is high dynamic range scene-linear, not display-referred.
        ReversedDepth = 1u << 1, ///< Larger depth values are nearer the camera.
        AutoExposure = 1u << 2,  ///< The implementation computes exposure itself; `UpscaleParams::exposure` is ignored.
    };

    /// Returns the union of two flag sets.
    constexpr UpscalerFlags operator|(UpscalerFlags a, UpscalerFlags b)
    {
        return static_cast<UpscalerFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
    }

    /// Returns the flags both sets contain.
    constexpr UpscalerFlags operator&(UpscalerFlags a, UpscalerFlags b)
    {
        return static_cast<UpscalerFlags>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
    }

    /// Describes an upscaler for `Device::createUpscaler`. `input` is greater than zero in both axes (VU-rojo-upscale-0001) and
    /// `output` is at least `input` in both (VU-rojo-upscale-0002); `color` and `outputFormat` are color formats, `depth` is a
    /// depth format and `motion` is a two-channel floating-point format (VU-rojo-upscale-0003). The active content inside
    /// `input` may shrink per frame through `UpscaleParams::inputContent`.
    struct UpscalerDesc
    {
        /// Implementation to create.
        UpscalerKind kind = UpscalerKind::Auto;
        /// Allocated extent of the color, depth, motion and reactive inputs, in texels.
        Extent2D input;
        /// Extent of the reconstructed output, in texels.
        Extent2D output;
        /// Format of the color input.
        Format color = Format::RGBA16Float;
        /// Format of the depth input.
        Format depth = Format::D32Float;
        /// Format of the two-channel motion input.
        Format motion = Format::RG16Float;
        /// Format of the reconstructed output.
        Format outputFormat = Format::RGBA16Float;
        /// Creation-time behaviour.
        UpscalerFlags flags = UpscalerFlags::None;
        /// Diagnostic label, copied at creation; empty selects `rojorhi.upscaler.unnamed`.
        std::string_view label;
    };

    /// The arguments of `CommandList::upscale`. `preExposure` is greater than zero (VU-rojo-upscale-0004) and `sharpness` is
    /// from 0 to 1 (VU-rojo-upscale-0005); the textures obey VU-rojo-upscale-0009 to 0015.
    struct UpscaleParams
    {
        /// Jittered color input; scene-linear and pre-exposed by `preExposure` with `UpscalerFlags::HDR`.
        Texture color;
        /// This frame's depth.
        Texture depth;
        /// Per-pixel motion, scaled by `motionScaleX` and `motionScaleY`; it points from the current position to the previous
        /// one, in input pixels.
        Texture motion;
        /// Reconstructed result at the descriptor's `output` extent.
        Texture output;
        /// Optional 1x1 exposure multiplier for the color input; null means 1.0; ignored with `UpscalerFlags::AutoExposure`.
        Texture exposure = {};
        /// Optional per-pixel weight; 1.0 asks for no history accumulation; null means none.
        Texture reactive = {};
        /// Active origin-anchored rectangle of the inputs; zero in both axes means the whole `input` extent.
        Extent2D inputContent = {};
        /// Sub-pixel raster jitter of this frame in x, in input texels, positive down.
        float jitterX = 0.0f;
        /// Sub-pixel raster jitter of this frame in y, in input texels, positive down.
        float jitterY = 0.0f;
        /// Converts stored motion to input pixels along x.
        float motionScaleX = 1.0f;
        /// Converts stored motion to input pixels along y.
        float motionScaleY = 1.0f;
        /// Positive multiplier already applied to the color input.
        float preExposure = 1.0f;
        /// Sharpening strength from 0 to 1; implementations without a sharpening pass ignore it.
        float sharpness = 0.0f;
        /// Discards the history before this frame, as after a camera cut.
        bool reset = false;
        /// Debug-group label of the operation in captures and scope timings, copied while recording; empty selects
        /// `rojorhi.upscale.unnamed`.
        std::string_view label;
    };
} // namespace rojoRHI

//======================================================================================================================
// 13. Sparse resources - docs/spec/13-sparse.md

namespace rojoRHI {
    /// The shape of the tiles of a sparse resource and of its mip tail, which `Device::tileShape` returns. Every heap offset and
    /// buffer range of a mapping is a multiple of `bytes`.
    struct TileShape
    {
        /// Texels one tile covers along x; for a buffer, the tile size in bytes.
        uint32_t width = 0;
        /// Texels one tile covers along y; 1 for a buffer.
        uint32_t height = 0;
        /// Texels one tile covers along z; 1 for a buffer.
        uint32_t depth = 0;
        /// Heap bytes one tile maps.
        uint32_t bytes = 0;
        /// First mip level packed into the mip tail; the texture's mip count when it has no tail; zero for a buffer.
        uint32_t mipTailFirst = 0;
        /// Heap bytes the mip tail of one layer needs; zero when there is no tail.
        uint64_t mipTailBytes = 0;
    };

    /// One change to the mapping of a texture region, a texture's mip tail, or a buffer byte range, applied by
    /// `Queue::mapTiles`. Exactly one of `texture` and `buffer` is set (VU-rojo-sparse-0002) and it was created with `Sparse`
    /// usage (VU-rojo-sparse-0003). A texture region lies in one mip level below the tile shape's `mipTailFirst`, aligned to
    /// the tile shape (VU-rojo-sparse-0004); with `mipTail` only the layer range of `region` is read and the texture has a tail
    /// (VU-rojo-sparse-0005); `mipTail` is false for a buffer (VU-rojo-sparse-0006). A buffer range is a multiple of the tile
    /// size or reaches the buffer's end and lies inside it (VU-rojo-sparse-0007). Without `unmap`, `heap` is a `Memory::Gpu`
    /// heap (VU-rojo-sparse-0009), `heapOffset` is a multiple of the tile bytes and every mapped tile lies inside the heap
    /// (VU-rojo-sparse-0008); with `unmap`, `heap` is null and `heapOffset` is zero (VU-rojo-sparse-0010). The tiles of a
    /// region map onto consecutive heap tiles in order of x, then y, then z, then layer; mip tails map one after another, one
    /// per layer, each `TileShape::mipTailBytes` long; a buffer range maps onto consecutive heap bytes.
    struct TileMapping
    {
        /// Sparse texture to map, or null when `buffer` is set.
        Texture texture = {};
        /// Texels of one mip level, over a range of layers; with `mipTail` only its layer range is read.
        TextureRegion region;
        /// Maps the mip tail of each of `region`'s layers instead of a region.
        bool mipTail = false;
        /// Sparse buffer to map, or null when `texture` is set.
        Buffer buffer = {};
        /// Byte offset of the range of `buffer` to map.
        uint64_t bufferOffset = 0;
        /// Byte size of the range of `buffer` to map.
        uint64_t bufferSize = 0;
        /// Heap supplying the memory; null with `unmap`.
        Heap heap = {};
        /// Byte offset of the first tile in `heap`.
        uint64_t heapOffset = 0;
        /// Removes the mapping of the named tiles instead of adding one.
        bool unmap = false;
    };
} // namespace rojoRHI

//======================================================================================================================
// 14. Diagnostics - docs/spec/14-diagnostics.md

namespace rojoRHI {
    /// One shader access the next draw or dispatch makes through a heap index or an `Addr`, declared through
    /// `CommandList::declareAccess` so the validation tracker can check it. Exactly one of `buffer` and `texture` is not null
    /// (VU-rojo-diag-0001); the range lies inside the resource (VU-rojo-diag-0002); `stage` and `access` are shader ones
    /// (VU-rojo-diag-0003).
    struct ResourceAccess
    {
        /// The buffer accessed; null when `texture` is set.
        Buffer buffer = {};
        /// Byte offset of the accessed range in `buffer`.
        uint64_t offset = 0;
        /// Bytes of the range, or `kWholeSize` for the rest of the buffer.
        uint64_t size = kWholeSize;
        /// The texture accessed; null when `buffer` is set.
        Texture texture = {};
        /// The subresources of `texture` accessed.
        SubresourceRange range;
        /// The shader stage that makes the access: exactly one of the five shader stages.
        Stage stage;
        /// Shader read, shader write, or both.
        Access access;
    };

    /// Plain integer counters of the work a device does, always compiled in and each maintained by a single increment. They
    /// count from device creation and never reset; a caller measures an interval by subtracting two snapshots from
    /// `Device::counters`. Every field means the same on every backend.
    struct Counters
    {
        uint64_t nativeCreateCalls = 0; ///< Native calls that create or release objects, views and memory.
        uint64_t nativeRecordCalls = 0; ///< Native calls recorded into command buffers: state, draws, dispatches, copies, barriers.
        uint64_t nativeSubmitCalls = 0; ///< Native queue calls: submission, presentation, tile mapping, fence signals and waits.
        uint64_t nativeHostCalls = 0;   ///< Other native calls on the CPU: residency commits, fence queries, timestamp reads.
        /// CPU heap allocations RojoRHI makes at explicit sites: each `new`, `make_unique` and host memory block the library
        /// allocates itself. Growth inside standard containers is not counted, and no allocator is replaced to count it.
        uint64_t cpuAllocations = 0;
        uint64_t gpuAllocations = 0;              ///< Native memory objects created: buffers, textures, heaps and frame-arena pages.
        uint64_t frameDataUploads = 0;            ///< `CommandList::uploadFrameData` calls.
        uint64_t frameArenaBytes = 0;             ///< Bytes those calls copied, excluding alignment padding.
        uint64_t frameArenaGrowths = 0;           ///< Frame-arena growth events.
        uint64_t descriptorWrites = 0;            ///< Heap-slot writes.
        uint64_t globalBarriers = 0;              ///< Global barrier records passed to `CommandList::barrier`.
        uint64_t bufferBarriers = 0;              ///< Buffer barrier records passed to `CommandList::barrier`.
        uint64_t textureBarriers = 0;             ///< Texture barrier records passed to `CommandList::barrier`.
        uint64_t nativeBarriers = 0;              ///< Native barrier commands emitted after batching.
        uint64_t pipelinesCreated = 0;            ///< Pipelines created, including any variant a backend creates on its own.
        uint64_t pipelineCacheHits = 0;           ///< Pipeline creations served by the pipeline cache.
        uint64_t pipelineCacheMisses = 0;         ///< Pipeline creations not served by the pipeline cache.
        uint64_t pipelineCompileNanoseconds = 0;  ///< CPU time spent in pipeline creation; the one timed field, not exact.
        uint64_t encodersOpened = 0;              ///< Native encoders on Metal 4; render passes begun elsewhere.
        uint64_t deferredDestructionsRetired = 0; ///< Deferred destructions whose native release has run.
    };

    /// The GPU duration of one debug scope, as `Device::scopeTimings` reports it; every render pass and upscale forms a scope
    /// of its own. Durations are wall time: nested scopes overlap their parent, and scopes on different queues overlap each
    /// other.
    struct ScopeTiming
    {
        /// The scope's label, or the render pass's or upscale's, with the default label substituted for an empty one.
        std::string_view label;
        /// Number of the frame the scope was recorded in, counting `Device::beginFrame` calls from one.
        uint64_t frame = 0;
        /// Queue type of the command list that recorded the scope.
        QueueType queue = QueueType::Graphics;
        /// Nesting depth; zero for an outermost scope.
        uint32_t depth = 0;
        /// GPU wall time between the scope's two timestamps, in milliseconds; zero on the Null backend.
        double gpuMilliseconds = 0.0;
    };
} // namespace rojoRHI

//======================================================================================================================
// 15. Formats and limits - docs/spec/15-formats-and-limits*.md

namespace rojoRHI {
    /// What one device supports for one format, fixed for the lifetime of the device. A member is `true` only when every
    /// operation it names is supported for every texture type the device can create in that format; every value is at least
    /// the guaranteed capability the reference states.
    struct FormatCapabilities
    {
        bool sampled = false;         ///< Readable through a Sampled view.
        bool filterLinear = false;    ///< Linear minification and magnification filtering.
        bool storageRead = false;     ///< Readable through a Storage view.
        bool storageWrite = false;    ///< Writable through a Storage view.
        bool colorAttachment = false; ///< Usable as a ColorAttachment texture.
        bool blend = false;           ///< Blendable as a color attachment.
        bool depthAttachment = false; ///< Usable as a DepthAttachment texture.
        bool resolve = false;         ///< Usable as a ColorAttachment resolve target.
        bool sparse = false;          ///< Creatable with TextureUsage::Sparse.
        uint32_t sampleCounts = 1;    ///< Bit i set: 1 << i samples supported; bit 0 always set.
    };

    /// The numeric limits every other entry checks its arguments against; fixed for the lifetime of the device and at least
    /// the floor the reference states for each field.
    struct Limits
    {
        uint32_t maxViews = 0;                ///< ViewIndex slots in the resource heap.
        uint32_t maxSamplers = 0;             ///< SamplerIndex slots in the sampler heap.
        uint32_t maxRootBlockSize = 0;        ///< Bytes of one root block.
        uint32_t rootAlignment = 0;           ///< Byte alignment of a root block address.
        uint32_t frameDataAlignment = 0;      ///< Default uploadFrameData alignment.
        uint64_t maxBufferSize = 0;           ///< Bytes of one buffer.
        uint64_t maxAddrOffset = 0;           ///< Bytes an Addr reaches from its buffer's start.
        uint32_t maxTextureDimension1D = 0;   ///< Texels.
        uint32_t maxTextureDimension2D = 0;   ///< Texels, width and height.
        uint32_t maxTextureDimension3D = 0;   ///< Texels, each axis.
        uint32_t maxTextureDimensionCube = 0; ///< Texels, face width and height.
        uint32_t maxTextureLayers = 0;        ///< Array layers, six per cube.
        uint32_t maxColorAttachments = 0;     ///< ColorAttachment entries in one render pass.
        uint32_t maxColorBytesPerSample = 0;  ///< Summed attachment storage per sample.
        uint32_t maxSamplerAnisotropy = 0;    ///< SamplerDesc anisotropy.
        uint32_t maxDrawIndirectCount = 0;    ///< maxCount of one indirect draw call.
        uint32_t maxDispatchGroups = 0;       ///< Threadgroups per dispatch dimension.
        uint32_t maxThreadsPerGroup = 0;      ///< Threads in one compute threadgroup.
        uint32_t maxGroupSize[3] = {};        ///< Threads per threadgroup axis, x, y, z.
        uint32_t maxGroupSharedMemory = 0;    ///< Bytes of threadgroup (groupshared) memory.
        uint32_t maxMeshTaskGroups = 0;       ///< Mesh task groups per dimension.
        uint32_t maxMeshTaskGroupsTotal = 0;  ///< Product of the three mesh task dimensions.
    };
} // namespace rojoRHI

//======================================================================================================================
// 02. Device: Capabilities - docs/spec/02-device.md
// Placed after 15 because it names Limits.

namespace rojoRHI {
    /// What a device supports, fixed for its lifetime. Every string and span points at device-owned storage valid until
    /// `destroyDevice`. Each `bool` gates the entries it names: using a gated feature on a device that reports `false` is a
    /// valid-usage violation of the owning chapter.
    struct Capabilities
    {
        std::string_view adapterName;            ///< The GPU's name as the native API reports it.
        std::string_view floorName;              ///< The floor met, e.g. "Apple9" or "FL 12_2, SM 6.8".
        uint32_t maxViews = 0;                   ///< Shader-visible texture views a shader can index.
        uint32_t maxSamplers = 0;                ///< Shader-visible samplers a shader can index.
        bool uploadInDeviceMemory = false;       ///< Memory::Upload is device-local and CPU-writable.
        bool placementSparse = false;            ///< BufferUsage::Sparse and TextureUsage::Sparse work.
        bool meshTasksIndirect = false;          ///< CommandList::drawMeshTasksIndirect works.
        bool conservativeRaster = false;         ///< RasterState conservative rasterization works.
        std::span<const UpscalerKind> upscalers; ///< Upscaler kinds Device::createUpscaler accepts.
        Limits limits;                           ///< Numeric limits.
    };
} // namespace rojoRHI

//======================================================================================================================
// 01. Fundamentals: object handles - docs/spec/01-fundamentals*.md
// Placed last because the members of CommandList, Queue and Device name types from every chapter.

namespace rojoRHI {
    /// A recorded sequence of commands for one queue type. It wraps one backend pointer: copies refer to the same list, and a
    /// default-constructed one refers to none (VU-rojo-core-0004). It comes from a per-frame pool and returns to it when its
    /// frame retires; using it after that is undefined behaviour no tier detects. It is used only with the device that
    /// acquired it (VU-rojo-core-0005). One thread records one list at a time. In Release builds recording takes no locks,
    /// except when a list claims a new frame-arena page for `uploadFrameData` or for the label of a debug scope; a Debug-tier
    /// check may take the lock of the pool it reads (ADR 0020).
    ///
    /// Every command other than `end` is recorded on a list that is recording (VU-rojo-cmd-0063).
    class CommandList
    {
      public:
        /// Ends recording; the list then records nothing further and is ready for `Queue::submit`. The list is recording
        /// (VU-rojo-cmd-0002), no render pass is open (VU-rojo-cmd-0003) and every scope begun on it has ended
        /// (VU-rojo-cmd-0004). Thread class command list.
        void end();

        /// Copies `bytes` into the open frame's arena at an address aligned to `alignment` and returns that address, valid
        /// only within the frame that produced it. `bytes` is not empty (VU-rojo-buffer-0015), `alignment` is a power of two
        /// (VU-rojo-buffer-0016), a frame is open (VU-rojo-buffer-0017) and only work submitted before that frame's
        /// `Device::endFrame` reads the address; a command that takes the address is recorded on a list of the frame that
        /// produced it (VU-rojo-buffer-0020). Thread class command list.
        Addr uploadFrameData(std::span<const std::byte> bytes, uint32_t alignment = 256);

        /// Begins a render pass: performs the load operations and no layout transition, forms a scope labeled `desc.label` and
        /// starts the dynamic state as chapter 06 lists it, with no pipeline bound. Recorded on a `Graphics` list with no
        /// render pass open (VU-rojo-cmd-0016); `desc` obeys VU-rojo-cmd-0005 to 0015. Thread class command list.
        void beginRenderPass(const RenderPassDesc& desc);

        /// Ends the open render pass: performs the store operations and resolves, closes the pass's scope and unbinds the
        /// pipeline. A render pass is open (VU-rojo-cmd-0018) and every scope begun inside it has ended (VU-rojo-cmd-0019).
        /// Thread class command list.
        void endRenderPass();

        /// Opens a debug scope labeled `label`, inside or outside a render pass; scopes nest. Thread class command list.
        void beginScope(std::string_view label);

        /// Closes the innermost open scope. A scope is open (VU-rojo-cmd-0032) and the innermost one began on the same side of
        /// the render pass boundary as this call (VU-rojo-cmd-0033). Thread class command list.
        void endScope();

        /// Binds a graphics pipeline inside a render pass or a compute pipeline outside one; binding a graphics pipeline resets
        /// the topology to the list form of its class. `pipeline` is live (VU-rojo-cmd-0020); a graphics pipeline is bound
        /// inside a render pass of a `Graphics` list and matches the pass's attachment formats and sample count
        /// (VU-rojo-cmd-0021); a compute pipeline is bound outside a render pass of a `Graphics` or `Compute` list
        /// (VU-rojo-cmd-0022). Thread class command list.
        void setPipeline(Pipeline pipeline);

        /// Sets the single viewport. Recorded inside a render pass (VU-rojo-pipeline-0015); `viewport` obeys
        /// VU-rojo-pipeline-0016 and 0017. Thread class command list.
        void setViewport(const Viewport& viewport);

        /// Sets the single scissor rectangle. Recorded inside a render pass (VU-rojo-pipeline-0018); `rect` lies within the
        /// pass's area (VU-rojo-pipeline-0019). Thread class command list.
        void setScissor(const Rect2D& rect);

        /// Sets the stencil reference for both faces. Recorded inside a render pass (VU-rojo-pipeline-0020). Thread class command
        /// list.
        void setStencilReference(uint8_t reference);

        /// Sets the color `BlendFactor::Constant` reads. Recorded inside a render pass (VU-rojo-pipeline-0021). Thread class
        /// command list.
        void setBlendConstants(float r, float g, float b, float a);

        /// Sets the exact topology of the following draws. Recorded inside a render pass with a graphics pipeline bound that has
        /// a vertex stage (VU-rojo-pipeline-0022); `topology` belongs to that pipeline's class (VU-rojo-pipeline-0023). Thread
        /// class command list.
        void setPrimitiveTopology(PrimitiveTopology topology);

        /// Sets the depth bias added to each fragment's depth. Recorded inside a render pass (VU-rojo-pipeline-0024) with a
        /// graphics pipeline bound that was created with `RasterState::depthBias` (VU-rojo-pipeline-0025). Thread class command
        /// list.
        void setDepthBias(float constant, float slopeScaled, float clamp);

        /// Binds `root` as the root block of every later draw and dispatch recorded on this list, until the next `setRoot` or
        /// `pushRoot`; only the address is captured. `root` is not null (VU-rojo-abi-0001), names a live buffer and lies
        /// inside it (VU-rojo-buffer-0009), and its byte offset in that buffer is a multiple of 256 (VU-rojo-abi-0002). The
        /// buffer stays alive until every draw and dispatch that reads the root completes, or the root is frame data of the
        /// frame being recorded (VU-rojo-abi-0003, checked by the validation tracker). Thread class command list.
        void setRoot(Addr root);

        /// Copies `value` into the open frame's arena at 256-byte alignment, binds the copy as `setRoot` does and returns its
        /// address, valid only within the current frame. It is `uploadFrameData` of the bytes of `value` followed by `setRoot`
        /// of the result, with the valid usage of both. `T` is trivially copyable and standard-layout and at most 65536
        /// bytes; the build-time layout check rejects a root type that breaks the root layout rules. Thread class command
        /// list.
        template<class T> Addr pushRoot(const T& value)
        {
            static_assert(std::is_trivially_copyable_v<T>, "a root type is trivially copyable");
            static_assert(std::is_standard_layout_v<T>, "a root type is standard-layout");
            static_assert(sizeof(T) <= 65536, "a root block is at most 65536 bytes");
            const Addr address = uploadFrameData(std::as_bytes(std::span(&value, 1)), 256);
            setRoot(address);
            return address;
        }

        /// Draws `instances` instances of `vertices` vertices with the current topology; the shader pulls its vertices. A
        /// zero count records nothing. Recorded inside a render pass with a graphics pipeline that has a vertex stage bound
        /// (VU-rojo-cmd-0023), and with a viewport and a scissor set since `beginRenderPass` (VU-rojo-cmd-0024). Thread class
        /// command list.
        void draw(uint32_t vertices, uint32_t instances = 1, uint32_t firstVertex = 0, uint32_t firstInstance = 0);

        /// Draws `count` indices of `type` read from `indices` starting at index `firstIndex`, each added to `baseVertex`.
        /// Recorded as `draw` is, with a viewport and a scissor set (VU-rojo-cmd-0025). `indices` is live
        /// (VU-rojo-core-0001 to 0003), was created with `BufferUsage::Index`, and `firstIndex + count` indices of `type` lie
        /// within it (VU-rojo-cmd-0026). Thread class command list.
        void drawIndexed(
            Buffer indices, IndexType type, uint32_t count, uint32_t instances = 1, uint32_t firstIndex = 0, int32_t baseVertex = 0, uint32_t firstInstance = 0
        );

        /// Launches an `x` by `y` by `z` grid of task threadgroups, or of mesh threadgroups when the pipeline has no task
        /// stage. Recorded inside a render pass with a graphics pipeline that has a mesh stage bound, and with a viewport and
        /// a scissor set (VU-rojo-cmd-0027). `x`, `y` and `z` are each at most `Limits::maxMeshTaskGroups` and their product
        /// at most `Limits::maxMeshTaskGroupsTotal` (VU-rojo-cmd-0028). Thread class command list.
        void drawMeshTasks(uint32_t x, uint32_t y, uint32_t z);

        /// Launches an `x` by `y` by `z` grid of threadgroups of the bound compute pipeline. Recorded outside a render pass
        /// on a `Graphics` or `Compute` list (VU-rojo-cmd-0029) with a compute pipeline bound since the list began or since
        /// the last `endRenderPass` (VU-rojo-cmd-0030); `x`, `y` and `z` are each at most `Limits::maxDispatchGroups`
        /// (VU-rojo-cmd-0031). Thread class command list.
        void dispatch(uint32_t x, uint32_t y, uint32_t z);

        /// Issues up to `maxCount` non-indexed draws whose `DrawIndirectArgs` records start at `args`, with the bound
        /// pipeline and root; a null `count` means exactly `maxCount`, otherwise it names one `uint32` read at execution.
        /// Recorded inside a render pass with a graphics pipeline that has a vertex stage bound (VU-rojo-indirect-0001), after
        /// a viewport and a scissor were set (VU-rojo-indirect-0021); `args` is non-null and 4-byte aligned and `maxCount` records lie in one buffer created
        /// with `BufferUsage::IndirectArgs` (VU-rojo-indirect-0002); `maxCount` is above zero and at most `Limits::maxDrawIndirectCount`
        /// (VU-rojo-indirect-0003); a non-null `count` is 4-byte aligned and lies in such a buffer (VU-rojo-indirect-0004). Each address names a live buffer
        /// and lies inside it (VU-rojo-buffer-0009). Barriers for GPU writes to the records or the count are checked by the validation tracker
        /// (VU-rojo-indirect-0005). Thread class command list.
        void drawIndirect(Addr args, uint32_t maxCount, Addr count = {});

        /// Issues up to `maxCount` indexed draws whose `DrawIndexedIndirectArgs` records start at `args`, fetching indices of
        /// `type` from `indices`. Recorded as `drawIndirect` is (VU-rojo-indirect-0006, 0022); `indices` is live
        /// (VU-rojo-core-0001 to 0003) and was created with `BufferUsage::Index` (VU-rojo-indirect-0007); `args` obeys
        /// VU-rojo-indirect-0008, `maxCount` VU-rojo-indirect-0009 and `count` VU-rojo-indirect-0010, with the barriers of
        /// VU-rojo-indirect-0011 checked by the validation tracker. Thread class command list.
        void drawIndexedIndirect(Buffer indices, IndexType type, Addr args, uint32_t maxCount, Addr count = {});

        /// Issues up to `maxCount` mesh-task draws whose `DrawMeshTasksIndirectArgs` records start at `args`. Recorded inside
        /// a render pass with a graphics pipeline that has a mesh stage bound (VU-rojo-indirect-0012), after a viewport and a
        /// scissor were set (VU-rojo-indirect-0023), on a device that
        /// reports `Capabilities::meshTasksIndirect` (VU-rojo-indirect-0013); `args` obeys VU-rojo-indirect-0014, `maxCount`
        /// VU-rojo-indirect-0015 and `count` VU-rojo-indirect-0016, with the barriers of VU-rojo-indirect-0017 checked by the
        /// validation tracker. Thread class command list.
        void drawMeshTasksIndirect(Addr args, uint32_t maxCount, Addr count = {});

        /// Dispatches the bound compute pipeline with the threadgroup counts of the `DispatchIndirectArgs` record at `args`.
        /// Recorded outside a render pass with a compute pipeline bound (VU-rojo-indirect-0018); `args` is non-null and
        /// 4-byte aligned and the record lies in a buffer created with `BufferUsage::IndirectArgs`
        /// (VU-rojo-indirect-0019); the barrier of VU-rojo-indirect-0020 is checked by the validation tracker. Thread class
        /// command list.
        void dispatchIndirect(Addr args);

        /// Copies `size` bytes from `src` at `srcOffset` to `dst` at `dstOffset`, outside a render pass
        /// (VU-rojo-cmd-0042). `size` is above zero, both ranges lie within their buffers and the offsets and size are
        /// multiples of 4 (VU-rojo-cmd-0043); when `src` and `dst` are one buffer the ranges do not overlap
        /// (VU-rojo-cmd-0044). `src` is `Gpu` or `Upload` and `dst` is `Gpu` or `Readback` (VU-rojo-buffer-0001, 0002). Thread
        /// class command list.
        void copyBuffer(Buffer src, uint64_t srcOffset, Buffer dst, uint64_t dstOffset, uint64_t size);

        /// Copies texel data laid out by `layout` in `src` into `region` of `dst`, outside a render pass (VU-rojo-cmd-0046).
        /// `region` obeys VU-rojo-cmd-0038 to 0041 and `layout` VU-rojo-cmd-0034 to 0037; the bytes `layout` addresses lie
        /// within `src` (VU-rojo-cmd-0064), which is `Gpu` or `Upload` (VU-rojo-buffer-0002); `dst` has one sample
        /// (VU-rojo-cmd-0066). The layout of the subresources is checked by the validation tracker (VU-rojo-cmd-0067). Thread
        /// class command list.
        void copyBufferToTexture(Buffer src, const BufferTextureLayout& layout, Texture dst, const TextureRegion& region);

        /// Copies `region` of `src` into `dst`, laid out by `layout`, outside a render pass (VU-rojo-cmd-0049). The rules of
        /// `copyBufferToTexture` apply to `region` and `layout`; the bytes `layout` addresses lie within `dst`
        /// (VU-rojo-cmd-0065), which is `Gpu` or `Readback` (VU-rojo-buffer-0001); `src` has one sample (VU-rojo-cmd-0068).
        /// The layout of the subresources is checked by the validation tracker (VU-rojo-cmd-0069). Thread class command list.
        void copyTextureToBuffer(Texture src, const TextureRegion& region, Buffer dst, const BufferTextureLayout& layout);

        /// Copies texels between textures of one format, or of two formats that differ only in sRGB transfer, outside a
        /// render pass (VU-rojo-cmd-0052). Both regions obey VU-rojo-cmd-0038 to 0041; the formats are copy-compatible, both
        /// textures have one sample and the regions have equal extents in texel blocks and equal layer counts
        /// (VU-rojo-cmd-0053); within one texture the regions share no subresource (VU-rojo-cmd-0055). The layouts are checked
        /// by the validation tracker (VU-rojo-cmd-0054). Thread class command list.
        void copyTexture(Texture src, const TextureRegion& srcRegion, Texture dst, const TextureRegion& dstRegion);

        /// Writes `pattern`, repeated in little-endian byte order, over `size` bytes of `dst` from `offset`. Recorded outside a
        /// render pass on a `Graphics` or `Compute` list (VU-rojo-cmd-0056); `size` is above zero, `offset` and `size` are
        /// multiples of 4 and the range lies within `dst` (VU-rojo-cmd-0057); `dst` was created with
        /// `BufferUsage::ShaderWrite` (VU-rojo-cmd-0058). Thread class command list.
        void fillBuffer(Buffer dst, uint64_t offset, uint64_t size, uint32_t pattern);

        /// Clears every subresource in `range` to `value`. Recorded outside a render pass on a `Graphics` or `Compute` list
        /// (VU-rojo-cmd-0060); `value` obeys VU-rojo-cmd-0059; `range` obeys VU-rojo-texture-0016, 0017 and 0041; `texture`
        /// was created with a usage VU-rojo-cmd-0061 allows. The layouts are checked by the validation tracker
        /// (VU-rojo-cmd-0070). Thread class command list.
        void clearTexture(Texture texture, const SubresourceRange& range, const ClearValue& value);

        /// Records one batch of barriers: all source scopes complete, then all layout transitions, then all destination scopes
        /// may begin. An empty batch records nothing. Recorded outside a render pass (VU-rojo-sync-0019); no two entries of
        /// `textures` cover the same subresource (VU-rojo-sync-0020). Each barrier obeys the rules of its descriptor. The
        /// ordering and layout rules are checked by the validation tracker (VU-rojo-sync-0021, VU-rojo-sync-0024). Thread
        /// class command list.
        void barrier(std::span<const GlobalBarrier> globals, std::span<const BufferBarrier> buffers = {}, std::span<const TextureBarrier> textures = {});

        /// Records one reconstruction by `upscaler`: it reads the inputs and its history and writes `params.output`, as compute
        /// work, and forms its own debug group named `params.label`, defaulted to `rojorhi.upscale.unnamed`. Recorded on a
        /// `Graphics` list (VU-rojo-upscale-0006) outside a render pass (VU-rojo-upscale-0007), at most once per upscaler per
        /// frame (VU-rojo-upscale-0008). The textures obey VU-rojo-upscale-0009 to 0014 and `params` obeys
        /// VU-rojo-upscale-0004, 0005 and 0015; none is a swapchain image (VU-rojo-present-0012). The layouts are checked by
        /// the validation tracker (VU-rojo-upscale-0016). Thread class command list.
        void upscale(Upscaler upscaler, const UpscaleParams& params);

        /// Declares shader accesses for the next draw, indexed draw, mesh-task draw or dispatch recorded on the list,
        /// including their indirect forms; several calls before one command accumulate. Each entry obeys VU-rojo-diag-0001 to
        /// 0003 and 0005; the list is recording (VU-rojo-diag-0004); each `stage` is one the next command runs
        /// (VU-rojo-diag-0006) and a draw or dispatch follows before `end` (VU-rojo-diag-0007). The rules are checked only
        /// on a device created with `DeviceDesc::validation`, which returns at once otherwise. In a Release build this is an
        /// empty inline function, so the consumer and the library must be built with the same NDEBUG setting. Thread class command
        /// list.
        void declareAccess(std::span<const ResourceAccess> accesses)
        {
#ifndef NDEBUG
            declareAccessChecked(accesses);
#else
            (void)accesses;
#endif
        }

      private:
        /// The library's side of `declareAccess`; it is defined in every build, and only a non-Release caller reaches it.
        void declareAccessChecked(std::span<const ResourceAccess> accesses);

        friend struct CommandListImpl;
        struct CommandListImpl* m_impl = nullptr;
    };

    /// One of a device's queues. It wraps one backend pointer: copies refer to the same queue, and a default-constructed one
    /// refers to none (VU-rojo-core-0004). The device owns it and it is never destroyed separately; it is used only with the
    /// device that produced it (VU-rojo-core-0005). Operations on one queue are serialized by the caller.
    class Queue
    {
      public:
        /// Submits `lists` for execution in span order. No list starts before every fence in `wait` has reached its value;
        /// every fence in `signal` is set to its value once every list has completed. A submission with no lists only waits
        /// and signals. Every list has ended, is unsubmitted and was acquired in the open frame (VU-rojo-queue-0001), for this
        /// queue's `QueueType` (VU-rojo-queue-0002). Each fence appears at most once in `signal`, with a value above every
        /// value submitted for signal on it and above its initial value (VU-rojo-queue-0003); each `wait` value is at most the
        /// fence's initial value or the greatest value already submitted for signal (VU-rojo-queue-0007). Thread class queue.
        void submit(std::span<const CommandList> lists, std::span<const FenceValue> wait = {}, std::span<const FenceValue> signal = {});

        /// Hands the swapchain's acquired image to the display once all work submitted earlier to this queue completes. It is
        /// the graphics queue (VU-rojo-present-0015); the swapchain holds an image acquired in this frame and not yet presented
        /// (VU-rojo-present-0016), and the call comes before the `Device::endFrame` that closes that frame
        /// (VU-rojo-present-0017). The image was last moved to `Layout::Present` (VU-rojo-present-0018) and no list submitted
        /// afterwards references it (VU-rojo-present-0019). Does not wait. Thread class queue.
        void present(Swapchain swapchain);

        /// Applies `mappings` in order as a queue operation: after all work submitted earlier to this queue and before any work
        /// submitted to it later. Every element obeys the `TileMapping` rules (VU-rojo-sparse-0002 to 0010) and no two elements name the
        /// same tile (VU-rojo-sparse-0012). Work on another queue that accesses a tile whose mapping changes is complete or
        /// ordered against the mapping by fences before it executes (VU-rojo-sparse-0013, checked by the validation tracker). A
        /// heap stays alive while any tile remains mapped from it (VU-rojo-device-0009). Does not wait. Thread class queue.
        void mapTiles(std::span<const TileMapping> mappings);

      private:
        friend struct QueueImpl;
        struct QueueImpl* m_impl = nullptr;
    };

    /// The owner of every resource, queue and frame slot RojoRHI creates. It wraps one backend pointer: copies refer to the
    /// same device, and a default-constructed one refers to none (VU-rojo-core-0004). It lives from `createDevice` to
    /// `destroyDevice`.
    class Device
    {
      public:
        /// Returns the device's capabilities; the reference stays valid until `destroyDevice`. Free-threaded and lock-free.
        const Capabilities& capabilities() const;

        /// Returns what the device supports for `format`: a table lookup, free-threaded and lock-free. The format is an
        /// enumerator (VU-rojo-format-0001) and is not `Format::Undefined` (VU-rojo-format-0005).
        FormatCapabilities formatCapabilities(Format format) const;

        /// Opens the next frame, first recycling the slot last opened `framesInFlight` frames earlier: its deferred
        /// destructions are released. No frame may be open (VU-rojo-device-0003). Thread class frame.
        void beginFrame();

        /// Closes the open frame (VU-rojo-device-0004); every command list acquired in it has been submitted
        /// (VU-rojo-device-0005). Thread class frame.
        void endFrame();

        /// Creates a buffer of `desc.size` bytes in `desc.memory` and labels it. `Upload` and `Readback` buffers are persistently
        /// mapped. Non-empty `initial` fills the first `initial.size()` bytes and the rest is undefined; it is at most `desc.size`
        /// (VU-rojo-buffer-0007) and empty for a `Sparse` or `Readback` buffer (VU-rojo-buffer-0008). The descriptor obeys
        /// VU-rojo-buffer-0003 to 0006. A non-null `placement.heap` creates the buffer inside that heap under VU-rojo-heap-0003
        /// to 0006. Out of memory is an error result. Free-threaded.
        Result<Buffer> createBuffer(const BufferDesc& desc, std::span<const std::byte> initial = {}, Placement placement = {});

        /// Creates a placement heap of at least `desc.size` bytes (VU-rojo-heap-0001), labels it and returns it. Out of memory
        /// is an error result. Free-threaded.
        Result<Heap> createHeap(const HeapDesc& desc);

        /// Returns the bytes a buffer described by `desc` occupies inside a heap and the alignment its offset must satisfy.
        /// The descriptor obeys its own rules and is not `Sparse` (VU-rojo-heap-0002). Allocates nothing and returns the same
        /// value for equal descriptors on one device. Free-threaded.
        PlacementRequirements placementRequirements(const BufferDesc& desc);

        /// Returns the bytes a texture described by `desc` occupies inside a heap and the alignment its offset must satisfy.
        /// The descriptor obeys the texture rules and is not `Sparse` (VU-rojo-heap-0002). Allocates nothing and returns the
        /// same value for equal descriptors on one device. Free-threaded.
        PlacementRequirements placementRequirements(const TextureDesc& desc);

        /// Creates a texture and labels it. `initial` is empty or holds one entry per subresource, index `mip + layer * mips`
        /// (VU-rojo-texture-0018), and is empty for a `Sparse`, non-`Sampled` or multisampled texture
        /// (VU-rojo-texture-0019 to 0021); each entry obeys VU-rojo-texture-0013 to 0015. The descriptor obeys
        /// VU-rojo-texture-0001 to 0012. A non-null `placement.heap` creates the texture inside that heap under
        /// VU-rojo-heap-0003 to 0006. Out of memory is an error result. Free-threaded.
        Result<Texture> createTexture(const TextureDesc& desc, std::span<const TextureData> initial = {}, Placement placement = {});

        /// Allocates a slot in the view heap, writes a view of `texture` into it and returns the slot's index. The descriptor
        /// obeys VU-rojo-texture-0016, 0017 and 0022 to 0027, and the texture is destroyed only after every slot holding a
        /// view of it has been destroyed (VU-rojo-texture-0028). A full view heap is an error result. Free-threaded.
        Result<ViewIndex> createView(Texture texture, const TextureViewDesc& desc);

        /// Allocates a slot in the sampler heap, writes a sampler into it and returns the slot's index. The descriptor obeys
        /// VU-rojo-texture-0029 and 0030. A full sampler heap is an error result. Free-threaded.
        Result<SamplerIndex> createSampler(const SamplerDesc& desc);

        /// Reserves `count` contiguous slots in the view heap (VU-rojo-texture-0032), returned with `Kind::Views`. The slots
        /// hold no view until `writeView` writes one. No contiguous run of `count` free slots is an error result.
        /// Free-threaded.
        Result<IndexRange> reserveViews(uint32_t count);

        /// Writes a view of `texture` into slot `range.base + index` of a view range, replacing any view there; the range
        /// is a Views range and `index` is inside it (VU-rojo-texture-0033), and `desc` and `texture` obey
        /// VU-rojo-texture-0034, which also reports a format the device does not support for `desc.usage` (`createView` reports
        /// that as VU-rojo-format-0003), and `range` is a reserved, unreleased range as returned (VU-rojo-texture-0031). The write takes effect for work
        /// submitted after it. Free-threaded; writes to distinct slots may run concurrently.
        void writeView(IndexRange range, uint32_t index, Texture texture, const TextureViewDesc& desc);

        /// Reserves `count` contiguous slots in the sampler heap (VU-rojo-texture-0035), returned with `Kind::Samplers`. The
        /// slots hold no sampler until `writeSampler` writes one. No contiguous run of `count` free slots is an error
        /// result. Free-threaded.
        Result<IndexRange> reserveSamplers(uint32_t count);

        /// Writes a sampler into slot `range.base + index` of a sampler range, replacing any sampler there; the range is a
        /// Samplers range and `index` is inside it (VU-rojo-texture-0036), `desc` obeys VU-rojo-texture-0037 and `range` obeys
        /// VU-rojo-texture-0031. The
        /// write takes effect for work submitted after it. Free-threaded; writes to distinct slots may run concurrently.
        void writeSampler(IndexRange range, uint32_t index, const SamplerDesc& desc);

        /// Releases a caller-owned range, deferred like `destroy`: its slots and what they hold return to the heap once the
        /// last frame that could have read them has retired. The range obeys VU-rojo-texture-0038. Free-threaded.
        void release(IndexRange range);

        /// Returns the device's queue of `type`; the same queue on every call. The device owns it and it is never destroyed
        /// separately. Free-threaded.
        Queue queue(QueueType type);

        /// Returns a command list for `type` in the recording state from the open frame's pool, labelled `label` (empty selects
        /// a `rojorhi.`-prefixed default). A frame is open (VU-rojo-cmd-0001). The list belongs to that frame: it returns to the
        /// pool when the frame retires, and every list acquired in a frame is submitted before its `endFrame`
        /// (VU-rojo-device-0005). Free-threaded.
        CommandList acquireCommandList(QueueType type, std::string_view label);

        /// Loads a precompiled shader library from `blob`, which is not empty (VU-rojo-pipeline-0003), and labels it. A blob not
        /// built for the active backend or malformed is `ErrorCategory::DriverFailure`; one stamped with another
        /// `ROJORHI_SHADER_ABI_VERSION` is `ErrorCategory::ShaderAbiMismatch`. The bytes are consumed before return.
        /// Free-threaded.
        Result<ShaderLibrary> loadShaderLibrary(std::span<const std::byte> blob, std::string_view label);

        /// Creates a graphics pipeline synchronously; on return binding it never compiles. The descriptor obeys
        /// VU-rojo-pipeline-0001, 0002 and 0004 to 0012, and VU-rojo-format-0002 and 0003 for its formats. The device's
        /// pipeline cache is consulted first. A compiler or driver failure is an error result. Free-threaded.
        Result<Pipeline> createPipeline(const GraphicsPipelineDesc& desc);

        /// Creates a compute pipeline synchronously; the descriptor obeys VU-rojo-pipeline-0001, 0002 and 0013. Otherwise as
        /// the graphics overload. Free-threaded.
        Result<Pipeline> createPipeline(const ComputePipelineDesc& desc);

        /// Returns the current pipeline cache as a blob to pass back through `DeviceDesc::pipelineCache` at a later
        /// `createDevice`; it covers every pipeline created so far, and one created concurrently may be missing. Failing to
        /// serialize is an error result. Free-threaded.
        Result<std::vector<std::byte>> pipelineCacheData();

        /// Creates a timeline fence whose completed value starts at `initial` and labels it. Out of memory is an error result.
        /// Free-threaded.
        Result<Fence> createFence(uint64_t initial, std::string_view label);

        /// Creates a swapchain bound to `desc.nativeSurface` for the graphics queue, at the surface's current extent. The
        /// description obeys VU-rojo-present-0001 to 0005, and the surface is not that of another live swapchain of this device
        /// (VU-rojo-present-0006). A color space or present mode the surface or display cannot provide is an error result.
        /// Presentation setup: it may wait. Free-threaded.
        Result<Swapchain> createSwapchain(const SwapchainDesc& desc);

        /// Creates the upscaler implementation `desc.kind` selects, with its private history; `Auto` selects the first kind of
        /// `capabilities().upscalers`. The description obeys VU-rojo-upscale-0001 to 0003. A kind the device does not list, an
        /// extent ratio or format the implementation rejects, or a platform refusal is an error result, never a substitution.
        /// Free-threaded and synchronous.
        Result<Upscaler> createUpscaler(const UpscalerDesc& desc);

        /// Returns the greatest value `fence` has reached. Never blocks. Free-threaded.
        uint64_t completedValue(Fence fence);

        /// Blocks until `value.fence` reaches `value.value` or `timeoutNs` nanoseconds pass; returns true when the value was
        /// reached. A `timeoutNs` of `UINT64_MAX` requires the value to be at most the fence's initial value or the greatest
        /// value already submitted for signal (VU-rojo-queue-0008). Free-threaded.
        bool wait(FenceValue value, uint64_t timeoutNs);

        /// Returns the shader-visible address of byte `offset` of `buffer`: `offset` is less than the buffer's size
        /// (VU-rojo-buffer-0012), the buffer is not `Readback` (VU-rojo-buffer-0013), and the address lies less than
        /// `Limits::maxAddrOffset` bytes from the buffer's start (VU-rojo-buffer-0011). Free-threaded; the same buffer and
        /// offset give the same value for the buffer's lifetime.
        Addr address(Buffer buffer, uint64_t offset = 0);

        /// Returns the CPU pointer to byte 0 of a persistently mapped buffer, valid until the buffer is destroyed; the buffer is
        /// `Upload` or `Readback` (VU-rojo-buffer-0014). Free-threaded.
        std::byte* cpuPointer(Buffer buffer);

        /// Returns the tile shape of a sparse texture, fixed at creation. The texture was created with `TextureUsage::Sparse`
        /// (VU-rojo-sparse-0001) and is not a swapchain image (VU-rojo-present-0012). Records no GPU work. Free-threaded.
        TileShape tileShape(Texture texture);

        /// Returns the tile shape of a sparse buffer, fixed at creation. The buffer was created with `BufferUsage::Sparse`
        /// (VU-rojo-sparse-0001). Records no GPU work. Free-threaded.
        TileShape tileShape(Buffer buffer);

        /// Returns the scopes of the frame the most recent `beginFrame` waited for, the frame `framesInFlight` frames before the
        /// one it opened: records are in submission order per queue, and within a command list in the order the scopes began, a
        /// parent before its children. The span is empty until a frame has retired, and for a frame with no scopes; the device
        /// owns it, labels included, and the next `beginFrame` invalidates it.
        std::span<const ScopeTiming> scopeTimings();

        /// Returns a snapshot of the counters by value. Recording counters accumulate in each command list and fold into the
        /// device when the list is submitted, so recording stays lock-free; creation counters increment on the creating thread.
        /// Counts recorded on a command list reach the device only when the list is submitted, so a list that was acquired and
        /// never submitted contributes none. A snapshot taken while other threads create objects or submit may miss their
        /// latest increments. Free-threaded.
        Counters counters() const;

        /// Starts a GPU capture of everything the device's queues execute until `endCapture`, written to the file `path`, whose
        /// extension the caller supplies for the platform. No capture started by this device is in progress
        /// (VU-rojo-diag-0008) and `path` is not empty (VU-rojo-diag-0009). Capture tooling that is unavailable to the process,
        /// as on the Null backend, or a destination that cannot be written is an error result, and no capture is then in
        /// progress.
        Result<void> beginCapture(std::string_view path);

        /// Ends the capture `beginCapture` started and finishes writing its file; work submitted before the call is in the
        /// capture. A capture started by this device is in progress (VU-rojo-diag-0010).
        void endCapture();

        /// Destroys a buffer, deferred: the handle is invalid at once and the slot is reused once the frame slot open at the
        /// call, or the next one opened if none is, retires. A sparse buffer's tiles are unmapped at the call. Free-threaded.
        void destroy(Buffer buffer);
        /// Destroys a texture, deferred as `destroy(Buffer)` is; a sparse texture's tiles are unmapped at the call.
        void destroy(Texture texture);
        /// Destroys a pipeline, deferred as `destroy(Buffer)` is.
        void destroy(Pipeline pipeline);
        /// Destroys a shader library, deferred as `destroy(Buffer)` is.
        void destroy(ShaderLibrary library);
        /// Destroys a heap, deferred as `destroy(Buffer)` is; the heap holds no placed resource and no mapped tile
        /// (VU-rojo-device-0008, VU-rojo-device-0009).
        void destroy(Heap heap);
        /// Destroys a fence, deferred as `destroy(Buffer)` is.
        void destroy(Fence fence);
        /// Destroys an upscaler, deferred as `destroy(Buffer)` is.
        void destroy(Upscaler upscaler);
        /// Releases a view slot into the deferred path; the index came from `createView` (VU-rojo-device-0010).
        void destroy(ViewIndex view);
        /// Releases a sampler slot into the deferred path; the index came from `createSampler` (VU-rojo-device-0010).
        void destroy(SamplerIndex sampler);
        /// Destroys a swapchain, deferred as `destroy(Buffer)` is; an image it holds acquired is released with it, and using the
        /// swapchain afterwards is undefined behaviour.
        void destroy(Swapchain swapchain);

      private:
        friend struct DeviceImpl;
        struct DeviceImpl* m_impl = nullptr;
    };

    /// Opens the system's GPU and creates a device. Returns `ErrorCategory::Unsupported` when `DeviceDesc::validation` is set
    /// in a Release build, or when the hardware floor is not met. Free-threaded; several devices may coexist.
    Result<Device> createDevice(const DeviceDesc& desc = {});

    /// Waits for all submitted work, releases every object the device owns and invalidates every handle it produced. No
    /// frame is open (VU-rojo-device-0002). Thread class exclusive.
    void destroyDevice(Device device);
} // namespace rojoRHI
