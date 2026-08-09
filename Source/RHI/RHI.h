#pragma once
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace lmx::rhi {

enum class ErrorCode {
    DeviceUnsupported,
    ShaderLoadFailed,
    PipelineCreationFailed,
    ResourceCreationFailed,
    SwapchainFailed,
    InvalidDesc,
};
struct Error {
    ErrorCode code;
    std::string message;
};
template <typename T>
using Result = std::expected<T, Error>;

// RGBA16Float is the scene-linear color format: half precision keeps radiance above 1.0 that an
// 8-bit unorm target would clamp away. RG16Float carries two-channel lookup tables and is sampled
// only -- like the rest of this header it grows per real demand (ADR 0004).
enum class Format {
    Unknown,
    BGRA8Unorm,
    RGBA8Unorm,
    RGBA8Unorm_sRGB,
    RGBA16Float,
    RG16Float,
    BC1Unorm,
    BC1Unorm_sRGB,
    D32Float
};

// Cube is six square faces in the +X, -X, +Y, -Y, +Z, -Z order every graphics API agrees on;
// a shader samples it with a direction rather than a uv. Tex2DArray and Tex3D are deliberately
// absent until a feature demands them (ADR 0004).
enum class TextureKind { Tex2D, Cube };

struct BufferDesc {
    uint64_t size = 0;
    std::string_view label;
};
class Buffer {
public:
    virtual ~Buffer() = default;
    virtual uint64_t size() const = 0;
};

struct TextureDesc {
    uint32_t width = 0, height = 0;
    Format format = Format::BGRA8Unorm;
    // Cube requires width == height; width/height then describe one face, and the texture has
    // six of them.
    TextureKind kind = TextureKind::Tex2D;
    // 1..floor(log2(max(width, height))) + 1 -- the full chain down to a single texel. Levels
    // beyond 0 are filled by the createTexture upload below; a caller with only level 0 in hand
    // (Engine/Scene.cpp's unbaked-DDS fallback) passes a full mipLevels-sized span with the
    // remaining entries null, leaving those levels' GPU content undefined until a future upload.
    uint32_t mipLevels = 1;
    bool renderTarget = false;
    bool sampled = false;     // bound for shader reads after rendering (scene RT, shadow maps)
    bool cpuReadback = false; // shared storage; enables readback()
    std::string_view label;
};

// One mip level of one face, as the CPU holds it before upload.
//
// bytesPerRow is the caller's because only the caller knows the source layout: for an
// uncompressed format it is width * bytesPerTexel, but for BC1 a "row" is a row of 4x4 *blocks*
// -- ((width + 3) / 4) * 8 -- and rows 1..3 of every block live inside that same stride. Note the
// rounding is *up*: a width of 6 is two blocks (16 bytes), not one, because the sixth column
// still needs a block to live in. Truncating division agrees with the ceiling on every power of
// two and disagrees on everything else, which is a stride bug that fails silently.
//
// The RHI deliberately does not derive any of this: a decoder that hands over a padded or
// block-aligned buffer would then have to un-pad it first.
//
// data == nullptr leaves that level untouched -- undefined GPU content until something else
// uploads it. A caller with only level 0 in hand still passes a full mipLevels * faceCount span,
// with the remaining entries left null.
struct TextureMip {
    const void* data = nullptr;
    uint64_t bytesPerRow = 0;
};

class Texture {
public:
    virtual ~Texture() = default;
    virtual uint32_t width() const = 0;
    virtual uint32_t height() const = 0;
    // Blocking readback of the full texture (requires cpuReadback). out must hold exactly
    // width * height * bytesPerPixel(format) bytes, tightly packed, in the format's own channel
    // order -- see bytesPerPixel in RHI/Validate.h, which also decides which formats readback
    // accepts at all. Caller ensures GPU work completed (Device::waitIdle).
    virtual void readback(void* out, uint64_t outSize) = 0;
};

// How a texture is being used at a barrier boundary. Grows per real feature demand, exactly
// like the rest of this header (ADR 0004).
enum class TextureUse { RenderTarget, ShaderRead };

enum class FilterMode { Nearest, Linear };
enum class AddressMode { Wrap, Clamp };
// Grows per demand. LessEqual is the shadow compare for a conventional depth buffer; GreaterEqual
// is its reversed-Z counterpart, where the larger stored depth is the nearer surface.
enum class CompareFunc { Never, LessEqual, GreaterEqual };
struct SamplerDesc {
    FilterMode filter = FilterMode::Linear;      // min/mag/mip together (thin on purpose)
    AddressMode addressMode = AddressMode::Wrap; // all axes
    uint32_t maxAnisotropy = 1;                  // 1..16
    CompareFunc compare = CompareFunc::Never;    // != Never makes a comparison sampler
    std::string_view label;
};
// Immutable once created, and cheap enough that the backend keeps no cache: a sampler is a
// handful of descriptor bits, so callers create the two or three the frame needs at startup
// and hold them for the device's lifetime.
class Sampler {
public:
    virtual ~Sampler() = default;
};

class ShaderLibrary {
public:
    virtual ~ShaderLibrary() = default;
};

enum class FillMode { Solid, Wireframe };
// Back culls back faces, where "front" is the project's one winding convention: counter-clockwise
// when viewed from outside, which is what Source/Render/Mesh.cpp and Source/Engine's generators
// produce and what Tests/RenderTests.cpp pins. The backend states the matching front-face winding
// on the encoder rather than inheriting the API default, so the convention holds no matter what a
// given API's default happens to be.
enum class CullMode { None, Back };
// The depth comparison a pipeline draws with, when depthTestEnable is set. LessEqual exists for
// the sky, which is drawn at exactly the far plane (the z = w trick) and would fail a strict Less
// against a cleared depth buffer. Greater and GreaterEqual are the reversed-Z pair: with the near
// plane at 1 and the far plane at 0, the nearer fragment is the numerically larger one, so a pass
// clears depth to 0 and keeps what compares Greater.
enum class DepthCompare { Less, LessEqual, Greater, GreaterEqual };
// Offsets a fragment's depth to keep a surface from shadowing itself. constant is in units of the
// depth format's smallest resolvable difference; slopeScale multiplies the polygon's depth slope,
// which is what covers steeply-angled geometry; clamp caps the total (0 = uncapped).
struct DepthBias {
    float constant = 0.f;
    float slopeScale = 0.f;
    float clamp = 0.f;
};

struct GraphicsPipelineDesc {
    ShaderLibrary* library = nullptr;
    std::string_view vertexEntry;
    std::string_view fragmentEntry;
    // Unknown = no color attachment: a depth-only pipeline, for the depth-only passes below. Its
    // fragment entry must then write no color (a void fragment function) -- a fragment output with
    // no attachment to land in is a pipeline-creation failure, not a silently ignored write.
    Format colorFormat = Format::BGRA8Unorm;
    // Unknown = no depth attachment. Metal 4 pipelines carry no depth pixel format (it is a
    // render-pass property there) -- this field is validated CPU-side against the depth flags
    // and kept in the desc because the future Vulkan backend bakes it into the pipeline.
    Format depthFormat = Format::Unknown;
    bool depthTestEnable = false; // compares with depthCompare when enabled
    bool depthWriteEnable = false;
    FillMode fillMode = FillMode::Solid;
    CullMode cullMode = CullMode::Back;
    DepthCompare depthCompare = DepthCompare::Less;
    // Applied by the passes this pipeline is bound in. Harmless without a depth attachment (there
    // is no depth to offset), so it is not an error -- just inert.
    DepthBias depthBias;
    std::string_view label;
};
class GraphicsPipeline {
public:
    virtual ~GraphicsPipeline() = default;
};

struct RenderPassDesc {
    Texture* colorTarget = nullptr;
    float clearColor[4] = {0.f, 0.f, 0.f, 1.f};
    bool clear = true;
    Texture* depthTarget = nullptr;
    float clearDepth = 1.0f;
    bool storeDepth = false;
    // Names the pass in GPU captures and validation diagnostics. Backends provide a stable
    // fallback for an empty label, but production passes should use a subsystem-qualified name.
    std::string_view label;
};

class CommandList {
public:
    virtual ~CommandList() = default;
    virtual void beginRenderPass(const RenderPassDesc& desc) = 0;
    virtual void bindPipeline(GraphicsPipeline& pipeline) = 0;
    // Binds a buffer at the given argument-table buffer slot -- vertex buffers, read here by
    // bindless vertex-pulling (StructuredBuffer, indexed with SV_VertexID), and any other buffer
    // a shader addresses directly. Buffer slots are their own index space, shared with
    // setUniforms below: slot 0 here and slot 0 in setUniforms are the same binding, so two
    // different resources must not be bound to the same slot index within one pass. Texture and
    // sampler slots (bindTexture, bindSampler) are separate index spaces again -- slot 0 in any
    // one of the three does not collide with slot 0 in either other. Valid only inside a render
    // pass.
    virtual void bindBuffer(uint32_t slot, Buffer& buffer) = 0;
    // Binds a texture for shader reads at the given argument-table texture slot. Texture slots
    // are their own index space -- slot 0 here and buffer slot 0 coexist. Valid only inside a
    // render pass; the texture must carry shader-read usage. This backend grants shader-read
    // usage for both sampled = true and cpuReadback = true descriptors (so a texture created for
    // CPU readback may be sampled from).
    virtual void bindTexture(uint32_t slot, Texture& texture) = 0;
    // Binds a sampler at the given argument-table sampler slot. Sampler slots are their own
    // index space, like texture slots -- so slot 0 here coexists with texture slot 0 and buffer
    // slot 0. Valid only inside a render pass.
    virtual void bindSampler(uint32_t slot, Sampler& sampler) = 0;
    // Copies `size` bytes into the frame's transient uniform ring and binds the copy's GPU
    // address at the given argument-table buffer slot for subsequent draws -- the same index
    // space bindBuffer above binds into. The data is captured at call time -- the caller may
    // reuse or free its buffer immediately. Valid only inside a render pass. Ring capacity is a
    // fixed per-frame budget; exhausting it is fatal (LMX_ASSERT) -- grow the backend constant
    // when a real scene hits it.
    virtual void setUniforms(uint32_t slot, const void* data, uint64_t size) = 0;
    virtual void draw(uint32_t vertexCount, uint32_t firstVertex = 0) = 0;
    // Indexed draw. Indices are uint32 (the only index type this RHI models); the index buffer
    // is any Buffer holding them -- Metal 4 consumes it per-draw by GPU address, so there is no
    // separate index-buffer bind state. firstIndex is an element offset into the buffer.
    virtual void drawIndexed(Buffer& indexBuffer, uint32_t indexCount, uint32_t firstIndex = 0) = 0;
    virtual void endRenderPass() = 0;
    virtual void textureBarrier(Texture& texture, TextureUse from, TextureUse to) = 0;
};

struct SwapchainDesc {
    void* nativeLayer = nullptr; // CAMetalLayer* — created by the windowing layer
    uint32_t width = 0, height = 0;
    Format format = Format::BGRA8Unorm;
};
// Lifetime: a Swapchain must not outlive the Device that created it, and must be destroyed
// before the native surface it was built on. Destruction blocks until all GPU work referencing
// the swapchain has completed — a backend may not release presentation resources out from under
// in-flight command buffers — so no waitIdle() is required around it.
class Swapchain {
public:
    virtual ~Swapchain() = default;
    virtual Result<Texture*> acquireNextTexture() = 0; // valid until endFrame/present
    virtual void resize(uint32_t width, uint32_t height) = 0;
};

// How long the GPU spent on one render pass, measured on the device timeline by timestamps the
// backend writes at the pass boundaries -- callers record nothing.
//
// label is the RenderPassDesc label the pass was begun with, with the backend's unnamed-pass
// fallback substituted for an empty one. gpuMilliseconds covers the whole pass, load and store
// actions included, and is wall time on the GPU rather than a sum of shader costs: a pass that
// overlaps another still reports its own span, so times across a frame may add up to more than
// the frame took.
struct PassTiming {
    std::string label;
    double gpuMilliseconds = 0.0;
};

struct DeviceDesc {
    bool enableValidation = true;
};
class Device {
public:
    virtual ~Device() = default;
    virtual Result<std::unique_ptr<Swapchain>> createSwapchain(const SwapchainDesc&) = 0;
    virtual Result<std::unique_ptr<Buffer>> createBuffer(const BufferDesc&,
                                                         const void* initialData) = 0;
    // mips uploads initial content. Empty = no upload (a render target, or a texture a later pass
    // fills). Otherwise mips.size() must be mipLevels * faceCount (6 for Cube, 1 for Tex2D),
    // ordered mip-major per face: face0[mip0..N], face1[mip0..N], ... Anything else is a caller
    // error and asserts.
    virtual Result<std::unique_ptr<Texture>>
    createTexture(const TextureDesc&, std::span<const TextureMip> mips = {}) = 0;
    virtual Result<std::unique_ptr<Sampler>> createSampler(const SamplerDesc&) = 0;
    // pathNoExt: resolves "<pathNoExt>.metallib" (precompiled) first, else
    // "<pathNoExt>.metal" (runtime-compiled MSL, Metal 4 language version).
    virtual Result<std::unique_ptr<ShaderLibrary>>
    loadShaderLibrary(std::string_view pathNoExt) = 0;
    virtual Result<std::unique_ptr<GraphicsPipeline>>
    createGraphicsPipeline(const GraphicsPipelineDesc&) = 0;

    // Frame loop: beginFrame blocks on pacing (3 in flight), returns the frame CommandList.
    // endFrame commits; if presentTo != nullptr, presents its acquired texture.
    virtual CommandList& beginFrame() = 0;
    virtual void endFrame(Swapchain* presentTo) = 0;
    virtual void waitIdle() = 0;

    // Per-pass GPU times of one past frame, in the order that frame began its passes.
    //
    // The reported frame is the newest one the GPU had finished by the time of the most recent
    // beginFrame(). That lag is not an implementation detail to be tuned away: a frame's timestamps
    // are written by the GPU as it executes, so they are readable only once that frame retires, and
    // this RHI resolves them at the one point retirement is already proven -- beginFrame's pacing
    // wait. Reading them any earlier would mean stalling the CPU on the GPU mid-frame.
    //
    // Consequences a caller can rely on:
    //   - empty until a beginFrame() observes a retired frame, so the whole first frame reports
    //     nothing;
    //   - to obtain the timings of a *specific* frame, end it, waitIdle(), then call beginFrame()
    //     once more -- that call publishes exactly that frame;
    //   - in a continuous loop the readout trails the open frame by a few frames and never stalls.
    // A frame with no passes reports an empty span, not the previous frame's numbers.
    //
    // The span is owned by the device and is invalidated by the next beginFrame().
    virtual std::span<const PassTiming> passTimings() const = 0;

    virtual std::string_view deviceName() const = 0;
};

Result<std::unique_ptr<Device>> createDevice(const DeviceDesc& desc = {});

} // namespace lmx::rhi
