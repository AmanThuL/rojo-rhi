// Spike 3 shaders: the two backend kernels and one point-per-draw pipeline. Throwaway spike
// source; see docs/research/2026-10-12-spike-3-multi-draw-indirect.md.
#include <metal_stdlib>
using namespace metal;

constant uint kTargetSize = 256;

// RojoRHI records (chapter 08): a leading drawId, then the native fields.
struct DrawArgs {
    uint drawId, vertexCount, instanceCount, firstVertex, firstInstance;
};
struct IndexedArgs {
    uint drawId, indexCount, instanceCount, firstIndex;
    int baseVertex;
    uint firstInstance;
};
struct MeshArgs {
    uint drawId, x, y, z;
};

// Native Metal indirect records.
struct NativeDraw {
    uint vertexCount, instanceCount, vertexStart, baseInstance;
};
struct NativeIndexed {
    uint indexCount, instanceCount, indexStart;
    int baseVertex;
    uint baseInstance;
};
struct NativeMesh {
    uint x, y, z;
};

struct Icb {
    command_buffer cmds;
};

struct Root {
    ulong records;          // RojoRHI records
    constant uint* count;   // null means maxCount
    ulong out;              // E: Icb, L: native records
    device uint2* range;    // E: execution range {location, length}
    constant uint* indices; // indexed variant
    uint maxCount;
};

static uint drawCount(constant Root& r) {
    return r.count ? min(*r.count, r.maxCount) : r.maxCount;
}

// Arm E: one thread per record encodes records below the count; thread 0 writes the range.
kernel void encodeDraw(constant Root& r [[buffer(0)]], uint i [[thread_position_in_grid]]) {
    uint n = drawCount(r);
    if (i == 0)
        *r.range = uint2(0, n);
    if (i >= n)
        return;
    DrawArgs a = reinterpret_cast<constant DrawArgs*>(r.records)[i];
    render_command cmd(reinterpret_cast<device Icb*>(r.out)->cmds, i);
    cmd.draw_primitives(primitive_type::point, a.firstVertex, a.vertexCount, a.instanceCount,
                        a.drawId);
}

kernel void encodeIndexed(constant Root& r [[buffer(0)]], uint i [[thread_position_in_grid]]) {
    uint n = drawCount(r);
    if (i == 0)
        *r.range = uint2(0, n);
    if (i >= n)
        return;
    IndexedArgs a = reinterpret_cast<constant IndexedArgs*>(r.records)[i];
    render_command cmd(reinterpret_cast<device Icb*>(r.out)->cmds, i);
    cmd.draw_indexed_primitives(primitive_type::point, a.indexCount, r.indices + a.firstIndex,
                                a.instanceCount, a.baseVertex, a.drawId);
}

kernel void encodeMesh(constant Root& r [[buffer(0)]], uint i [[thread_position_in_grid]]) {
    uint n = drawCount(r);
    if (i == 0)
        *r.range = uint2(0, n);
    if (i >= n)
        return;
    constant MeshArgs* a = reinterpret_cast<constant MeshArgs*>(r.records) + i;
    render_command cmd(reinterpret_cast<device Icb*>(r.out)->cmds, i);
    cmd.set_object_buffer(a, 0);
    cmd.draw_mesh_threadgroups(uint3(a->x, a->y, a->z), uint3(1), uint3(1));
}

// Arm L: one thread per record writes a native record; records at or past the count draw nothing.
kernel void copyDraw(constant Root& r [[buffer(0)]], uint i [[thread_position_in_grid]]) {
    if (i >= r.maxCount)
        return;
    DrawArgs a = reinterpret_cast<constant DrawArgs*>(r.records)[i];
    reinterpret_cast<device NativeDraw*>(r.out)[i] = {
        a.vertexCount, i < drawCount(r) ? a.instanceCount : 0u, a.firstVertex, a.drawId};
}

kernel void copyIndexed(constant Root& r [[buffer(0)]], uint i [[thread_position_in_grid]]) {
    if (i >= r.maxCount)
        return;
    IndexedArgs a = reinterpret_cast<constant IndexedArgs*>(r.records)[i];
    reinterpret_cast<device NativeIndexed*>(r.out)[i] = {
        a.indexCount, i < drawCount(r) ? a.instanceCount : 0u, a.firstIndex, a.baseVertex,
        a.drawId};
}

kernel void copyMesh(constant Root& r [[buffer(0)]], uint i [[thread_position_in_grid]]) {
    if (i >= r.maxCount)
        return;
    MeshArgs a = reinterpret_cast<constant MeshArgs*>(r.records)[i];
    reinterpret_cast<device NativeMesh*>(r.out)[i] = {i < drawCount(r) ? a.x : 0u, a.y, a.z};
}

// One point at pixel drawId with value drawId + 1. A value of ~0 marks an instance index that is
// not zero-based after subtracting the base instance.
struct Point {
    float4 position [[position]];
    float size [[point_size]];
    uint value [[flat]];
};

static Point pointFor(uint drawId, bool zeroBased) {
    float2 pixel = float2(drawId % kTargetSize, drawId / kTargetSize) + 0.5f;
    float2 ndc = pixel / float(kTargetSize) * 2.0f - 1.0f;
    return {float4(ndc.x, -ndc.y, 0.0f, 1.0f), 1.0f, zeroBased ? drawId + 1 : ~0u};
}

vertex Point drawVertex(uint instance [[instance_id]], uint base [[base_instance]]) {
    return pointFor(base, instance - base == 0);
}

struct Color {
    uint value [[color(0)]];
};

fragment Color drawFragment(Point p [[stage_in]]) {
    return {p.value};
}

struct Payload {
    uint drawId;
};

[[object]] void meshObject(constant MeshArgs& record [[buffer(0)]],
                           object_data Payload& payload [[payload]],
                           mesh_grid_properties grid) {
    payload.drawId = record.drawId;
    grid.set_threadgroups_per_grid(uint3(1));
}

using PointMesh = metal::mesh<Point, void, 1, 1, topology::point>;

[[mesh]] void meshMain(const object_data Payload& payload [[payload]], PointMesh out) {
    out.set_primitive_count(1);
    out.set_vertex(0, pointFor(payload.drawId, true));
    out.set_index(0, 0);
}
