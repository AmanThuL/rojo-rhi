//----------------------------------------------------------------------------------------------------------------------
/// @file RHI.h
/// @brief Declares the whole public interface.
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

// Version
#define ROJORHI_VERSION_MAJOR 0
#define ROJORHI_VERSION_MINOR 2
#define ROJORHI_VERSION_PATCH 0

namespace rojoRHI {
    // Fundamentals
    enum class ErrorCategory : uint8_t
    {
        Unsupported,
        OutOfMemory,
        DriverFailure,
        ShaderAbiMismatch,
    };

    struct Error
    {
        ErrorCategory category;
        std::string message;
    };

    template<class T> using Result = std::expected<T, Error>;

    struct Buffer
    {
        uint64_t bits = 0;
        friend bool operator==(Buffer, Buffer) = default;
    };

    struct Texture
    {
        uint64_t bits = 0;
        friend bool operator==(Texture, Texture) = default;
    };

    struct Pipeline
    {
        uint64_t bits = 0;
        friend bool operator==(Pipeline, Pipeline) = default;
    };

    struct ShaderLibrary
    {
        uint64_t bits = 0;
        friend bool operator==(ShaderLibrary, ShaderLibrary) = default;
    };

    struct Heap
    {
        uint64_t bits = 0;
        friend bool operator==(Heap, Heap) = default;
    };

    struct Fence
    {
        uint64_t bits = 0;
        friend bool operator==(Fence, Fence) = default;
    };

    struct Upscaler
    {
        uint64_t bits = 0;
        friend bool operator==(Upscaler, Upscaler) = default;
    };

    struct Extent2D
    {
        uint32_t width = 0;
        uint32_t height = 0;
    };

    struct Extent3D
    {
        uint32_t width = 1;
        uint32_t height = 1;
        uint32_t depth = 1;
    };

    struct Offset3D
    {
        uint32_t x = 0;
        uint32_t y = 0;
        uint32_t z = 0;
    };

    struct Rect2D
    {
        uint32_t x = 0;
        uint32_t y = 0;
        uint32_t width = 0;
        uint32_t height = 0;
    };

    class Device;

    class Swapchain
    {
      public:
        // Outside a frame; waits until the graphics queue has finished with the images.
        void resize(Extent2D extent);

        // Waits for a free image; returns a null texture when none frees within one second.
        Texture acquire();

      private:
        friend class Device;
        friend struct SwapchainImpl;
        struct SwapchainImpl* m_impl = nullptr;
    };

    enum class MessageSeverity : uint8_t
    {
        Info,
        Warning,
        Error,
    };

    using MessageCallback = void (*)(MessageSeverity severity, std::string_view message, void* user);

    // Free-threaded. A callback replaced by this call may still be running when it returns.
    void setMessageCallback(MessageCallback callback, void* user);

    // Device
    struct DeviceDesc
    {
        bool validation = false; ///< Runs the validation tracker; non-Release builds only.
        bool enableCapture = false;
        bool scopeTimings = false;                ///< GPU timestamps at scope boundaries, read through Device::scopeTimings.
        uint32_t framesInFlight = 3;              ///< 2 or 3.
        std::span<const std::byte> pipelineCache; ///< From Device::pipelineCacheData; empty for none.
        std::string_view label;
    };

    // Memory and buffers
    enum class Memory : uint8_t
    {
        Gpu,      ///< Device-local; not CPU-visible.
        Upload,   ///< CPU writes, GPU reads; persistently mapped.
        Readback, ///< GPU writes, CPU reads; persistently mapped.
    };

    enum class BufferUsage : uint32_t
    {
        None = 0,
        Index = 1u << 0,
        IndirectArgs = 1u << 1,
        ShaderWrite = 1u << 2, ///< Memory::Gpu only.
        Sparse = 1u << 3,      ///< No memory until tiles are mapped; Memory::Gpu only.
    };

    constexpr BufferUsage operator|(BufferUsage a, BufferUsage b)
    {
        return static_cast<BufferUsage>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
    }

    constexpr BufferUsage operator&(BufferUsage a, BufferUsage b)
    {
        return static_cast<BufferUsage>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
    }

    struct BufferDesc
    {
        uint64_t size = 0; ///< Bytes.
        Memory memory = Memory::Gpu;
        BufferUsage usage = BufferUsage::None;
        std::string_view label;
    };

    struct alignas(8) Addr
    {
        uint32_t lo;
        uint32_t hi;
    };

    Addr operator+(Addr base, uint64_t offset);

    struct HeapDesc
    {
        uint64_t size = 0; ///< Bytes.
        Memory memory = Memory::Gpu;
        std::string_view label;
    };

    struct PlacementRequirements
    {
        uint64_t size = 0;
        uint64_t alignment = 0;
    };

    struct Placement
    {
        Heap heap = {}; ///< Null: a dedicated allocation.
        uint64_t offset = 0;
    };

    // Formats
    enum class Format : uint32_t
    {
        Undefined, ///< No format.
        BGRA8Unorm,
        RGBA8Unorm,
        RGBA8Unorm_sRGB,
        RGBA16Float, ///< The scene-linear color format.
        RG16Float,
        R8Unorm,
        BC1Unorm,
        BC1Unorm_sRGB,
        D32Float,
        R16Float,
        R32Float,
        D32FloatS8,
        RGB10A2Unorm, ///< The HDR10 presentation format.
    };

    // Compare operations
    enum class CompareOp : uint8_t
    {
        Never,
        Less,
        Equal,
        LessOrEqual,
        Greater,
        NotEqual,
        GreaterOrEqual,
        Always,
    };

    // Textures and heaps
    enum class TextureType : uint8_t
    {
        Tex1D,
        Tex1DArray,
        Tex2D,
        Tex2DArray,
        Tex3D,
        Cube,      ///< Six faces.
        CubeArray, ///< A multiple of six faces.
    };

    enum class TextureUsage : uint32_t
    {
        None = 0,
        Sampled = 1u << 0,
        Storage = 1u << 1,
        RenderTarget = 1u << 2,
        DepthStencil = 1u << 3,
        Sparse = 1u << 4, ///< No memory until tiles are mapped.
    };

    constexpr TextureUsage operator|(TextureUsage a, TextureUsage b)
    {
        return static_cast<TextureUsage>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
    }

    constexpr TextureUsage operator&(TextureUsage a, TextureUsage b)
    {
        return static_cast<TextureUsage>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
    }

    struct TextureDesc
    {
        TextureType type = TextureType::Tex2D;
        Format format = Format::Undefined;
        Extent3D extent; ///< Of mip 0.
        uint32_t mips = 1;
        uint32_t layers = 1; ///< Faces, for cube types.
        uint32_t samples = 1;
        TextureUsage usage = TextureUsage::None;
        std::string_view label;
    };

    struct TextureData
    {
        std::span<const std::byte> bytes; ///< Empty leaves the subresource undefined.
        uint64_t bytesPerRow = 0;
        uint64_t bytesPerImage = 0; ///< Tex3D only; 0 otherwise.
    };

    inline constexpr uint32_t kAllMips = ~0u;

    inline constexpr uint32_t kAllLayers = ~0u;

    struct SubresourceRange
    {
        uint32_t baseMip = 0;
        uint32_t mipCount = kAllMips;
        uint32_t baseLayer = 0;
        uint32_t layerCount = kAllLayers;
    };

    enum class TextureAspect : uint8_t
    {
        Color,
        Depth,
        Stencil,
    };

    struct TextureViewDesc
    {
        TextureType type = TextureType::Tex2D;
        Format format = Format::Undefined; ///< Undefined: the texture's own format.
        SubresourceRange range;
        TextureUsage usage = TextureUsage::Sampled; ///< Exactly Sampled or Storage.
        TextureAspect aspect = TextureAspect::Color;
    };

    enum class ViewIndex : uint32_t
    {
    };

    enum class SamplerFilter : uint8_t
    {
        Nearest,
        Linear,
    };

    enum class SamplerAddress : uint8_t
    {
        Repeat,
        MirrorRepeat,
        ClampToEdge,
    };

    struct SamplerDesc
    {
        SamplerFilter filter = SamplerFilter::Linear; ///< Minification and magnification.
        SamplerFilter mipFilter = SamplerFilter::Linear;
        SamplerAddress address = SamplerAddress::Repeat; ///< Every axis.
        uint32_t maxAnisotropy = 1;                      ///< 1 disables anisotropic filtering.
        bool compareEnable = false;
        CompareOp compare = CompareOp::LessOrEqual; ///< Used only when compareEnable is true.
        std::string_view label;
    };

    enum class SamplerIndex : uint32_t
    {
    };

    struct IndexRange
    {
        enum class Kind : uint8_t
        {
            Views,
            Samplers,
        };

        uint32_t base = 0;
        uint32_t count = 0;
        Kind kind = Kind::Views;
    };

    // Pipelines
    struct ShaderEntry
    {
        ShaderLibrary library; ///< Null: the stage is absent.
        std::string_view name;
    };

    enum class BlendFactor : uint8_t
    {
        Zero,
        One,
        SrcColor,
        OneMinusSrcColor,
        SrcAlpha,
        OneMinusSrcAlpha,
        DstColor,
        OneMinusDstColor,
        DstAlpha,
        OneMinusDstAlpha,
        SrcAlphaSaturate, ///< min(source alpha, 1 - destination alpha).
        Constant,         ///< The blend constants of the command list.
        OneMinusConstant,
    };

    enum class BlendOp : uint8_t
    {
        Add,
        Subtract,
        ReverseSubtract,
        Min, ///< The factors are ignored.
        Max, ///< The factors are ignored.
    };

    struct BlendState
    {
        bool enable = false;
        BlendFactor srcColor = BlendFactor::One;
        BlendFactor dstColor = BlendFactor::Zero;
        BlendOp colorOp = BlendOp::Add;
        BlendFactor srcAlpha = BlendFactor::One;
        BlendFactor dstAlpha = BlendFactor::Zero;
        BlendOp alphaOp = BlendOp::Add;
    };

    enum class ColorWriteMask : uint8_t
    {
        None = 0,
        R = 1,
        G = 2,
        B = 4,
        A = 8,
        All = 15,
    };

    constexpr ColorWriteMask operator|(ColorWriteMask a, ColorWriteMask b)
    {
        return static_cast<ColorWriteMask>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
    }

    constexpr ColorWriteMask operator&(ColorWriteMask a, ColorWriteMask b)
    {
        return static_cast<ColorWriteMask>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
    }

    struct ColorTarget
    {
        Format format = Format::Undefined;
        BlendState blend;
        ColorWriteMask writeMask = ColorWriteMask::All;
    };

    enum class CullMode : uint8_t
    {
        None,
        Front,
        Back,
    };

    enum class FrontFace : uint8_t
    {
        CounterClockwise,
        Clockwise,
    };

    enum class FillMode : uint8_t
    {
        Solid,
        Wireframe,
    };

    struct RasterState
    {
        CullMode cull = CullMode::None;
        FrontFace frontFace = FrontFace::CounterClockwise;
        FillMode fill = FillMode::Solid;
        bool depthClip = true;  ///< False clamps depth instead of clipping.
        bool depthBias = false; ///< Enables CommandList::setDepthBias.
        bool conservative = false;
    };

    enum class StencilOp : uint8_t
    {
        Keep,
        Zero,
        Replace, ///< Writes the stencil reference.
        IncrementClamp,
        DecrementClamp,
        Invert,
        IncrementWrap,
        DecrementWrap,
    };

    struct StencilFaceState
    {
        StencilOp fail = StencilOp::Keep;
        StencilOp depthFail = StencilOp::Keep;
        StencilOp pass = StencilOp::Keep;
        CompareOp compare = CompareOp::Always;
    };

    struct DepthStencilState
    {
        bool depthTest = false;
        bool depthWrite = false;
        CompareOp depthCompare = CompareOp::GreaterOrEqual; ///< Reverse Z.
        bool stencilTest = false;
        uint8_t stencilReadMask = 0xFF;
        uint8_t stencilWriteMask = 0xFF;
        StencilFaceState front;
        StencilFaceState back;
    };

    enum class PrimitiveClass : uint8_t
    {
        Point,
        Line,
        Triangle,
    };

    struct GraphicsPipelineDesc
    {
        ShaderEntry vertex; ///< Absent for a mesh pipeline.
        ShaderEntry task;
        ShaderEntry mesh;                                    ///< Absent for a vertex pipeline.
        ShaderEntry fragment;                                ///< Absent for a depth-only pipeline.
        PrimitiveClass primitive = PrimitiveClass::Triangle; ///< The exact topology is dynamic.
        RasterState raster;
        DepthStencilState depthStencil;
        std::span<const ColorTarget> colors;    ///< Entry i is color attachment i.
        Format depthFormat = Format::Undefined; ///< Undefined: no depth attachment.
        uint32_t samples = 1;
        std::string_view label;
    };

    struct ComputePipelineDesc
    {
        ShaderEntry compute;
        std::string_view label;
    };

    using PipelineCache = std::span<const std::byte>;

    struct Viewport
    {
        float x = 0.0f;
        float y = 0.0f;
        float width = 0.0f;
        float height = 0.0f;
        float minDepth = 0.0f; ///< Depth mapped from clip-space zero.
        float maxDepth = 1.0f; ///< Depth mapped from clip-space one.
    };

    enum class PrimitiveTopology : uint8_t
    {
        PointList,
        LineList,
        LineStrip,
        TriangleList,
        TriangleStrip,
    };

    // Command recording
    enum class QueueType : uint8_t
    {
        Graphics,
        Compute,
        Copy
    };

    enum class LoadOp : uint8_t
    {
        Load,
        Clear,
        DontCare, ///< Undefined; the pass overwrites it.
    };

    enum class StoreOp : uint8_t
    {
        Store,
        DontCare, ///< Undefined afterwards.
    };

    struct ClearColor
    {
        float r = 0.0f;
        float g = 0.0f;
        float b = 0.0f;
        float a = 0.0f;
    };

    struct ColorAttachment
    {
        Texture texture;
        uint32_t mip = 0;
        uint32_t layer = 0;
        LoadOp load = LoadOp::Load;
        StoreOp store = StoreOp::Store;
        ClearColor clear;
        Texture resolve = {}; ///< Single-sample resolve target, or null.
    };

    struct DepthAttachment
    {
        Texture texture; ///< Null: no depth attachment.
        uint32_t mip = 0;
        uint32_t layer = 0;
        LoadOp load = LoadOp::Load;
        StoreOp store = StoreOp::Store;
        float clearDepth = 0.0f;
        LoadOp stencilLoad = LoadOp::DontCare;
        StoreOp stencilStore = StoreOp::DontCare;
        uint8_t clearStencil = 0;
    };

    struct RenderPassDesc
    {
        std::span<const ColorAttachment> colors;
        DepthAttachment depth;
        Rect2D area;
        std::string_view label; ///< Of the pass's scope.
    };

    enum class IndexType : uint8_t
    {
        UInt16,
        UInt32,
    };

    struct BufferTextureLayout
    {
        uint64_t offset = 0;
        uint32_t rowPitch = 0;     ///< Bytes between rows of texel blocks.
        uint32_t rowsPerImage = 0; ///< 0: the region height.
    };

    struct TextureRegion
    {
        uint32_t mip = 0;
        uint32_t layer = 0;
        uint32_t layers = 1;
        Offset3D offset;
        Extent3D extent;
    };

    struct ClearValue
    {
        ClearColor color;
        float depth = 0.0f;
        uint8_t stencil = 0;
    };

    // GPU-driven
    struct DrawIndirectArgs
    {
        uint32_t drawId; ///< Returned by drawId() in the shader.
        uint32_t vertexCount;
        uint32_t instanceCount;
        uint32_t firstVertex;
        uint32_t firstInstance;
    };

    struct DrawIndexedIndirectArgs
    {
        uint32_t drawId; ///< Returned by drawId() in the shader.
        uint32_t indexCount;
        uint32_t instanceCount;
        uint32_t firstIndex;
        int32_t baseVertex;
        uint32_t firstInstance;
    };

    struct DrawMeshTasksIndirectArgs
    {
        uint32_t drawId; ///< Returned by drawId() in the shader.
        uint32_t x;
        uint32_t y;
        uint32_t z;
    };

    struct DispatchIndirectArgs
    {
        uint32_t x;
        uint32_t y;
        uint32_t z;
    };

    // Synchronization
    enum class Stage : uint32_t
    {
        None = 0,
        IndirectArgs = 1u << 0,
        IndexInput = 1u << 1,
        VertexShader = 1u << 2,
        TaskShader = 1u << 3,
        MeshShader = 1u << 4,
        FragmentShader = 1u << 5,
        ComputeShader = 1u << 6,
        ColorAttachment = 1u << 7,
        DepthStencil = 1u << 8,
        Copy = 1u << 9, ///< Copies, fillBuffer and clearTexture.
        Resolve = 1u << 10,
        AllGraphics = IndirectArgs | IndexInput | VertexShader | TaskShader | MeshShader | FragmentShader | ColorAttachment | DepthStencil | Resolve,
        All = (1u << 11) - 1,
    };

    constexpr Stage operator|(Stage a, Stage b)
    {
        return static_cast<Stage>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
    }

    constexpr Stage operator&(Stage a, Stage b)
    {
        return static_cast<Stage>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
    }

    constexpr Stage operator~(Stage a)
    {
        return static_cast<Stage>(~static_cast<uint32_t>(a));
    }

    enum class Access : uint32_t
    {
        None = 0, ///< Orders execution only.
        IndirectArgs = 1u << 0,
        IndexRead = 1u << 1,
        ShaderRead = 1u << 2,
        ShaderWrite = 1u << 3,
        ColorAttachmentRead = 1u << 4,
        ColorAttachmentWrite = 1u << 5,
        DepthStencilRead = 1u << 6,
        DepthStencilWrite = 1u << 7,
        CopyRead = 1u << 8,
        CopyWrite = 1u << 9,
        Resolve = 1u << 10,
    };

    constexpr Access operator|(Access a, Access b)
    {
        return static_cast<Access>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
    }

    constexpr Access operator&(Access a, Access b)
    {
        return static_cast<Access>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
    }

    constexpr Access operator~(Access a)
    {
        return static_cast<Access>(~static_cast<uint32_t>(a));
    }

    enum class Layout : uint8_t
    {
        Undefined, ///< Only before a barrier, which discards the contents.
        General,   ///< The layout of a cross-queue handoff.
        ShaderRead,
        ShaderWrite,
        ColorAttachment,
        DepthStencilWrite, ///< Any depth-stencil attachment use.
        DepthStencilRead,  ///< Shader reads of a depth-stencil texture.
        CopySrc,
        CopyDst,
        Present, ///< Permits no other use.
    };

    struct GlobalBarrier
    {
        Stage srcStage = Stage::None;
        Access srcAccess = Access::None;
        Stage dstStage = Stage::None;
        Access dstAccess = Access::None;
    };

    inline constexpr uint64_t kWholeSize = ~uint64_t{ 0 };

    struct BufferBarrier
    {
        Buffer buffer;
        uint64_t offset = 0;
        uint64_t size = kWholeSize;
        Stage srcStage = Stage::None;
        Access srcAccess = Access::None;
        Stage dstStage = Stage::None;
        Access dstAccess = Access::None;
    };

    struct TextureBarrier
    {
        Texture texture;
        SubresourceRange range;
        Stage srcStage = Stage::None;
        Access srcAccess = Access::None;
        Stage dstStage = Stage::None;
        Access dstAccess = Access::None;
        Layout oldLayout = Layout::Undefined; ///< Undefined discards the contents.
        Layout newLayout = Layout::Undefined;
    };

    // Queues and frames
    struct FenceValue
    {
        Fence fence;
        uint64_t value;
    };

    // Presentation
    enum class ColorSpace : uint8_t
    {
        SRGB,           ///< SDR; the shader writes sRGB-encoded values.
        ExtendedLinear, ///< EDR: linear, sRGB primaries, above 1.0 is brighter than SDR white.
        HDR10,          ///< BT.2020 primaries, PQ transfer.
    };

    enum class PresentMode : uint8_t
    {
        Fifo,      ///< In present order; never tears.
        Mailbox,   ///< A newer present replaces a queued one.
        Immediate, ///< May tear.
    };

    struct SwapchainDesc
    {
        void* nativeSurface = nullptr; ///< A CA::MetalLayer* on Metal 4, an HWND on D3D12.
        Format format = Format::BGRA8Unorm;
        ColorSpace colorSpace = ColorSpace::SRGB;
        uint32_t imageCount = 3;
        PresentMode mode = PresentMode::Fifo;
        uint32_t maxLatency = 2; ///< Queued presents at which acquire waits.
        std::string_view label;
    };

    // Upscaling
    enum class UpscalerKind : uint8_t
    {
        Auto, ///< The backend's preferred available kind.
        MetalFX,
        DLSS,
        FSR,
        XeSS,
    };

    struct UpscalerSupport
    {
        UpscalerKind kind = UpscalerKind::MetalFX;
        float minInputScale = 1.0f; ///< Smallest input extent over output extent, per axis, in (0, 1].
        float maxInputScale = 1.0f; ///< Largest, at least minInputScale and at most 1.
    };

    enum class UpscalerFlags : uint32_t
    {
        None = 0,
        HDR = 1u << 0,           ///< Scene-linear color input.
        ReversedDepth = 1u << 1, ///< Larger depth is nearer.
        AutoExposure = 1u << 2,  ///< UpscaleParams::exposure is ignored.
    };

    constexpr UpscalerFlags operator|(UpscalerFlags a, UpscalerFlags b)
    {
        return static_cast<UpscalerFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
    }

    constexpr UpscalerFlags operator&(UpscalerFlags a, UpscalerFlags b)
    {
        return static_cast<UpscalerFlags>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
    }

    struct UpscalerDesc
    {
        UpscalerKind kind = UpscalerKind::Auto;
        Extent2D input; ///< Allocated extent of every input.
        Extent2D output;
        Format color = Format::RGBA16Float;
        Format depth = Format::D32Float;
        Format motion = Format::RG16Float;
        Format outputFormat = Format::RGBA16Float;
        UpscalerFlags flags = UpscalerFlags::None;
        std::string_view label;
    };

    struct UpscaleParams
    {
        Texture color;
        Texture depth;
        Texture motion; ///< Current to previous position, in input pixels.
        Texture output;
        Texture exposure = {};      ///< Optional 1x1 multiplier; null means 1.0.
        Texture reactive = {};      ///< Optional per-pixel weight; null means none.
        Extent2D inputContent = {}; ///< Zero: the whole input extent.
        float jitterX = 0.0f;       ///< Input texels, positive down.
        float jitterY = 0.0f;       ///< Input texels, positive down.
        float motionScaleX = 1.0f;
        float motionScaleY = 1.0f;
        float preExposure = 1.0f; ///< Already applied to the color input.
        float sharpness = 0.0f;   ///< 0 to 1.
        bool reset = false;       ///< Discards the history, as after a camera cut.
        std::string_view label;
    };

    // Sparse
    struct TileShape
    {
        uint32_t width = 0; ///< For a buffer, the tile size in bytes.
        uint32_t height = 0;
        uint32_t depth = 0;
        uint32_t bytes = 0;        ///< Heap bytes one tile maps.
        uint32_t mipTailFirst = 0; ///< First mip in the tail; the mip count without one.
        uint64_t mipTailBytes = 0;
    };

    struct TileMapping
    {
        Texture texture = {};
        TextureRegion region; ///< With mipTail, only its layers are read.
        bool mipTail = false;
        Buffer buffer = {};
        uint64_t bufferOffset = 0;
        uint64_t bufferSize = 0;
        Heap heap = {}; ///< Null with unmap.
        uint64_t heapOffset = 0;
        bool unmap = false;
    };

    // Diagnostics
    struct ResourceAccess
    {
        Buffer buffer = {};
        uint64_t offset = 0;
        uint64_t size = kWholeSize;
        Texture texture = {};
        SubresourceRange range;
        Stage stage; ///< Exactly one shader stage.
        Access access;
    };

    struct Counters
    {
        uint64_t nativeCreateCalls = 0; ///< Create and release calls.
        uint64_t nativeRecordCalls = 0; ///< Calls recorded into command buffers.
        uint64_t nativeSubmitCalls = 0; ///< Queue calls.
        uint64_t nativeHostCalls = 0;   ///< Other native calls on the CPU.
        uint64_t cpuAllocations = 0;    ///< The library's own explicit allocations.
        uint64_t gpuAllocations = 0;    ///< Native memory objects created.
        uint64_t frameDataUploads = 0;
        uint64_t frameArenaBytes = 0;
        uint64_t frameArenaGrowths = 0;
        uint64_t descriptorWrites = 0; ///< Heap-slot writes.
        uint64_t globalBarriers = 0;
        uint64_t bufferBarriers = 0;
        uint64_t textureBarriers = 0;
        uint64_t nativeBarriers = 0; ///< Emitted after batching.
        uint64_t pipelinesCreated = 0;
        uint64_t pipelineCacheHits = 0;
        uint64_t pipelineCacheMisses = 0;
        uint64_t pipelineCompileNanoseconds = 0; ///< CPU time; not exact.
        uint64_t encodersOpened = 0;             ///< Native encoders on Metal 4; render passes elsewhere.
        uint64_t deferredDestructionsRetired = 0;
    };

    struct ScopeTiming
    {
        std::string_view label;
        uint64_t frame = 0; ///< Counts beginFrame calls from one.
        QueueType queue = QueueType::Graphics;
        uint32_t depth = 0; ///< Zero for an outermost scope.
        double gpuMilliseconds = 0.0;
    };

    // Formats and limits
    struct FormatCapabilities
    {
        bool sampled = false;
        bool filterLinear = false;
        bool storageRead = false;
        bool storageWrite = false;
        bool colorAttachment = false;
        bool blend = false;
        bool depthAttachment = false;
        bool resolve = false;
        bool sparse = false;
        uint32_t sampleCounts = 1; ///< Bit i set: 1 << i samples supported.
    };

    struct Limits
    {
        uint32_t maxViews = 0;
        uint32_t maxSamplers = 0;
        uint32_t maxRootBlockSize = 0; ///< Bytes.
        uint32_t rootAlignment = 0;
        uint32_t frameDataAlignment = 0; ///< Default uploadFrameData alignment.
        uint64_t maxBufferSize = 0;      ///< Bytes.
        uint64_t maxAddrOffset = 0;      ///< Bytes an Addr reaches from its buffer's start.
        uint32_t maxTextureDimension1D = 0;
        uint32_t maxTextureDimension2D = 0;
        uint32_t maxTextureDimension3D = 0;
        uint32_t maxTextureDimensionCube = 0;
        uint32_t maxTextureLayers = 0; ///< Six per cube.
        uint32_t maxColorAttachments = 0;
        uint32_t maxColorBytesPerSample = 0; ///< Summed over the color attachments.
        uint32_t maxSamplerAnisotropy = 0;
        uint32_t maxDrawIndirectCount = 0;
        uint32_t maxDispatchGroups = 0;
        uint32_t maxThreadsPerGroup = 0;
        uint32_t maxGroupSize[3] = {};     ///< x, y, z.
        uint32_t maxGroupSharedMemory = 0; ///< Bytes.
        uint32_t maxMeshTaskGroups = 0;
        uint32_t maxMeshTaskGroupsTotal = 0; ///< Product of the three dimensions.
    };

    // Capabilities
    struct Capabilities
    {
        std::string_view adapterName;
        std::string_view floorName; ///< For people, such as "Apple9".
        uint32_t maxViews = 0;
        uint32_t maxSamplers = 0;
        bool uploadInDeviceMemory = false; ///< Memory::Upload is device-local.
        bool placementSparse = false;
        bool meshTasksIndirect = false;
        bool conservativeRaster = false;
        std::span<const UpscalerSupport> upscalers;
        Limits limits;
    };

    // Objects
    class CommandList
    {
      public:
        void end();

        // The address is valid only within the current frame. Failing to grow the arena is fatal.
        Addr uploadFrameData(std::span<const std::byte> bytes, uint32_t alignment = 256);

        void beginRenderPass(const RenderPassDesc& desc);
        void endRenderPass();
        void beginScope(std::string_view label);
        void endScope();
        void setPipeline(Pipeline pipeline);
        void setViewport(const Viewport& viewport);
        void setScissor(const Rect2D& rect);
        void setStencilReference(uint8_t reference);
        void setBlendConstants(float r, float g, float b, float a);
        void setPrimitiveTopology(PrimitiveTopology topology);
        void setDepthBias(float constant, float slopeScaled, float clamp);
        void setRoot(Addr root);

        template<class T> Addr pushRoot(const T& value)
        {
            static_assert(std::is_trivially_copyable_v<T>, "a root type is trivially copyable");
            static_assert(std::is_standard_layout_v<T>, "a root type is standard-layout");
            static_assert(sizeof(T) <= 65536, "a root block is at most 65536 bytes");
            const Addr address = uploadFrameData(std::as_bytes(std::span(&value, 1)), 256);
            setRoot(address);
            return address;
        }

        void draw(uint32_t vertices, uint32_t instances = 1, uint32_t firstVertex = 0, uint32_t firstInstance = 0);
        void drawIndexed(
            Buffer indices, IndexType type, uint32_t count, uint32_t instances = 1, uint32_t firstIndex = 0, int32_t baseVertex = 0, uint32_t firstInstance = 0
        );
        void drawMeshTasks(uint32_t x, uint32_t y, uint32_t z);
        void dispatch(uint32_t x, uint32_t y, uint32_t z);
        void drawIndirect(Addr args, uint32_t maxCount, Addr count = {});
        void drawIndexedIndirect(Buffer indices, IndexType type, Addr args, uint32_t maxCount, Addr count = {});
        void drawMeshTasksIndirect(Addr args, uint32_t maxCount, Addr count = {});
        void dispatchIndirect(Addr args);
        void copyBuffer(Buffer src, uint64_t srcOffset, Buffer dst, uint64_t dstOffset, uint64_t size);
        void copyBufferToTexture(Buffer src, const BufferTextureLayout& layout, Texture dst, const TextureRegion& region);
        void copyTextureToBuffer(Texture src, const TextureRegion& region, Buffer dst, const BufferTextureLayout& layout);
        void copyTexture(Texture src, const TextureRegion& srcRegion, Texture dst, const TextureRegion& dstRegion);
        void fillBuffer(Buffer dst, uint64_t offset, uint64_t size, uint32_t pattern);
        void clearTexture(Texture texture, const SubresourceRange& range, const ClearValue& value);
        void barrier(std::span<const GlobalBarrier> globals, std::span<const BufferBarrier> buffers = {}, std::span<const TextureBarrier> textures = {});
        void upscale(Upscaler upscaler, const UpscaleParams& params);

        // Empty in Release builds: build the consumer and the library with the same NDEBUG setting.
        void declareAccess(std::span<const ResourceAccess> accesses)
        {
#ifndef NDEBUG
            declareAccessChecked(accesses);
#else
            (void)accesses;
#endif
        }

      private:
        void declareAccessChecked(std::span<const ResourceAccess> accesses);
        friend struct CommandListImpl;
        struct CommandListImpl* m_impl = nullptr;
    };

    class Queue
    {
      public:
        void submit(std::span<const CommandList> lists, std::span<const FenceValue> wait = {}, std::span<const FenceValue> signal = {});
        void present(Swapchain swapchain);
        void mapTiles(std::span<const TileMapping> mappings);

      private:
        friend struct QueueImpl;
        struct QueueImpl* m_impl = nullptr;
    };

    class Device
    {
      public:
        const Capabilities& capabilities() const;
        FormatCapabilities formatCapabilities(Format format) const;

        // Frame thread class. Waits until the frame slot opened framesInFlight frames earlier retires.
        void beginFrame();

        // Frame thread class.
        void endFrame();

        Result<Buffer> createBuffer(const BufferDesc& desc, std::span<const std::byte> initial = {}, Placement placement = {});
        Result<Heap> createHeap(const HeapDesc& desc);
        PlacementRequirements placementRequirements(const BufferDesc& desc);
        PlacementRequirements placementRequirements(const TextureDesc& desc);
        Result<Texture> createTexture(const TextureDesc& desc, std::span<const TextureData> initial = {}, Placement placement = {});
        Result<ViewIndex> createView(Texture texture, const TextureViewDesc& desc);
        Result<SamplerIndex> createSampler(const SamplerDesc& desc);
        Result<IndexRange> reserveViews(uint32_t count);

        // A native refusal is fatal.
        void writeView(IndexRange range, uint32_t index, Texture texture, const TextureViewDesc& desc);

        Result<IndexRange> reserveSamplers(uint32_t count);

        // A native refusal is fatal.
        void writeSampler(IndexRange range, uint32_t index, const SamplerDesc& desc);

        void release(IndexRange range);
        Queue queue(QueueType type);
        CommandList acquireCommandList(QueueType type, std::string_view label);
        Result<ShaderLibrary> loadShaderLibrary(std::span<const std::byte> blob, std::string_view label);
        Result<Pipeline> createPipeline(const GraphicsPipelineDesc& desc);
        Result<Pipeline> createPipeline(const ComputePipelineDesc& desc);
        Result<std::vector<std::byte>> pipelineCacheData();
        Result<Fence> createFence(uint64_t initial, std::string_view label);

        // May wait for the last present of a destroyed swapchain on the same surface.
        Result<Swapchain> createSwapchain(const SwapchainDesc& desc);

        Result<Upscaler> createUpscaler(const UpscalerDesc& desc);
        uint64_t completedValue(Fence fence);
        bool wait(FenceValue value, uint64_t timeoutNs);
        Addr address(Buffer buffer, uint64_t offset = 0);
        std::byte* cpuPointer(Buffer buffer);
        TileShape tileShape(Texture texture);
        TileShape tileShape(Buffer buffer);

        // Frame thread class. The next beginFrame invalidates the span. Always empty on a device created without
        // DeviceDesc::scopeTimings.
        std::span<const ScopeTiming> scopeTimings();

        Counters counters() const;
        Result<void> beginCapture(std::string_view path);
        void endCapture();

        // Deferred: the native object is released when the frame slot open at the call retires.
        void destroy(Buffer buffer);
        void destroy(Texture texture);
        void destroy(Pipeline pipeline);
        void destroy(ShaderLibrary library);
        void destroy(Heap heap);
        void destroy(Fence fence);
        void destroy(Upscaler upscaler);
        void destroy(ViewIndex view);
        void destroy(SamplerIndex sampler);
        void destroy(Swapchain swapchain);

      private:
        friend struct DeviceImpl;
        struct DeviceImpl* m_impl = nullptr;
    };

    // Returns ErrorCategory::Unsupported when DeviceDesc::validation is set in a Release build.
    Result<Device> createDevice(const DeviceDesc& desc = {});

    // Thread class exclusive. Waits for all submitted work.
    void destroyDevice(Device device);
} // namespace rojoRHI
