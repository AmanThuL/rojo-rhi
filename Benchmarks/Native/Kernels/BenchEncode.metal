//----------------------------------------------------------------------------------------------------------------------
/// @file BenchEncode.metal
/// @brief Benchmark kernels: encode the draws of a counted indirect draw into an indirect command buffer.
//----------------------------------------------------------------------------------------------------------------------
#include <metal_stdlib>
using namespace metal;

// The records the RojoRHI interface defines: a leading drawId, then the native fields.
struct DrawArgs
{
    uint drawId, vertexCount, instanceCount, firstVertex, firstInstance;
};

struct IndexedArgs
{
    uint drawId, indexCount, instanceCount, firstIndex;
    int baseVertex;
    uint firstInstance;
};

struct Icb
{
    command_buffer commands;
};

// One counted draw's inputs. Mirrors NativeIndirect.cpp: count is null when the draw takes exactly maxCount.
struct Root
{
    ulong records;
    constant uint* count;
    ulong icb; // The address of a resource ID of the indirect command buffer.
    device uint2* range;
    constant ushort* indices;
    uint maxCount;
};

static uint drawCount(constant Root& root)
{
    return root.count ? min(*root.count, root.maxCount) : root.maxCount;
}

// One thread per record encodes the records below the count; thread 0 writes the execution range. The record's
// drawId is the draw's base instance, which the vertex shader reads back; its firstInstance never reaches a shader.
kernel void encodeDraw(constant Root& root [[buffer(0)]], uint index [[thread_position_in_grid]])
{
    const uint count = drawCount(root);
    if (index == 0)
    {
        *root.range = uint2(0, count);
    }
    if (index >= count)
    {
        return;
    }
    const DrawArgs args = reinterpret_cast<constant DrawArgs*>(root.records)[index];
    render_command command(reinterpret_cast<device Icb*>(root.icb)->commands, index);
    command.draw_primitives(primitive_type::point, args.firstVertex, args.vertexCount, args.instanceCount, args.drawId);
}

static void encodeIndexedRecord(constant Root& root, uint index, primitive_type primitive)
{
    const uint count = drawCount(root);
    if (index == 0)
    {
        *root.range = uint2(0, count);
    }
    if (index >= count)
    {
        return;
    }
    const IndexedArgs args = reinterpret_cast<constant IndexedArgs*>(root.records)[index];
    render_command command(reinterpret_cast<device Icb*>(root.icb)->commands, index);
    command.draw_indexed_primitives(primitive, args.indexCount, root.indices + args.firstIndex, args.instanceCount, args.baseVertex, args.drawId);
}

kernel void encodeIndexed(constant Root& root [[buffer(0)]], uint index [[thread_position_in_grid]])
{
    encodeIndexedRecord(root, index, primitive_type::point);
}

// The same records drawn as triangle lists, for meshes.
kernel void encodeIndexedTriangles(constant Root& root [[buffer(0)]], uint index [[thread_position_in_grid]])
{
    encodeIndexedRecord(root, index, primitive_type::triangle);
}
