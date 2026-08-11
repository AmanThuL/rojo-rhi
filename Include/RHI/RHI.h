//----------------------------------------------------------------------------------------------------------------------
/// @file RHI.h
/// @brief Declares the backend-neutral rendering hardware interface.
//----------------------------------------------------------------------------------------------------------------------
#pragma once
#include "RHI/Result.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

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

/// Describes a GPU buffer allocation and its allowed usages.
///
/// Vertex, index, and uniform reads need no usage flag: they are what a buffer is for. The storage
/// flags below are the ones a shader's read-write bindings need, and they exist so that a
/// bindStorageBuffer with an access the buffer was never created for is a caller error with a
/// message rather than undefined shader behavior.
struct BufferDesc {
    uint64_t size = 0;         ///< Allocation size in bytes.
    bool storageRead = false;  ///< Enables shader reads through a storage binding.
    bool storageWrite = false; ///< Enables shader writes through a storage binding.
    bool cpuReadback = false;  ///< Enables blocking CPU readback of the buffer's contents.
    std::string_view label;    ///< Diagnostic object label.
};
/// Provides access to an immutable-size GPU buffer.
class Buffer {
public:
    /// Destroys the buffer after its owning device has finished using it.
    virtual ~Buffer() = default;
    /// Returns the allocation size in bytes.
    virtual uint64_t size() const = 0;
    /// Blocking readback of the buffer's leading bytes (requires cpuReadback). outSize must not
    /// exceed size(), and the caller is responsible for having completed the GPU work that wrote
    /// the range (Device::waitIdle) -- this call performs no synchronization of its own.
    /// Copies the first `outSize` bytes of the buffer into `out` after GPU work has completed.
    virtual void readback(void* out, uint64_t outSize) = 0;
};

/// BufferRange::size: every byte from `offset` to the end of the allocation.
inline constexpr uint64_t kWholeBuffer = ~uint64_t{0};

/// Names the bytes of a buffer that a barrier covers.
///
/// The default covers the whole allocation, which is what a caller with no sub-range detail means;
/// a narrower range is how one pass's slice of a shared buffer is ordered against another's. Ranges
/// are validated against the buffer they are used with: an empty range, or one running past the end
/// of the allocation, is a caller error.
struct BufferRange {
    uint64_t offset = 0;          ///< First byte in the range.
    uint64_t size = kWholeBuffer; ///< Bytes covered, or kWholeBuffer.
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
    /// Enables shader reads through a storage binding. Distinct from `sampled` because a storage
    /// read is an unfiltered fetch through a read-write binding, which restricts the format.
    bool storageRead = false;
    bool storageWrite = false; ///< Enables shader writes through a storage binding.
    bool cpuReadback = false;  ///< Enables blocking CPU readback through shared storage.
    std::string_view label;    ///< Diagnostic object label.
};

/// mipLevelCount: every level from baseMipLevel to the end of the chain.
inline constexpr uint32_t kAllMipLevels = ~uint32_t{0};
/// arrayLayerCount: every layer from baseArrayLayer to the last one.
inline constexpr uint32_t kAllArrayLayers = ~uint32_t{0};

/// Names the mip levels and array layers of a texture that a view, binding, or barrier covers.
///
/// The default covers the whole resource, which is what every whole-resource declaration uses; a
/// narrower range is how a pass addresses one mip of a chain (a bloom step writing mip N+1 while
/// reading mip N), or one layer in a copy or barrier. Ranges are validated against the texture they
/// are used with: an empty range, or one running past the end of the chain, is a caller error.
struct TextureSubresourceRange {
    uint32_t baseMipLevel = 0;                  ///< First mip level in the range.
    uint32_t mipLevelCount = kAllMipLevels;     ///< Mip levels covered, or kAllMipLevels.
    uint32_t baseArrayLayer = 0;                ///< First array layer (cube face) in the range.
    uint32_t arrayLayerCount = kAllArrayLayers; ///< Layers covered, or kAllArrayLayers.
};

/// Describes the subresources and format a binding sees a texture through.
///
/// A default-constructed view is the whole texture in its own format, which is what a binding that
/// wants no reinterpretation passes. `format` reinterprets the same bits under a different transfer
/// function -- the sRGB and linear members of one format family -- and is validated against the
/// texture's family: a view may not reinterpret a format as one with a different bit layout.
struct TextureViewDesc {
    TextureSubresourceRange range; ///< Subresources the view exposes.
    /// Format the view reads and writes through, or Format::Unknown for the texture's own format.
    Format format = Format::Unknown;
};

/// Names the rectangle of one subresource that a copy command reads or writes.
///
/// A region addresses exactly one mip level of one array layer: a copy spanning several is several
/// copy calls, which is what keeps the addressing unambiguous. The origin and extent are in texels
/// of `mipLevel`, not of level zero, so copying mip 2 of a 64x64 chain uses a 16x16 extent.
///
/// `z` and `depth` complete the vocabulary a volume texture needs. This RHI models no 3D texture
/// kind, so until one exists `z` must be 0 and `depth` must be 1; they are stated here rather than
/// omitted so that adding the kind does not change the shape of every copy call.
///
/// The extent has no whole-mip default on purpose: a copy that silently resized itself to whatever
/// the texture happened to be is the kind of thing that works until the texture changes.
struct TextureCopyRegion {
    uint32_t mipLevel = 0;   ///< Mip level the region addresses.
    uint32_t arrayLayer = 0; ///< Array layer -- a cube face -- the region addresses.
    uint32_t x = 0;          ///< Origin x in texels of `mipLevel`.
    uint32_t y = 0;          ///< Origin y in texels of `mipLevel`.
    uint32_t z = 0;          ///< Origin z; must be 0 until a 3D texture kind exists.
    uint32_t width = 0;      ///< Extent width in texels of `mipLevel`.
    uint32_t height = 0;     ///< Extent height in texels of `mipLevel`.
    uint32_t depth = 1;      ///< Extent depth; must be 1 until a 3D texture kind exists.
};

/// Describes how a region's texels are laid out on the buffer side of a buffer<->texture copy.
///
/// `bytesPerRow` is the caller's for the same reason TextureMip::bytesPerRow is: only the caller
/// knows whether its rows are tightly packed or padded, and deriving a stride here would force a
/// caller holding a padded buffer to un-pad it first. It is a stride of the *region*, so a copy of
/// a 16x16 rectangle of RGBA8 texels is tightly packed at 64 bytes, whatever the texture's width.
///
/// `bytesPerSlice` is the distance between two array slices. Every copy this RHI can express covers
/// a single slice, so zero -- meaning "one slice, no image stride" -- is the value to pass; it is
/// named because a future array or volume copy has nowhere else to state it.
struct BufferTextureLayout {
    uint64_t offset = 0;        ///< First byte of the region's texels within the buffer.
    uint64_t bytesPerRow = 0;   ///< Distance in bytes between the starts of two rows.
    uint64_t bytesPerSlice = 0; ///< Distance in bytes between two slices, or 0 for a single slice.
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
    /// Returns the pixel format the texture was created with.
    virtual Format format() const = 0;
    /// Returns the number of mip levels each face was allocated with.
    virtual uint32_t mipLevels() const = 0;
    /// Returns the number of array layers: one for a 2D texture, six for a cubemap's faces.
    virtual uint32_t arrayLayers() const = 0;
    /// Blocking readback of the full texture (requires cpuReadback). out must hold exactly
    /// width * height * bytesPerPixel(format) bytes, tightly packed, in the format's own channel
    /// order -- see bytesPerPixel in RHI/Validate.h, which also decides which formats readback
    /// accepts at all. Caller ensures GPU work completed (Device::waitIdle).
    /// Copies the full tightly packed texture into `out` after GPU work has completed.
    virtual void readback(void* out, uint64_t outSize) = 0;
};

/// What one resource costs inside a heap: the bytes it occupies and the alignment its placement
/// offset must satisfy. Both are the backend's answer for that exact descriptor, so a caller that
/// packs resources into a heap asks rather than assuming a texel-count-times-size arithmetic that
/// no driver promises.
struct SizeAlign {
    uint64_t size = 0;      ///< Bytes the resource occupies in a heap.
    uint64_t alignment = 0; ///< Alignment, in bytes, its heap offset must be a multiple of.
};

/// Describes a placement heap: one device allocation resources are created inside at explicit
/// offsets.
struct HeapDesc {
    uint64_t size = 0;      ///< Allocation size in bytes.
    std::string_view label; ///< Diagnostic object label.
};

/// Owns one block of device-private memory that placed resources are created inside at
/// caller-chosen offsets.
///
/// Placement is explicit and the heap tracks no hazards of its own: two resources whose byte ranges
/// overlap share those bytes, and nothing in the backend orders one against the other. Every
/// ordering between a placed resource and whatever last used the memory under it is the caller's,
/// stated through the barriers on CommandList -- which is exactly what makes a heap the substrate a
/// render graph aliases transients in, and what makes an unstated overlap silent corruption rather
/// than a stall.
///
/// A Heap must outlive every resource placed in it, and must not outlive the Device that created
/// it. Destroying it while the GPU still reads a resource placed in it is a caller error: the
/// three-frames-in-flight pacing is what a caller proves retirement with.
class Heap {
public:
    /// Destroys the heap after its owning device has finished using it.
    virtual ~Heap() = default;
    /// Returns the heap's size in bytes.
    virtual uint64_t size() const = 0;
};

/// How a texture is being used at a barrier boundary. Grows per real feature demand, exactly
/// like the rest of this header (ADR 0004).
/// Identifies texture use on either side of an explicit barrier.
enum class TextureUse {
    RenderTarget,   ///< Written as a render-pass attachment.
    ShaderRead,     ///< Read by a shader.
    StorageRead,    ///< Read through a storage binding.
    StorageWrite,   ///< Written through a storage binding.
    CopySource,     ///< Read by a copy command.
    CopyDestination ///< Written by a copy command.
};

/// The same, for buffers -- the resource kind fillBuffer, the storage bindings, and the indirect
/// argument reads all hazard on, and which no texture edge can honestly stand in for.
/// Identifies buffer use on either side of an explicit barrier.
enum class BufferUse {
    /// Read by a shader through a non-storage binding: uniforms, vertex pulling, or indices.
    ShaderRead,
    StorageRead,     ///< Read through a storage binding.
    StorageWrite,    ///< Written through a storage binding.
    CopySource,      ///< Read by a copy command.
    CopyDestination, ///< Written by a copy command or by fillBuffer.
    IndirectArgument ///< Read by the GPU as the arguments of an indirect dispatch or draw.
};

/// Additional memory visibility a barrier must establish beyond ordinary access to one resource.
enum class BarrierOptions : uint8_t {
    None = 0,         ///< Orders ordinary accesses to one logical resource.
    ResourceAlias = 1 ///< Makes reused physical memory visible through a different resource.
};

/// Combines independent barrier visibility requirements.
constexpr BarrierOptions operator|(BarrierOptions a, BarrierOptions b) {
    return static_cast<BarrierOptions>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

/// Returns true when `options` includes `option`.
constexpr bool hasBarrierOption(BarrierOptions options, BarrierOptions option) {
    return (static_cast<uint8_t>(options) & static_cast<uint8_t>(option)) != 0;
}

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

/// Describes the kernel and threadgroup shape of a compute pipeline.
///
/// threadsPerThreadgroup restates the kernel's own `[numthreads]` because the Slang-to-Metal path
/// does not carry it into the compiled library: the shading language expects the host to supply the
/// threadgroup size at dispatch. Stating it once here keeps CommandList::dispatch a count of
/// threadgroups rather than a second place where the shader's shape has to be repeated. It is the
/// caller's job to keep the two in step -- a mismatch is a shader that reads out of its own bounds,
/// not a pipeline-creation failure.
struct ComputePipelineDesc {
    ShaderLibrary* library = nullptr; ///< Shader library containing the kernel.
    std::string_view computeEntry;    ///< Compute-stage entry-point name.
    /// Threads per threadgroup in x, y, z; must equal the kernel's `[numthreads]` and each
    /// component must be at least one.
    uint32_t threadsPerThreadgroup[3] = {1, 1, 1};
    std::string_view label; ///< Diagnostic object label.
};
/// Represents an immutable compute pipeline.
class ComputePipeline {
public:
    /// Destroys the compute pipeline.
    virtual ~ComputePipeline() = default;
};

/// Declares what a shader does with a storage binding, so that validation and a future backend's
/// state tracking know the intent without inspecting the shader. This backend does not track
/// resource state, so the declaration constrains only which resources may be bound where.
enum class StorageAccess {
    Read,     ///< The shader only reads the binding.
    Write,    ///< The shader only writes the binding.
    ReadWrite ///< The shader both reads and writes the binding.
};

/// Byte alignment every indirect-argument buffer offset must satisfy.
///
/// Four bytes, which is the alignment of the 32-bit members the three argument structs below are
/// made of and the alignment the backend's encoders require of the address they are handed. An
/// offset that is not a multiple of it is a caller error, not a slow path.
inline constexpr uint64_t kIndirectArgsAlignment = 4;

/// Layout contract shared by DispatchIndirectArgs, DrawIndirectArgs, and DrawIndexedIndirectArgs:
///
///   - each is standard-layout and made only of 4-byte scalars, so every member sits at its own
///     natural offset with no padding anywhere, including at the end -- an array of them is tightly
///     packed at sizeof(), and the static_asserts below pin that rather than trusting it;
///   - the buffer offset passed to the matching command is in bytes and must be a multiple of
///     kIndirectArgsAlignment;
///   - the GPU reads these fields directly out of the buffer, so a caller writing them from a
///     compute kernel must lay the same 32-bit words out in the same order. The kernel's own
///     structure declaration is not checked against this one by anything; the conformance tests are
///     what pin the two together.
///
/// The structs are RHI-owned rather than backend-shaped: this backend's encoders happen to consume
/// exactly these fields in exactly this order, and a backend whose native layout differs must
/// translate rather than silently reinterpret.
/// Arguments the GPU reads for an indirect dispatch.
struct DispatchIndirectArgs {
    uint32_t threadgroupsX = 0; ///< Threadgroups dispatched in x; threads come from the pipeline.
    uint32_t threadgroupsY = 0; ///< Threadgroups dispatched in y.
    uint32_t threadgroupsZ = 0; ///< Threadgroups dispatched in z.
};
static_assert(std::is_standard_layout_v<DispatchIndirectArgs>);
static_assert(sizeof(DispatchIndirectArgs) == 12);
static_assert(offsetof(DispatchIndirectArgs, threadgroupsX) == 0);
static_assert(offsetof(DispatchIndirectArgs, threadgroupsY) == 4);
static_assert(offsetof(DispatchIndirectArgs, threadgroupsZ) == 8);

/// Arguments the GPU reads for an indirect non-indexed draw.
struct DrawIndirectArgs {
    uint32_t vertexCount = 0; ///< Vertices drawn per instance.
    /// Instances drawn. Defaults to one because zero is a draw that silently does nothing.
    uint32_t instanceCount = 1;
    uint32_t firstVertex = 0;   ///< First vertex index, added to the shader's vertex ID.
    uint32_t firstInstance = 0; ///< First instance index, added to the shader's instance ID.
};
static_assert(std::is_standard_layout_v<DrawIndirectArgs>);
static_assert(sizeof(DrawIndirectArgs) == 16);
static_assert(offsetof(DrawIndirectArgs, vertexCount) == 0);
static_assert(offsetof(DrawIndirectArgs, instanceCount) == 4);
static_assert(offsetof(DrawIndirectArgs, firstVertex) == 8);
static_assert(offsetof(DrawIndirectArgs, firstInstance) == 12);

/// Arguments the GPU reads for an indirect indexed draw, matching drawIndexed's uint32 index model.
struct DrawIndexedIndirectArgs {
    uint32_t indexCount = 0; ///< Indices drawn per instance.
    /// Instances drawn. Defaults to one for the same reason DrawIndirectArgs::instanceCount does.
    uint32_t instanceCount = 1;
    uint32_t firstIndex = 0; ///< First index, as an element offset into the index buffer.
    /// Added to every index before the vertex is fetched. Signed, so a draw may address vertices
    /// before the ones its indices name.
    int32_t baseVertex = 0;
    uint32_t firstInstance = 0; ///< First instance index, added to the shader's instance ID.
};
static_assert(std::is_standard_layout_v<DrawIndexedIndirectArgs>);
static_assert(sizeof(DrawIndexedIndirectArgs) == 20);
static_assert(offsetof(DrawIndexedIndirectArgs, indexCount) == 0);
static_assert(offsetof(DrawIndexedIndirectArgs, instanceCount) == 4);
static_assert(offsetof(DrawIndexedIndirectArgs, firstIndex) == 8);
static_assert(offsetof(DrawIndexedIndirectArgs, baseVertex) == 12);
static_assert(offsetof(DrawIndexedIndirectArgs, firstInstance) == 16);

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

/// Records one frame's render, compute, and copy passes, bindings, barriers, dispatches, and draws.
///
/// Exactly one pass is open at a time: every command below documents the scope it is valid in, and
/// calling it outside that scope is a sequencing bug the backend asserts on rather than a failure
/// it reports. Bindings live in the frame's argument table and therefore survive across passes of
/// the frame; a pass that depends on a slot binds it rather than inheriting whatever an earlier
/// pass left there.
class CommandList {
public:
    /// Number of buffer slots in the shared argument-table buffer namespace.
    static constexpr uint32_t kMaxBufferBindings = 8;
    /// Number of texture slots in the argument-table texture namespace.
    static constexpr uint32_t kMaxTextureBindings = 16;
    /// Number of sampler slots in the argument-table sampler namespace.
    static constexpr uint32_t kMaxSamplerBindings = 8;

    /// Destroys the command list through its owning device.
    virtual ~CommandList() = default;
    /// Begins a render pass using the supplied attachments and load actions.
    virtual void beginRenderPass(const RenderPassDesc& desc) = 0;
    /// Begins a compute pass. Inside it, bindComputePipeline, bindStorageBuffer,
    /// bindStorageTexture, the read-only texture/sampler/buffer binds, setUniforms, dispatch, and
    /// dispatchIndirect are valid; render-pass commands are not. `label` names the pass in GPU
    /// captures, validation diagnostics, and passTimings(); backends substitute a stable fallback
    /// for an empty one.
    /// Begins a labeled compute pass on this command list.
    virtual void beginComputePass(std::string_view label) = 0;
    /// Binds a compute pipeline for subsequent dispatches. Valid only inside a compute pass.
    virtual void bindComputePipeline(ComputePipeline& pipeline) = 0;
    /// Binds a buffer for shader reads and/or writes at the given argument-table buffer slot --
    /// the same index space bindBuffer and setUniforms use. `access` declares what the shader does
    /// with it and must be granted by the buffer's BufferDesc storage flags. Valid only inside a
    /// compute pass. Ordering against other passes is not implied: a dispatch that must see an
    /// earlier pass's writes needs an explicit barrier.
    /// Binds a storage buffer with declared access to an argument-table buffer slot.
    virtual void bindStorageBuffer(uint32_t slot, Buffer& buffer, StorageAccess access) = 0;
    /// Binds a texture for shader reads and/or writes at the given argument-table texture slot --
    /// the same index space bindTexture uses. `view` selects the subresources and format the
    /// shader addresses, defaulting to the whole texture in its own format, and `access` declares
    /// what the shader does with them; both must be granted by the texture's TextureDesc storage
    /// flags and format. Valid only inside a compute pass. Like bindStorageBuffer, this implies no
    /// ordering: a dispatch that must see an earlier pass's writes needs an explicit barrier.
    /// Binds a storage texture view with declared access to an argument-table texture slot.
    virtual void bindStorageTexture(uint32_t slot, Texture& texture, const TextureViewDesc& view,
                                    StorageAccess access) = 0;
    /// Dispatches a grid of threadgroups; each argument is a count of *threadgroups*, not of
    /// threads, and the threads within one come from the bound pipeline's threadsPerThreadgroup.
    /// Every count must be greater than zero. Valid only inside a compute pass, after
    /// bindComputePipeline. Dispatches within one pass carry no ordering guarantee at all --
    /// neighboring dispatches may overlap execution, and there is no barrier that orders them,
    /// because textureBarrier is valid only between passes. A dispatch that must see another's
    /// writes therefore belongs in a later pass: end this one, record the barrier, and begin the
    /// next.
    /// Records a compute dispatch of the given threadgroup counts.
    virtual void dispatch(uint32_t threadgroupsX, uint32_t threadgroupsY,
                          uint32_t threadgroupsZ) = 0;
    /// Dispatches the threadgroup counts a DispatchIndirectArgs sitting at `offset` bytes into
    /// `argumentBuffer` holds; threads per threadgroup still come from the bound pipeline, because
    /// the GPU-side struct carries counts only. Valid only inside a compute pass, after
    /// bindComputePipeline. `offset` must be a multiple of kIndirectArgsAlignment and the whole
    /// struct must fit in the buffer.
    ///
    /// The arguments are read by the GPU at execution, so whatever wrote them must be ordered
    /// against this dispatch: a CPU-filled buffer is ordered by the frame boundary, and a buffer an
    /// earlier pass wrote needs a bufferBarrier ending in BufferUse::IndirectArgument. Their values
    /// are never validated -- nothing on the CPU can read them -- so a count of zero is a dispatch
    /// that does nothing rather than the caller error dispatch() would report.
    /// Records a compute dispatch whose threadgroup counts the GPU reads from a buffer.
    virtual void dispatchIndirect(Buffer& argumentBuffer, uint64_t offset) = 0;
    /// Ends the active compute pass.
    virtual void endComputePass() = 0;

    /// Begins a copy pass. Inside it, copyBuffer, copyBufferToTexture, copyTextureToBuffer,
    /// copyTexture, and fillBuffer are valid, and nothing else is: a copy pass has no pipeline and
    /// no bindings, so binds, draws, and dispatches all belong to one of the other two scopes.
    /// `label` names the pass in GPU captures, validation diagnostics, and passTimings() exactly as
    /// the other two pass kinds' labels do, and a copy pass is timed like them.
    ///
    /// Copies within one pass carry no ordering guarantee against each other, for the same reason
    /// dispatch() states: ordering between two pieces of work is what a barrier between two passes
    /// expresses, and there is no in-pass barrier. Copies of disjoint ranges are the normal
    /// contents of one pass.
    /// Begins a labeled copy pass on this command list.
    virtual void beginCopyPass(std::string_view label) = 0;
    /// Copies `size` bytes between two buffers, or between two ranges of one buffer as long as the
    /// ranges do not overlap. Both ranges must lie inside their allocation. Valid only inside a
    /// copy pass.
    /// Copies a byte range from one buffer into another.
    virtual void copyBuffer(Buffer& source, uint64_t sourceOffset, Buffer& destination,
                            uint64_t destinationOffset, uint64_t size) = 0;
    /// Copies texels out of a buffer into one rectangle of one subresource. `layout` describes the
    /// buffer side -- where the texels start and how their rows are strided -- and `region` the
    /// texture side. The bytes the two together address must lie inside the buffer, and the region
    /// inside the addressed mip level. Valid only inside a copy pass.
    /// Copies buffer bytes into a texture subresource region.
    virtual void copyBufferToTexture(Buffer& source, const BufferTextureLayout& layout,
                                     Texture& destination, const TextureCopyRegion& region) = 0;
    /// The reverse, and the way an arbitrary mip level or array layer is read back: copy the
    /// subresource into a buffer created with BufferDesc.cpuReadback, then read that buffer once
    /// the GPU work has completed. Texture::readback covers only the whole of level zero, so this
    /// is the only path to any other subresource. Valid only inside a copy pass.
    /// Copies a texture subresource region into buffer bytes.
    virtual void copyTextureToBuffer(Texture& source, const TextureCopyRegion& region,
                                     Buffer& destination, const BufferTextureLayout& layout) = 0;
    /// Copies one subresource rectangle to another, in the same texture or a different one. The two
    /// regions must have the same extent -- a copy does not filter or rescale -- and the two
    /// textures the same format: reinterpreting one format's bits as another's is what a texture
    /// view is for. Overlapping regions of one subresource are a caller error. Valid only inside a
    /// copy pass.
    /// Copies a texture subresource region into another texture subresource region.
    virtual void copyTexture(Texture& source, const TextureCopyRegion& sourceRegion,
                             Texture& destination, const TextureCopyRegion& destinationRegion) = 0;
    /// Writes `value` into every one of the `size` bytes starting at `offset`. The value is a
    /// *byte*, not a word: filling with a 32-bit pattern is not something this expresses, and the
    /// use it exists for is clearing an accumulation buffer to zero. The range must lie inside the
    /// buffer and must not be empty. Valid only inside a copy pass.
    /// Fills a buffer range with a repeated byte value.
    virtual void fillBuffer(Buffer& buffer, uint64_t offset, uint64_t size, uint8_t value) = 0;
    /// Ends the active copy pass.
    virtual void endCopyPass() = 0;
    /// Binds a graphics pipeline for subsequent draws.
    virtual void bindPipeline(GraphicsPipeline& pipeline) = 0;
    /// Binds a buffer at the given argument-table buffer slot -- vertex buffers, read here by
    /// bindless vertex-pulling (StructuredBuffer, indexed with SV_VertexID), and any other buffer
    /// a shader addresses directly. Buffer slots are their own index space, shared with
    /// setUniforms below: slot 0 here and slot 0 in setUniforms are the same binding, so two
    /// different resources must not be bound to the same slot index within one pass. Texture and
    /// sampler slots (bindTexture, bindSampler) are separate index spaces again -- slot 0 in any
    /// one of the three does not collide with slot 0 in either other. Valid inside a render or a
    /// compute pass.
    /// Binds a buffer to an argument-table buffer slot in the active pass.
    virtual void bindBuffer(uint32_t slot, Buffer& buffer) = 0;
    /// Binds a texture view for shader reads at the given argument-table texture slot. `view`
    /// defaults to every subresource in the texture's own format. Texture slots are their own index
    /// space -- slot 0 here and buffer slot 0 coexist. Valid inside a render or a compute pass; the
    /// texture must carry shader-read usage. This backend grants
    /// shader-read usage for sampled = true, storageRead = true, and cpuReadback = true
    /// descriptors (so a texture created for CPU readback may be sampled from).
    /// Binds a shader-readable texture to an argument-table texture slot.
    virtual void bindTexture(uint32_t slot, Texture& texture, const TextureViewDesc& view = {}) = 0;
    /// Binds a sampler at the given argument-table sampler slot. Sampler slots are their own
    /// index space, like texture slots -- so slot 0 here coexists with texture slot 0 and buffer
    /// slot 0. Valid inside a render or a compute pass.
    /// Binds a sampler to an argument-table sampler slot.
    virtual void bindSampler(uint32_t slot, Sampler& sampler) = 0;
    /// Copies `size` bytes into the frame's transient uniform ring and binds the copy's GPU
    /// address at the given argument-table buffer slot for subsequent draws -- the same index
    /// space bindBuffer above binds into. The data is captured at call time -- the caller may
    /// reuse or free its buffer immediately. Valid inside a render or a compute pass. Ring
    /// capacity is a
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
    /// Draws the DrawIndirectArgs sitting at `offset` bytes into `argumentBuffer`. Valid only
    /// inside a render pass, after bindPipeline. `offset` must be a multiple of
    /// kIndirectArgsAlignment and the whole struct must fit in the buffer; the argument values
    /// themselves are never validated, exactly as dispatchIndirect documents, and must be ordered
    /// against whatever wrote them the same way.
    /// Records a non-indexed draw whose arguments the GPU reads from a buffer.
    virtual void drawIndirect(Buffer& argumentBuffer, uint64_t offset) = 0;
    /// The indexed counterpart, taking the index buffer per draw exactly as drawIndexed does --
    /// the count, the first index, the base vertex, and the instancing all come from the
    /// DrawIndexedIndirectArgs in `argumentBuffer` instead of from arguments here.
    /// Records an indexed draw whose arguments the GPU reads from a buffer.
    virtual void drawIndexedIndirect(Buffer& indexBuffer, Buffer& argumentBuffer,
                                     uint64_t offset) = 0;
    /// Ends the active render pass.
    virtual void endRenderPass() = 0;
    /// Orders the given subresources of `texture` between two uses, and makes the writes of the
    /// earlier use visible to the later one. Valid only between passes.
    ///
    /// At least one side must be a write: two reads have no hazard to order. Which passes the
    /// barrier separates is positional -- everything encoded before this call is the producer, and
    /// the next pass to open is the consumer -- so a barrier recorded with no pass after it is a
    /// dependency nothing consumes and is a caller error.
    ///
    /// `range` states the subresources the dependency covers, which is what a caller declares and
    /// what a graph reasons about. A backend may synchronize more than the range asks for: this
    /// one does, because Metal 4 tracks no resource state and its barriers order pipeline stages
    /// rather than subresources, so every barrier is at least a whole-queue stage dependency.
    ///
    /// What a barrier does *not* cover is the other consumers. The dependency is scoped to the
    /// consuming pass's own stage class -- the kind of encoder the pass opens -- because that is
    /// the side a backend can only express in the terms of the pass it is emitting for; this one
    /// records it as the consuming encoder's first command, naming the producing stages and its
    /// own. Passes of one stage class are ordered among themselves, so one barrier serves every
    /// later consumer of that class; a consumer of a *different* class needs its own barrier, over
    /// the same subresources and the same producing use, however wide the first one's range was.
    /// A render graph derives its barriers from this rule, and a caller hand-encoding two passes
    /// of different kinds over one producer's output owes each of them a barrier.
    ///
    /// ResourceAlias additionally flushes memory being reused through a different logical resource.
    /// The `texture` argument is the resource the consumer pass will use; ordinary same-resource
    /// transitions leave `options` at None.
    /// Orders a texture's subresources between a producing and a consuming use.
    virtual void textureBarrier(Texture& texture, const TextureSubresourceRange& range,
                                TextureUse from, TextureUse to,
                                BarrierOptions options = BarrierOptions::None) = 0;
    /// Same, for the whole texture -- the common case, and what a pass that declares no
    /// subresource detail means.
    /// Orders a whole texture between a producing and a consuming use.
    void textureBarrier(Texture& texture, TextureUse from, TextureUse to,
                        BarrierOptions options = BarrierOptions::None) {
        textureBarrier(texture, TextureSubresourceRange{}, from, to, options);
    }
    /// The same contract for a buffer: the range, the at-least-one-write rule, the positional
    /// producer and consumer, and the backend's freedom to synchronize more than the range asks for
    /// all read exactly as textureBarrier's do. It exists because a buffer hazard has no texture to
    /// borrow -- a fillBuffer whose zeros an accumulating dispatch must see, or arguments a compute
    /// pass writes for a later indirect draw, are dependencies on bytes, and expressing them
    /// through some unrelated texture's edge would be a lie a graph would later reason from.
    /// ResourceAlias has the same meaning as on textureBarrier: `buffer` is the resource receiving
    /// physical memory that a different logical resource used earlier.
    /// Orders a buffer's byte range between a producing and a consuming use.
    virtual void bufferBarrier(Buffer& buffer, const BufferRange& range, BufferUse from,
                               BufferUse to, BarrierOptions options = BarrierOptions::None) = 0;
    /// Same, for the whole buffer -- the common case, and what a pass that declares no byte-range
    /// detail means.
    /// Orders a whole buffer between a producing and a consuming use.
    void bufferBarrier(Buffer& buffer, BufferUse from, BufferUse to,
                       BarrierOptions options = BarrierOptions::None) {
        bufferBarrier(buffer, BufferRange{}, from, to, options);
    }
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

/// How long the GPU spent on one pass of any kind, measured on the device timeline by timestamps
/// the backend writes at the pass boundaries -- callers record nothing.
///
/// label is the label the pass was begun with, with the backend's unnamed-pass fallback
/// substituted for an empty one. gpuMilliseconds covers the whole pass, load and store actions
/// included, and is wall time on the GPU rather than a sum of shader costs: a pass that overlaps
/// another still reports its own span, so times across a frame may add up to more than the frame
/// took.
/// Reports the GPU duration associated with one labeled pass.
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

    /// Creates a placement heap for resources the caller positions itself. See Heap for the
    /// hazard-tracking contract the placed resources inherit.
    virtual Result<std::unique_ptr<Heap>> createHeap(const HeapDesc&) = 0;

    /// Creates a texture occupying `heap` from `offset`, which must be a multiple of the alignment
    /// textureSizeAlign(desc) reports and must leave that call's size within the heap. The texture
    /// has no initial contents -- a placed resource is device-private, so `desc` may not ask for
    /// cpuReadback and there is no upload span -- and it must not outlive the heap.
    virtual Result<std::unique_ptr<Texture>> createPlacedTexture(Heap&, uint64_t offset,
                                                                 const TextureDesc&) = 0;
    /// The buffer counterpart of createPlacedTexture, on the same terms and with bufferSizeAlign as
    /// the size and alignment source.
    virtual Result<std::unique_ptr<Buffer>> createPlacedBuffer(Heap&, uint64_t offset,
                                                               const BufferDesc&) = 0;

    /// Reports what `desc` costs inside a heap. Answered by the backend for that exact descriptor
    /// and stable for the device's lifetime, so a caller may plan a whole heap's layout from it
    /// before creating anything.
    virtual SizeAlign textureSizeAlign(const TextureDesc&) const = 0;
    /// The buffer counterpart of textureSizeAlign.
    virtual SizeAlign bufferSizeAlign(const BufferDesc&) const = 0;

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
    /// Creates an immutable compute pipeline.
    virtual Result<std::unique_ptr<ComputePipeline>>
    /// Creates a compute pipeline from the supplied descriptor.
    createComputePipeline(const ComputePipelineDesc&) = 0;

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

    /// Which frame passTimings() describes, so a caller can attribute a measurement to the frame
    /// it came from rather than to "some frame a few back". Frames are numbered in beginFrame()
    /// order starting at one, so a caller that counts its own beginFrame() calls shares the
    /// numbering; zero means nothing has been published yet.
    /// Returns the frame number the current pass timings were measured on.
    virtual uint64_t passTimingsFrame() const = 0;

    /// The newest frame beginFrame() has opened, in the same numbering passTimingsFrame() reports:
    /// frames count from one in beginFrame() order, and zero means beginFrame() has not been called
    /// yet. It names the frame being *built* where passTimingsFrame() names the frame most recently
    /// *measured*, so the two are equal only in the degenerate case of a drained device, and a
    /// caller joining CPU-side per-frame state to retired GPU timings compares one against the
    /// other rather than assuming they agree.
    ///
    /// The value does not change while a frame is open, so everything recorded between beginFrame()
    /// and endFrame() belongs to the number this reports.
    /// Returns the number of the newest frame beginFrame has opened.
    virtual uint64_t frameNumber() const = 0;

    /// Returns the backend device's human-readable name.
    virtual std::string_view deviceName() const = 0;
};

/// Creates the platform RHI device.
Result<std::unique_ptr<Device>> createDevice(const DeviceDesc& desc = {});

} // namespace lmx::rhi
