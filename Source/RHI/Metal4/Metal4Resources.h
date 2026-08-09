#pragma once
#include "RHI/Metal4/Metal4Common.h"

#include <cstdint>
#include <utility>

namespace lmx::rhi::metal4 {

// Concrete RHI objects for the Metal 4 backend. Each is a thin owning wrapper: it holds the
// metal-cpp object in an NS::SharedPtr (same TransferPtr ownership convention as
// Metal4Device -- see the note on that class) and exposes handle() so sibling backend files
// can reach the native object. handle() is deliberately not on the RHI interfaces, so no
// Metal type escapes into RHI.h.
//
// Construction is always through Metal4Device::create*/loadShaderLibrary, which is where
// desc validation and labeling happen; the wrappers themselves own, report, and manage
// residency membership (below).

class ResidencyRegistration {
public:
    ResidencyRegistration() = default;
    ResidencyRegistration(NS::SharedPtr<MTL::ResidencySet> residency,
                          const MTL::Allocation* allocation);
    ~ResidencyRegistration();

    ResidencyRegistration(const ResidencyRegistration&) = delete;
    ResidencyRegistration& operator=(const ResidencyRegistration&) = delete;

private:
    NS::SharedPtr<MTL::ResidencySet> m_residency;
    const MTL::Allocation* m_allocation = nullptr;
};

class Metal4Buffer final : public Buffer {
public:
    Metal4Buffer(NS::SharedPtr<MTL::Buffer> buffer, NS::SharedPtr<MTL::ResidencySet> residency)
        : m_buffer(std::move(buffer)), m_residency(std::move(residency), m_buffer.get()) {}

    // Drops this buffer's capture-schema entry. Identity contract with
    // Metal4Device::createBuffer: the key is the MTL::Buffer pointer, which is m_buffer.get()
    // here and buffer.get() there -- the same object, since the wrapper took ownership of it.
    // Out of line so the header needs no CaptureSchema include.
    ~Metal4Buffer() override;

    uint64_t size() const override { return m_buffer->length(); }

    MTL::Buffer* handle() const { return m_buffer.get(); }

private:
    // m_residency is declared last so it is destroyed *first* (reverse declaration order),
    // while m_buffer still holds the allocation it has to unregister.
    NS::SharedPtr<MTL::Buffer> m_buffer;
    ResidencyRegistration m_residency;
};

class Metal4Texture final : public Texture {
public:
    Metal4Texture(NS::SharedPtr<MTL::Texture> texture, uint32_t width, uint32_t height,
                  bool cpuReadback, NS::SharedPtr<MTL::ResidencySet> residency)
        : m_texture(std::move(texture)), m_width(width), m_height(height),
          m_cpuReadback(cpuReadback), m_residency(std::move(residency), m_texture.get()) {}

    // Drops this texture's capture-schema entry -- same identity contract as ~Metal4Buffer, with
    // the MTL::Texture pointer Metal4Device::createTexture registered.
    ~Metal4Texture() override;

    uint32_t width() const override { return m_width; }
    uint32_t height() const override { return m_height; }
    void readback(void* out, uint64_t outSize) override;

    MTL::Texture* handle() const { return m_texture.get(); }

private:
    // Dimensions are cached from the desc rather than queried from MTL::Texture on every
    // call: they are immutable for the texture's lifetime and the getters are hot enough
    // (per-readback bounds math) that an objc_msgSend each is pure overhead.
    NS::SharedPtr<MTL::Texture> m_texture;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    bool m_cpuReadback = false;
    // Declared last -- see the note in Metal4Buffer.
    ResidencyRegistration m_residency;
};

// No ResidencyRegistration, unlike every other wrapper here: MTL::SamplerState is not an
// MTL::Allocation (MTLSampler.hpp:141 -- it derives from NS::Referencing, not from Allocation),
// so a residency set has nothing to take. Argument tables bind it by MTL::ResourceID all the
// same, which is what setSupportArgumentBuffers on its descriptor buys.
class Metal4Sampler final : public Sampler {
public:
    explicit Metal4Sampler(NS::SharedPtr<MTL::SamplerState> sampler)
        : m_sampler(std::move(sampler)) {}

    MTL::SamplerState* handle() const { return m_sampler.get(); }

private:
    NS::SharedPtr<MTL::SamplerState> m_sampler;
};

class Metal4ShaderLibrary final : public ShaderLibrary {
public:
    explicit Metal4ShaderLibrary(NS::SharedPtr<MTL::Library> library)
        : m_library(std::move(library)) {}

    MTL::Library* handle() const { return m_library.get(); }

private:
    NS::SharedPtr<MTL::Library> m_library;
};

// The whole of CullMode::Back's meaning, in one constant: which winding Metal is to treat as
// front-facing. RHI.h pins the project convention as counter-clockwise seen from outside, and a
// right-handed projection (glm::perspectiveRH_ZO, Render/Camera.cpp) keeps it counter-clockwise
// through to clip space -- so the pipelines that cull ask Metal for exactly that.
//
// Stated rather than inherited even though it happens to be Metal's default behaviour for our
// geometry: MTL::Winding is documented in terms of the framebuffer coordinate system, whose origin
// is top-left with +y down, and the reasonable-sounding conclusion from that -- that the viewport's
// y-mirror turns our clip-space CCW into a framebuffer CW, so WindingClockwise is the value to pass
// -- is *wrong*. Measured on this backend rather than reasoned: with WindingClockwise here, 11 of
// the GPU cases (whose geometry is counter-clockwise in clip space by construction) came back
// showing nothing but their clear colour; with WindingCounterClockwise all of them render, and
// Tests/GpuSmokeTests.cpp's raster-state case pins it from both sides. A future backend must be
// measured the same way rather than ported from this line.
constexpr MTL::Winding kFrontFacingWinding = MTL::WindingCounterClockwise;

// Everything a GraphicsPipelineDesc asks for that Metal keeps on the *encoder* rather than in the
// pipeline state: fill mode, culling and depth bias. Converted once at creation and replayed by
// bindPipeline, so one RHI bind still fully describes the pipeline's rasterisation -- the same
// trick the depth-stencil state below plays, for the same reason (the future Vulkan backend bakes
// all of it into the pipeline object and could not hand it out separately).
struct Metal4RasterState {
    MTL::TriangleFillMode fillMode = MTL::TriangleFillModeFill;
    MTL::CullMode cullMode = MTL::CullModeNone;
    // Which winding counts as front-facing. Carried per pipeline even though every pipeline
    // currently agrees on it, because it is meaningless without the cull mode it pairs with --
    // splitting them would let one be replayed without the other.
    MTL::Winding frontFacingWinding = kFrontFacingWinding;
    float depthBias = 0.0f;
    float slopeScale = 0.0f;
    float biasClamp = 0.0f;
};

class Metal4Pipeline final : public GraphicsPipeline {
public:
    Metal4Pipeline(NS::SharedPtr<MTL::RenderPipelineState> state,
                   NS::SharedPtr<MTL::DepthStencilState> depthState,
                   const Metal4RasterState& rasterState)
        : m_state(std::move(state)), m_depthState(std::move(depthState)),
          m_rasterState(rasterState) {}

    MTL::RenderPipelineState* handle() const { return m_state.get(); }

    // Null for a pipeline whose desc enabled neither depth test nor depth write -- Metal's
    // default depth-stencil state (compare Always, writes off) is already exactly that, so
    // bindPipeline simply skips the bind rather than creating a no-op state per pipeline.
    MTL::DepthStencilState* depthState() const { return m_depthState.get(); }

    const Metal4RasterState& rasterState() const { return m_rasterState; }

private:
    // MTL4::Compiler hands back a plain MTL::RenderPipelineState -- Metal 4 reuses the
    // Metal 3 pipeline-state type, only the descriptor and the compiler entry point are new.
    //
    // The depth-stencil state is a *separate* object bound alongside the pipeline rather than
    // baked into it: in Metal it is encoder state, not pipeline state. The RHI hides that split
    // (both come from one GraphicsPipelineDesc) because the future Vulkan backend bakes depth
    // into the pipeline and could not expose it separately.
    //
    // Unlike Metal4Buffer/Metal4Texture there is no ordering constraint between these two
    // members: neither references the other, so declaration order is arbitrary and release
    // order does not matter.
    NS::SharedPtr<MTL::RenderPipelineState> m_state;
    NS::SharedPtr<MTL::DepthStencilState> m_depthState;
    Metal4RasterState m_rasterState;
};

} // namespace lmx::rhi::metal4
