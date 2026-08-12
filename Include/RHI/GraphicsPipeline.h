//----------------------------------------------------------------------------------------------------------------------
/// @file GraphicsPipeline.h
/// @brief Declares graphics pipelines, their descriptor, and their fixed-function vocabulary.
//----------------------------------------------------------------------------------------------------------------------
#pragma once
#include "RHI/Format.h"

#include <string_view>

namespace lmx::rhi {

class ShaderLibrary;

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

} // namespace lmx::rhi
