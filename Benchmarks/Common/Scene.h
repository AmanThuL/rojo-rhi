//----------------------------------------------------------------------------------------------------------------------
/// @file Scene.h
/// @brief Declares the composed frame's scene as pure functions of an index: transforms, meshes, camera, cascades and culling.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include <array>
#include <cstdint>
#include <vector>

namespace rojoRHI::bench {
    // The scene's counts.
    inline constexpr uint32_t kBoxCount = 10'000;     ///< Boxes; box i uses mesh i mod kMeshCount.
    inline constexpr uint32_t kMeshCount = 64;        ///< Distinct box meshes.
    inline constexpr uint32_t kMeshVertexCount = 24;  ///< Vertices of one mesh: four per face.
    inline constexpr uint32_t kMeshIndexCount = 36;   ///< UInt16 indices of one mesh: two triangles per face.
    inline constexpr uint32_t kForwardBoxCount = 500; ///< Alpha-blended boxes drawn directly by the forward pass.
    inline constexpr uint32_t kCascadeCount = 3;      ///< Shadow cascades.
    inline constexpr uint32_t kViewCount = 4;         ///< Culled views: cascades 0 to 2, then the camera.
    inline constexpr uint32_t kKeptPerView = 6'000;   ///< Boxes kept by the culling rule in every view.
    inline constexpr uint32_t kForwardBoxStride = 20; ///< Forward box j is scene box kForwardBoxStride * j.
    inline constexpr uint32_t kFrameWidth = 1920;     ///< Pixels of the camera's targets.
    inline constexpr uint32_t kFrameHeight = 1080;

    // Four floats, 16-byte aligned like a float4 on every target.
    struct alignas(16) Float4
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        float w = 0.0f;
    };

    // A 4x4 matrix as four float4 rows, 64 bytes: the instance transform record and the view-projection value. A point
    // p = (x, y, z, 1) maps to (dot(row0, p), dot(row1, p), dot(row2, p), dot(row3, p)).
    struct Transform
    {
        Float4 row0;
        Float4 row1;
        Float4 row2;
        Float4 row3;
    };

    // One mesh vertex, 32 bytes: object-space position and normal (w unused).
    struct MeshVertex
    {
        Float4 position;
        Float4 normal;
    };

    // One alpha-blended box of the forward pass.
    struct ForwardBox
    {
        uint32_t box = 0; ///< The scene box it draws; its mesh is box mod kMeshCount.
        Transform transform;
        Float4 color; ///< Alpha below 1.
    };

    /// The model transform of box `box` of kBoxCount: a uniform scale, a rotation about Y and a translation on a 100 by
    /// 100 grid, bottom row (0, 0, 0, 1).
    Transform instanceTransform(uint32_t box);

    /// The 24 vertices of mesh `mesh` of kMeshCount, four per face in the order +X, -X, +Y, -Y, +Z, -Z, wound
    /// counter-clockwise seen from outside.
    std::array<MeshVertex, kMeshVertexCount> meshVertices(uint32_t mesh);

    /// The 36 indices of mesh `mesh`, local to its own 24 vertices: all below kMeshVertexCount.
    std::array<uint16_t, kMeshIndexCount> meshIndices(uint32_t mesh);

    /// All kBoxCount transforms in box order, the bytes of the `instances` buffer.
    std::vector<Transform> allInstances();

    /// The vertices of the 64 meshes one after another, the bytes of the `vertices` buffer: mesh m starts at vertex
    /// kMeshVertexCount * m.
    std::vector<MeshVertex> allVertices();

    /// The indices of the 64 meshes one after another, the bytes of the `indices` buffer: mesh m starts at index
    /// kMeshIndexCount * m, and a draw adds baseVertex kMeshVertexCount * m.
    std::vector<uint16_t> allIndices();

    /// The camera's view-projection with reverse Z: depth 1 at the near plane and 0 at the far plane, tested with Greater.
    Transform cameraViewProjection();

    /// The inverse of cameraViewProjection(), for rebuilding a world position from a depth sample.
    Transform cameraInverseViewProjection();

    /// The orthographic view-projection of shadow cascade `cascade` of kCascadeCount, reverse Z like the camera's.
    Transform cascadeViewProjection(uint32_t cascade);

    /// The direction the light travels, normalized, w = 0.
    Float4 lightDirection();

    /// Forward box `index` of kForwardBoxCount, in index order.
    ForwardBox forwardBox(uint32_t index);

    /// Whether the culling rule keeps box `box` in every view: box mod 5 below 3.
    bool isKept(uint32_t box);

    /// The record index of a kept box: 3 * (box / 5) + box mod 5.
    uint32_t recordIndex(uint32_t box);

    /// The record the culling kernel writes for kept box `box`: drawId box, 36 indices from the first index of mesh
    /// box mod 64, one instance, base vertex of that mesh, first instance 0.
    DrawIndexedIndirectArgs keptRecord(uint32_t box);

    /// The kKeptPerView records of one view in record order, what every view's array holds after the culling kernel.
    std::vector<DrawIndexedIndirectArgs> cullRecords();

    /// Applies a transform to a point, returning the four row products.
    Float4 transformPoint(const Transform& transform, const Float4& point);
} // namespace rojoRHI::bench
