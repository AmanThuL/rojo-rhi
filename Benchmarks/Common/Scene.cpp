//----------------------------------------------------------------------------------------------------------------------
/// @file Scene.cpp
/// @brief Implements the composed frame's scene functions in single precision, computed once by the caller and uploaded.
//----------------------------------------------------------------------------------------------------------------------
#include "Common/Scene.h"

#include <array>
#include <cmath>
#include <numbers>
#include <utility>

namespace rojoRHI::bench {
    namespace {
        struct Vector3
        {
            float x = 0.0f;
            float y = 0.0f;
            float z = 0.0f;
        };

        // Rows as plain arrays, for the matrix products and the inverse.
        using Rows = std::array<std::array<float, 4>, 4>;

        //==============================================================================================================
        Vector3 subtract(Vector3 a, Vector3 b)
        {
            return { a.x - b.x, a.y - b.y, a.z - b.z };
        }

        //==============================================================================================================
        float dot(Vector3 a, Vector3 b)
        {
            return a.x * b.x + a.y * b.y + a.z * b.z;
        }

        //==============================================================================================================
        Vector3 cross(Vector3 a, Vector3 b)
        {
            return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
        }

        //==============================================================================================================
        Vector3 normalize(Vector3 v)
        {
            const float length = std::sqrt(dot(v, v));
            return { v.x / length, v.y / length, v.z / length };
        }

        //==============================================================================================================
        Rows toRows(const Transform& transform)
        {
            const Float4* source[4] = { &transform.row0, &transform.row1, &transform.row2, &transform.row3 };
            Rows rows{};
            for (uint32_t row = 0; row < 4; ++row)
            {
                rows[row] = { source[row]->x, source[row]->y, source[row]->z, source[row]->w };
            }
            return rows;
        }

        //==============================================================================================================
        Transform fromRows(const Rows& rows)
        {
            const auto row = [&rows](uint32_t index) { return Float4{ rows[index][0], rows[index][1], rows[index][2], rows[index][3] }; };
            return Transform{ row(0), row(1), row(2), row(3) };
        }

        //==============================================================================================================
        Rows multiply(const Rows& a, const Rows& b)
        {
            Rows result{};
            for (uint32_t row = 0; row < 4; ++row)
            {
                for (uint32_t column = 0; column < 4; ++column)
                {
                    for (uint32_t k = 0; k < 4; ++k)
                    {
                        result[row][column] += a[row][k] * b[k][column];
                    }
                }
            }
            return result;
        }

        //==============================================================================================================
        // Gauss-Jordan elimination with partial pivoting in single precision; the matrices here are never singular.
        Rows inverse(Rows m)
        {
            Rows result{};
            for (uint32_t i = 0; i < 4; ++i)
            {
                result[i][i] = 1.0f;
            }
            for (uint32_t column = 0; column < 4; ++column)
            {
                uint32_t pivot = column;
                for (uint32_t row = column + 1; row < 4; ++row)
                {
                    if (std::abs(m[row][column]) > std::abs(m[pivot][column]))
                    {
                        pivot = row;
                    }
                }
                std::swap(m[column], m[pivot]);
                std::swap(result[column], result[pivot]);
                const float scale = 1.0f / m[column][column];
                for (uint32_t k = 0; k < 4; ++k)
                {
                    m[column][k] *= scale;
                    result[column][k] *= scale;
                }
                for (uint32_t row = 0; row < 4; ++row)
                {
                    if (row == column)
                    {
                        continue;
                    }
                    const float factor = m[row][column];
                    for (uint32_t k = 0; k < 4; ++k)
                    {
                        m[row][k] -= factor * m[column][k];
                        result[row][k] -= factor * result[column][k];
                    }
                }
            }
            return result;
        }

        //==============================================================================================================
        // A right-handed view looking from `eye` to `target`.
        Rows lookAt(Vector3 eye, Vector3 target, Vector3 up)
        {
            const Vector3 forward = normalize(subtract(target, eye));
            const Vector3 side = normalize(cross(forward, up));
            const Vector3 above = cross(side, forward);
            return { { { side.x, side.y, side.z, -dot(side, eye) },
                       { above.x, above.y, above.z, -dot(above, eye) },
                       { -forward.x, -forward.y, -forward.z, dot(forward, eye) },
                       { 0.0f, 0.0f, 0.0f, 1.0f } } };
        }

        //==============================================================================================================
        // Reverse-Z perspective: view-space depth -near maps to 1 and -far to 0, with clip w = -z.
        Rows perspective(float verticalFov, float aspect, float near, float far)
        {
            const float fy = 1.0f / std::tan(verticalFov * 0.5f);
            const float fx = fy / aspect;
            const float a = near / (far - near);
            const float b = near * far / (far - near);
            return { { { fx, 0.0f, 0.0f, 0.0f }, { 0.0f, fy, 0.0f, 0.0f }, { 0.0f, 0.0f, a, b }, { 0.0f, 0.0f, -1.0f, 0.0f } } };
        }

        //==============================================================================================================
        // Reverse-Z orthographic, centred on the view axis: view-space depth -near maps to 1 and -far to 0.
        Rows orthographic(float halfExtent, float near, float far)
        {
            const float scale = 1.0f / halfExtent;
            return { { { scale, 0.0f, 0.0f, 0.0f },
                       { 0.0f, scale, 0.0f, 0.0f },
                       { 0.0f, 0.0f, 1.0f / (far - near), far / (far - near) },
                       { 0.0f, 0.0f, 0.0f, 1.0f } } };
        }

        // The camera.
        constexpr Vector3 kEye{ 0.0f, 90.0f, 160.0f };
        constexpr Vector3 kTarget{ 0.0f, 0.0f, 0.0f };
        constexpr Vector3 kUp{ 0.0f, 1.0f, 0.0f };
        constexpr float kVerticalFov = std::numbers::pi_v<float> / 3.0f;
        constexpr float kNear = 0.5f;
        constexpr float kFar = 500.0f;

        // The cascades: the light looks at the origin from kLightDistance away, and each cascade covers a square of this
        // half extent.
        constexpr Vector3 kLightDirection{ -0.4f, -1.0f, -0.3f };
        constexpr float kLightDistance = 300.0f;
        constexpr std::array<float, kCascadeCount> kCascadeHalfExtent{ 40.0f, 100.0f, 200.0f };
        constexpr float kCascadeNear = 1.0f;
        constexpr float kCascadeFar = 700.0f;

        // A face of the box mesh: its outward normal and the two in-plane axes, with side x above = normal.
        struct Face
        {
            Vector3 normal;
            Vector3 side;
            Vector3 above;
        };

        constexpr std::array<Face, 6> kFaces{ {
            { { 1, 0, 0 }, { 0, 0, -1 }, { 0, 1, 0 } },
            { { -1, 0, 0 }, { 0, 0, 1 }, { 0, 1, 0 } },
            { { 0, 1, 0 }, { 1, 0, 0 }, { 0, 0, -1 } },
            { { 0, -1, 0 }, { 1, 0, 0 }, { 0, 0, 1 } },
            { { 0, 0, 1 }, { 1, 0, 0 }, { 0, 1, 0 } },
            { { 0, 0, -1 }, { -1, 0, 0 }, { 0, 1, 0 } },
        } };
    } // namespace

    //==================================================================================================================
    Transform instanceTransform(uint32_t box)
    {
        const float x = (static_cast<float>(box % 100) - 49.5f) * 3.0f;
        const float z = (static_cast<float>(box / 100) - 49.5f) * 3.0f;
        const float scale = 0.6f + 0.2f * static_cast<float>(box % 4);
        const float angle = 0.37f * static_cast<float>(box % 17);
        const float c = std::cos(angle) * scale;
        const float s = std::sin(angle) * scale;
        return Transform{ { c, 0.0f, s, x }, { 0.0f, scale, 0.0f, 0.5f * scale }, { -s, 0.0f, c, z }, { 0.0f, 0.0f, 0.0f, 1.0f } };
    }

    //==================================================================================================================
    std::array<MeshVertex, kMeshVertexCount> meshVertices(uint32_t mesh)
    {
        const Vector3 half{ 0.5f + 0.02f * static_cast<float>(mesh % 8),
                            0.5f + 0.02f * static_cast<float>(mesh / 8),
                            0.5f + 0.01f * static_cast<float>(mesh % 5) };
        constexpr std::array<std::pair<float, float>, 4> kCorners{ { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } } };
        std::array<MeshVertex, kMeshVertexCount> vertices{};
        for (uint32_t face = 0; face < 6; ++face)
        {
            const Face& f = kFaces[face];
            for (uint32_t corner = 0; corner < 4; ++corner)
            {
                const auto [su, sv] = kCorners[corner];
                const Vector3 p{ (f.normal.x + su * f.side.x + sv * f.above.x) * half.x,
                                 (f.normal.y + su * f.side.y + sv * f.above.y) * half.y,
                                 (f.normal.z + su * f.side.z + sv * f.above.z) * half.z };
                vertices[face * 4 + corner] = MeshVertex{ { p.x, p.y, p.z, 1.0f }, { f.normal.x, f.normal.y, f.normal.z, 0.0f } };
            }
        }
        return vertices;
    }

    //==================================================================================================================
    std::array<uint16_t, kMeshIndexCount> meshIndices(uint32_t /*mesh*/)
    {
        std::array<uint16_t, kMeshIndexCount> indices{};
        constexpr std::array<uint16_t, 6> kQuad{ 0, 1, 2, 0, 2, 3 };
        for (uint32_t face = 0; face < 6; ++face)
        {
            for (uint32_t i = 0; i < 6; ++i)
            {
                indices[face * 6 + i] = static_cast<uint16_t>(face * 4 + kQuad[i]);
            }
        }
        return indices;
    }

    //==================================================================================================================
    std::vector<Transform> allInstances()
    {
        std::vector<Transform> instances;
        instances.reserve(kBoxCount);
        for (uint32_t box = 0; box < kBoxCount; ++box)
        {
            instances.push_back(instanceTransform(box));
        }
        return instances;
    }

    //==================================================================================================================
    std::vector<MeshVertex> allVertices()
    {
        std::vector<MeshVertex> vertices;
        vertices.reserve(kMeshCount * kMeshVertexCount);
        for (uint32_t mesh = 0; mesh < kMeshCount; ++mesh)
        {
            const std::array<MeshVertex, kMeshVertexCount> one = meshVertices(mesh);
            vertices.insert(vertices.end(), one.begin(), one.end());
        }
        return vertices;
    }

    //==================================================================================================================
    std::vector<uint16_t> allIndices()
    {
        std::vector<uint16_t> indices;
        indices.reserve(kMeshCount * kMeshIndexCount);
        for (uint32_t mesh = 0; mesh < kMeshCount; ++mesh)
        {
            const std::array<uint16_t, kMeshIndexCount> one = meshIndices(mesh);
            indices.insert(indices.end(), one.begin(), one.end());
        }
        return indices;
    }

    //==================================================================================================================
    Transform cameraViewProjection()
    {
        const float aspect = static_cast<float>(kFrameWidth) / static_cast<float>(kFrameHeight);
        return fromRows(multiply(perspective(kVerticalFov, aspect, kNear, kFar), lookAt(kEye, kTarget, kUp)));
    }

    //==================================================================================================================
    Transform cameraInverseViewProjection()
    {
        return fromRows(inverse(toRows(cameraViewProjection())));
    }

    //==================================================================================================================
    Transform cascadeViewProjection(uint32_t cascade)
    {
        const Vector3 direction = normalize(kLightDirection);
        const Vector3 eye{ kTarget.x - direction.x * kLightDistance, kTarget.y - direction.y * kLightDistance, kTarget.z - direction.z * kLightDistance };
        return fromRows(multiply(orthographic(kCascadeHalfExtent[cascade], kCascadeNear, kCascadeFar), lookAt(eye, kTarget, kUp)));
    }

    //==================================================================================================================
    Float4 lightDirection()
    {
        const Vector3 direction = normalize(kLightDirection);
        return Float4{ direction.x, direction.y, direction.z, 0.0f };
    }

    //==================================================================================================================
    ForwardBox forwardBox(uint32_t index)
    {
        const uint32_t box = kForwardBoxStride * index;
        const float phase = static_cast<float>(index % 8) / 8.0f;
        return ForwardBox{ .box = box, .transform = instanceTransform(box), .color = Float4{ 0.9f - 0.6f * phase, 0.3f + 0.5f * phase, 0.4f, 0.5f } };
    }

    //==================================================================================================================
    bool isKept(uint32_t box)
    {
        return box % 5 < 3;
    }

    //==================================================================================================================
    uint32_t recordIndex(uint32_t box)
    {
        return 3 * (box / 5) + box % 5;
    }

    //==================================================================================================================
    DrawIndexedIndirectArgs keptRecord(uint32_t box)
    {
        const uint32_t mesh = box % kMeshCount;
        return DrawIndexedIndirectArgs{ .drawId = box,
                                        .indexCount = kMeshIndexCount,
                                        .instanceCount = 1,
                                        .firstIndex = kMeshIndexCount * mesh,
                                        .baseVertex = static_cast<int32_t>(kMeshVertexCount * mesh),
                                        .firstInstance = 0 };
    }

    //==================================================================================================================
    std::vector<DrawIndexedIndirectArgs> cullRecords()
    {
        std::vector<DrawIndexedIndirectArgs> records(kKeptPerView);
        for (uint32_t box = 0; box < kBoxCount; ++box)
        {
            if (isKept(box))
            {
                records[recordIndex(box)] = keptRecord(box);
            }
        }
        return records;
    }

    //==================================================================================================================
    Float4 transformPoint(const Transform& transform, const Float4& point)
    {
        const auto dot4 = [&point](const Float4& row) { return row.x * point.x + row.y * point.y + row.z * point.z + row.w * point.w; };
        return Float4{ dot4(transform.row0), dot4(transform.row1), dot4(transform.row2), dot4(transform.row3) };
    }
} // namespace rojoRHI::bench
