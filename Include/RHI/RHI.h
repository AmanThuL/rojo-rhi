//----------------------------------------------------------------------------------------------------------------------
/// @file RHI.h
/// @brief Declares the backend-neutral rendering hardware interface.
//----------------------------------------------------------------------------------------------------------------------
#pragma once
#include "RHI/Result.h"
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace lmx::rhi {

/// Identifies the storage and transfer format of an RHI texture attachment.
/// RGBA16Float is the scene-linear color format: half precision preserves radiance above 1.0.
enum class Format {
    Unknown,         ///< No color or depth format.
    BGRA8Unorm,      ///< Eight-bit normalized BGRA color.
    RGBA8Unorm,      ///< Eight-bit normalized RGBA color.
    RGBA8Unorm_sRGB, ///< Eight-bit RGBA color with sRGB transfer on sampling.
    RGBA16Float,     ///< Half-precision linear RGBA color.
    RG16Float,       ///< Half-precision two-channel linear data.
    BC1Unorm,        ///< BC1-compressed normalized linear color.
    BC1Unorm_sRGB,   ///< BC1-compressed color with sRGB transfer on sampling.
    D32Float         ///< 32-bit floating-point depth.
};

/// Identifies whether a texture is two-dimensional or a six-face cubemap.
enum class TextureKind {
    Tex2D, ///< A single two-dimensional image.
    Cube   ///< Six square faces sampled by direction.
};

/// Describes a GPU buffer allocation.
struct BufferDesc {
    uint64_t size = 0;      ///< Allocation size in bytes.
    std::string_view label; ///< Diagnostic object label.
};
/// Provides access to an immutable-size GPU buffer.
class Buffer {
public:
    /// Destroys the buffer after its owning device has finished using it.
    virtual ~Buffer() = default;
    /// Returns the allocation size in bytes.
    virtual uint64_t size() const = 0;
};

/// Describes a GPU texture allocation and its allowed usages.
struct TextureDesc {
    uint32_t width = 0, height = 0;     ///< Extent in texels; cubemaps require equal dimensions.
    Format format = Format::BGRA8Unorm; ///< Pixel format.
    /// Cube requires width == height; width/height then describe one face, and the texture has
    /// six of them.
    TextureKind kind = TextureKind::Tex2D; ///< Texture dimensionality.
    /// 1..floor(log2(max(width, height))) + 1 -- the full chain down to a single texel. Levels
    /// beyond 0 are filled by the createTexture upload below; a caller with only level 0 in hand
    /// (Engine/Scene.cpp's unbaked-DDS fallback) passes a full mipLevels-sized span with the
    /// remaining entries null, leaving those levels' GPU content undefined until a future upload.
    uint32_t mipLevels = 1;    ///< Number of mip levels allocated for each face.
    bool renderTarget = false; ///< Enables render-target use.
    bool sampled = false;      ///< Enables shader reads.
    bool cpuReadback = false;  ///< Enables blocking CPU readback through shared storage.
    std::string_view label;    ///< Diagnostic object label.
};

/// Describes the CPU upload payload for one mip level of one texture face.
///
/// bytesPerRow is the caller's because only the caller knows the source layout: for an
/// uncompressed format it is width * bytesPerTexel, but for BC1 a "row" is a row of 4x4 *blocks*
/// -- ((width + 3) / 4) * 8 -- and rows 1..3 of every block live inside that same stride. Note the
/// rounding is *up*: a width of 6 is two blocks (16 bytes), not one, because the sixth column
/// still needs a block to live in. Truncating division agrees with the ceiling on every power of
/// two and disagrees on everything else, which is a stride bug that fails silently.
///
/// The RHI deliberately does not derive any of this: a decoder that hands over a padded or
/// block-aligned buffer would then have to un-pad it first.
///
/// data == nullptr leaves that level untouched -- undefined GPU content until something else
/// uploads it. A caller with only level 0 in hand still passes a full mipLevels * faceCount span,
/// with the remaining entries left null.
struct TextureMip {
    const void* data = nullptr; ///< Source bytes, or null to leave the level untouched.
    uint64_t bytesPerRow = 0;   ///< Source row stride in bytes.
};

/// Provides dimensions and optional CPU readback for a GPU texture.
class Texture {
public:
    /// Destroys the texture after its owning device has finished using it.
    virtual ~Texture() = default;
    /// Returns the texture width in texels.
    virtual uint32_t width() const = 0;
    /// Returns the texture height in texels.
    virtual uint32_t height() const = 0;
    /// Blocking readback of the full texture (requires cpuReadback). out must hold exactly
    /// width * height * bytesPerPixel(format) bytes, tightly packed, in the format's own channel
    /// order -- see bytesPerPixel in RHI/Validate.h, which also decides which formats readback
    /// accepts at all. Caller ensures GPU work completed (Device::waitIdle).
    /// Copies the full tightly packed texture into `out` after GPU work has completed.
    virtual void readback(void* out, uint64_t outSize) = 0;
};

/// How a texture is being used at a barrier boundary. Grows per real feature demand, exactly
/// like the rest of this header (ADR 0004).
/// Identifies texture use on either side of an explicit barrier.
enum class TextureUse {
    RenderTarget, ///< Written as a render-pass attachment.
    ShaderRead    ///< Read by a shader.
};

/// Selects nearest or linear texture filtering.
enum class FilterMode {
    Nearest, ///< Selects the nearest sample.
    Linear   ///< Linearly interpolates neighboring samples.
};
/// Selects wrapping or clamping outside normalized texture coordinates.
enum class AddressMode {
    Wrap, ///< Repeats coordinates outside the normalized range.
    Clamp ///< Clamps coordinates to the texture edge.
};
/// Grows per demand. LessEqual is the shadow compare for a conventional depth buffer; GreaterEqual
/// is its reversed-Z counterpart, where the larger stored depth is the nearer surface.
/// Selects the depth comparison performed by a comparison sampler.
enum class CompareFunc {
    Never,       ///< Creates a non-comparison sampler.
    LessEqual,   ///< Passes when the sampled value is less than or equal to the reference.
    GreaterEqual ///< Passes when the sampled value is greater than or equal to the reference.
};
/// Describes immutable sampler filtering, addressing, anisotropy, and comparison behavior.
struct SamplerDesc {
    FilterMode filter = FilterMode::Linear; ///< Shared minification, magnification, and mip filter.
    AddressMode addressMode = AddressMode::Wrap; ///< Shared addressing mode for every axis.
    uint32_t maxAnisotropy = 1;               ///< Maximum anisotropy in the inclusive range 1..16.
    CompareFunc compare = CompareFunc::Never; ///< Comparison function; Never disables comparison.
    std::string_view label;                   ///< Diagnostic object label.
};
/// Immutable once created, and cheap enough that the backend keeps no cache: a sampler is a
/// handful of descriptor bits, so callers create the two or three the frame needs at startup
/// and hold them for the device's lifetime.
/// Represents an immutable GPU sampler.
class Sampler {
public:
    /// Destroys the sampler.
    virtual ~Sampler() = default;
};

/// Represents a loaded backend shader library.
class ShaderLibrary {
public:
    /// Destroys the shader library.
    virtual ~ShaderLibrary() = default;
};

/// Selects solid or wireframe rasterization.
enum class FillMode {
    Solid,    ///< Rasterizes filled triangles.
    Wireframe ///< Rasterizes triangle edges.
};
/// Back culls back faces, where "front" is the project's one winding convention: counter-clockwise
/// when viewed from outside, which is what Source/Render/Mesh.cpp and Source/Engine's generators
/// produce and what Tests/RenderTests.cpp pins. The backend states the matching front-face winding
/// on the encoder rather than inheriting the API default, so the convention holds no matter what a
/// given API's default happens to be.
/// Selects triangle culling against the project's counter-clockwise front-face convention.
enum class CullMode {
    None, ///< Disables triangle culling.
    Back  ///< Culls back-facing triangles.
};
/// The depth comparison a pipeline draws with, when depthTestEnable is set. LessEqual exists for
/// the sky, which is drawn at exactly the far plane (the z = w trick) and would fail a strict Less
/// against a cleared depth buffer. Greater and GreaterEqual are the reversed-Z pair: with the near
/// plane at 1 and the far plane at 0, the nearer fragment is the numerically larger one, so a pass
/// clears depth to 0 and keeps what compares Greater.
/// Selects the depth comparison performed by a graphics pipeline.
enum class DepthCompare {
    Less,        ///< Passes when incoming depth is less than stored depth.
    LessEqual,   ///< Passes when incoming depth is less than or equal to stored depth.
    Greater,     ///< Passes when incoming depth is greater than stored depth.
    GreaterEqual ///< Passes when incoming depth is greater than or equal to stored depth.
};
/// Offsets a fragment's depth to keep a surface from shadowing itself. constant is in units of the
/// depth format's smallest resolvable difference; slopeScale multiplies the polygon's depth slope,
/// which is what covers steeply-angled geometry; clamp caps the total (0 = uncapped).
/// Describes the rasterized depth offset used to reduce surface self-shadowing.
struct DepthBias {
    float constant = 0.f;   ///< Constant depth offset in minimum depth-resolution units.
    float slopeScale = 0.f; ///< Multiplier applied to the polygon's depth slope.
    float clamp = 0.f;      ///< Absolute bias clamp, or zero for no clamp.
};

/// Describes shader entries, attachments, and fixed state for a graphics pipeline.
struct GraphicsPipelineDesc {
    ShaderLibrary* library = nullptr; ///< Shader library containing both entry points.
    std::string_view vertexEntry;     ///< Vertex-stage entry-point name.
    std::string_view fragmentEntry;   ///< Fragment-stage entry-point name.
    /// Unknown = no color attachment: a depth-only pipeline, for the depth-only passes below. Its
    /// fragment entry must then write no color (a void fragment function) -- a fragment output with
    /// no attachment to land in is a pipeline-creation failure, not a silently ignored write.
    Format colorFormat = Format::BGRA8Unorm;
    /// Unknown = no depth attachment. Metal 4 pipelines carry no depth pixel format (it is a
    /// render-pass property there) -- this field is validated CPU-side against the depth flags
    /// and kept in the desc because the future Vulkan backend bakes it into the pipeline.
    Format depthFormat = Format::Unknown;
    bool depthTestEnable = false;                   ///< Enables comparison using `depthCompare`.
    bool depthWriteEnable = false;                  ///< Enables writes to the depth attachment.
    FillMode fillMode = FillMode::Solid;            ///< Triangle fill mode.
    CullMode cullMode = CullMode::Back;             ///< Triangle culling mode.
    DepthCompare depthCompare = DepthCompare::Less; ///< Depth comparison function.
    /// Applied by the passes this pipeline is bound in. Harmless without a depth attachment (there
    /// is no depth to offset), so it is not an error -- just inert.
    DepthBias depthBias;
    std::string_view label; ///< Diagnostic object label.
};
/// Represents an immutable graphics pipeline.
class GraphicsPipeline {
public:
    /// Destroys the graphics pipeline.
    virtual ~GraphicsPipeline() = default;
};

/// Describes render-pass attachments, clear operations, and its diagnostic label.
struct RenderPassDesc {
    Texture* colorTarget = nullptr; ///< Color attachment, or null for a depth-only pass.
    float clearColor[4] = {0.f, 0.f, 0.f, 1.f}; ///< Scene-linear RGBA clear value.
    bool clear = true;                          ///< Clears the color attachment when true.
    Texture* depthTarget = nullptr;             ///< Depth attachment, or null when unused.
    float clearDepth = 1.0f;                    ///< Depth clear value.
    bool storeDepth = false;                    ///< Preserves depth after the pass when true.
    /// Names the pass in GPU captures and validation diagnostics. Backends provide a stable
    /// fallback for an empty label, but production passes should use a subsystem-qualified name.
    std::string_view label;
};

/// Records one frame's render passes, bindings, barriers, and draw commands.
class CommandList {
public:
    /// Destroys the command list through its owning device.
    virtual ~CommandList() = default;
    /// Begins a render pass using the supplied attachments and load actions.
    virtual void beginRenderPass(const RenderPassDesc& desc) = 0;
    /// Binds a graphics pipeline for subsequent draws.
    virtual void bindPipeline(GraphicsPipeline& pipeline) = 0;
    /// Binds a buffer at the given argument-table buffer slot -- vertex buffers, read here by
    /// bindless vertex-pulling (StructuredBuffer, indexed with SV_VertexID), and any other buffer
    /// a shader addresses directly. Buffer slots are their own index space, shared with
    /// setUniforms below: slot 0 here and slot 0 in setUniforms are the same binding, so two
    /// different resources must not be bound to the same slot index within one pass. Texture and
    /// sampler slots (bindTexture, bindSampler) are separate index spaces again -- slot 0 in any
    /// one of the three does not collide with slot 0 in either other. Valid only inside a render
    /// pass.
    /// Binds a buffer to an argument-table buffer slot in the active render pass.
    virtual void bindBuffer(uint32_t slot, Buffer& buffer) = 0;
    /// Binds a texture for shader reads at the given argument-table texture slot. Texture slots
    /// are their own index space -- slot 0 here and buffer slot 0 coexist. Valid only inside a
    /// render pass; the texture must carry shader-read usage. This backend grants shader-read
    /// usage for both sampled = true and cpuReadback = true descriptors (so a texture created for
    /// CPU readback may be sampled from).
    /// Binds a shader-readable texture to an argument-table texture slot.
    virtual void bindTexture(uint32_t slot, Texture& texture) = 0;
    /// Binds a sampler at the given argument-table sampler slot. Sampler slots are their own
    /// index space, like texture slots -- so slot 0 here coexists with texture slot 0 and buffer
    /// slot 0. Valid only inside a render pass.
    /// Binds a sampler to an argument-table sampler slot.
    virtual void bindSampler(uint32_t slot, Sampler& sampler) = 0;
    /// Copies `size` bytes into the frame's transient uniform ring and binds the copy's GPU
    /// address at the given argument-table buffer slot for subsequent draws -- the same index
    /// space bindBuffer above binds into. The data is captured at call time -- the caller may
    /// reuse or free its buffer immediately. Valid only inside a render pass. Ring capacity is a
    /// fixed per-frame budget; exhausting it is fatal (LMX_ASSERT) -- grow the backend constant
    /// when a real scene hits it.
    /// Copies transient uniform data and binds it to an argument-table buffer slot.
    virtual void setUniforms(uint32_t slot, const void* data, uint64_t size) = 0;
    /// Records a non-indexed draw.
    virtual void draw(uint32_t vertexCount, uint32_t firstVertex = 0) = 0;
    /// Indexed draw. Indices are uint32 (the only index type this RHI models); the index buffer
    /// is any Buffer holding them -- Metal 4 consumes it per-draw by GPU address, so there is no
    /// separate index-buffer bind state. firstIndex is an element offset into the buffer.
    /// Records an indexed draw using 32-bit indices from `indexBuffer`.
    virtual void drawIndexed(Buffer& indexBuffer, uint32_t indexCount, uint32_t firstIndex = 0) = 0;
    /// Ends the active render pass.
    virtual void endRenderPass() = 0;
    /// Transitions a texture between the supported pass-boundary usages.
    virtual void textureBarrier(Texture& texture, TextureUse from, TextureUse to) = 0;
};

/// Describes a swapchain bound to a native presentation layer.
struct SwapchainDesc {
    void* nativeLayer = nullptr;        ///< Native CAMetalLayer supplied by the windowing layer.
    uint32_t width = 0, height = 0;     ///< Drawable extent in pixels.
    Format format = Format::BGRA8Unorm; ///< Presentation color format.
};
/// Lifetime: a Swapchain must not outlive the Device that created it, and must be destroyed
/// before the native surface it was built on. Destruction blocks until all GPU work referencing
/// the swapchain has completed — a backend may not release presentation resources out from under
/// in-flight command buffers — so no waitIdle() is required around it.
/// Owns presentation resources created by a device for one native surface.
class Swapchain {
public:
    /// Waits for outstanding use and destroys the presentation resources.
    virtual ~Swapchain() = default;
    /// Acquires the next presentation texture, valid through endFrame.
    virtual Result<Texture*> acquireNextTexture() = 0; // valid until endFrame/present
    /// Resizes the presentation surface.
    virtual void resize(uint32_t width, uint32_t height) = 0;
};

/// How long the GPU spent on one render pass, measured on the device timeline by timestamps the
/// backend writes at the pass boundaries -- callers record nothing.
///
/// label is the RenderPassDesc label the pass was begun with, with the backend's unnamed-pass
/// fallback substituted for an empty one. gpuMilliseconds covers the whole pass, load and store
/// actions included, and is wall time on the GPU rather than a sum of shader costs: a pass that
/// overlaps another still reports its own span, so times across a frame may add up to more than
/// the frame took.
/// Reports the GPU duration associated with one labeled render pass.
struct PassTiming {
    std::string label;            ///< Render-pass diagnostic label.
    double gpuMilliseconds = 0.0; ///< Measured GPU wall time in milliseconds.
};

/// Selects optional behavior when creating an RHI device.
struct DeviceDesc {
    bool enableValidation = true; ///< Enables backend validation diagnostics.
};
/// Creates GPU resources and controls the three-frames-in-flight frame loop.
class Device {
public:
    /// Destroys the device and its backend state.
    virtual ~Device() = default;
    /// Creates a swapchain for a native presentation surface.
    virtual Result<std::unique_ptr<Swapchain>> createSwapchain(const SwapchainDesc&) = 0;
    /// Creates a GPU buffer and optionally uploads its initial contents.
    virtual Result<std::unique_ptr<Buffer>> createBuffer(const BufferDesc&,
                                                         const void* initialData) = 0;
    /// mips uploads initial content. Empty = no upload (a render target, or a texture a later pass
    /// fills). Otherwise mips.size() must be mipLevels * faceCount (6 for Cube, 1 for Tex2D),
    /// ordered mip-major per face: face0[mip0..N], face1[mip0..N], ... Anything else is a caller
    /// error and asserts.
    virtual Result<std::unique_ptr<Texture>>
    createTexture(const TextureDesc&, std::span<const TextureMip> mips = {}) = 0;
    /// Creates an immutable sampler.
    virtual Result<std::unique_ptr<Sampler>> createSampler(const SamplerDesc&) = 0;
    /// pathNoExt: resolves "<pathNoExt>.metallib" (precompiled) first, else
    /// "<pathNoExt>.metal" (runtime-compiled MSL, Metal 4 language version).
    virtual Result<std::unique_ptr<ShaderLibrary>>
    /// Loads the shader library resolved from `pathNoExt`.
    loadShaderLibrary(std::string_view pathNoExt) = 0;
    /// Creates an immutable graphics pipeline.
    virtual Result<std::unique_ptr<GraphicsPipeline>>
    /// Creates a graphics pipeline from the supplied descriptor.
    createGraphicsPipeline(const GraphicsPipelineDesc&) = 0;

    /// Frame loop: beginFrame blocks on pacing (3 in flight), returns the frame CommandList.
    /// endFrame commits; if presentTo != nullptr, presents its acquired texture.
    /// Begins a paced frame and returns its command list.
    virtual CommandList& beginFrame() = 0;
    /// Commits the frame and optionally presents through `presentTo`.
    virtual void endFrame(Swapchain* presentTo) = 0;
    /// Blocks until all work submitted to this device completes.
    virtual void waitIdle() = 0;

    /// Per-pass GPU times of one past frame, in the order that frame began its passes.
    ///
    /// The reported frame is the newest one the GPU had finished by the time of the most recent
    /// beginFrame(). That lag is not an implementation detail to be tuned away: a frame's
    /// timestamps are written by the GPU as it executes, so they are readable only once that frame
    /// retires, and this RHI resolves them at the one point retirement is already proven --
    /// beginFrame's pacing wait. Reading them any earlier would mean stalling the CPU on the GPU
    /// mid-frame.
    ///
    /// Consequences a caller can rely on:
    ///   - empty until a beginFrame() observes a retired frame, so the whole first frame reports
    ///     nothing;
    ///   - to obtain the timings of a *specific* frame, end it, waitIdle(), then call beginFrame()
    ///     once more -- that call publishes exactly that frame;
    ///   - in a continuous loop the readout trails the open frame by a few frames and never stalls.
    /// A frame with no passes reports an empty span, not the previous frame's numbers.
    ///
    /// The span is owned by the device and is invalidated by the next beginFrame().
    /// Returns the newest retired frame's pass timings until the next beginFrame call.
    virtual std::span<const PassTiming> passTimings() const = 0;

    /// Returns the backend device's human-readable name.
    virtual std::string_view deviceName() const = 0;
};

/// Creates the platform RHI device.
Result<std::unique_ptr<Device>> createDevice(const DeviceDesc& desc = {});

} // namespace lmx::rhi
