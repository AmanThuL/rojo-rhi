//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4DrawIndirect.metal
/// @brief Defines the counted-draw kernels: three encode records into an indirect command buffer, three write native records.
//----------------------------------------------------------------------------------------------------------------------
#include <metal_stdlib>

namespace rojoRHI {
    // Matches the layout `metal4::DrawParams` writes.
    struct DrawParams
    {
        ulong icb; // 0 for the writer kernels.
        ulong records;
        ulong count;  // 0 means `maxCount`.
        ulong output; // The execution range, or the native record the writer kernels write.
        ulong indices;
        ulong root;
        ulong views;
        ulong samplers;
        uint maxCount;
        uint primitive;   // An `MTL::PrimitiveType` value.
        uint indexType;   // An `MTL::IndexType` value: 0 for 16-bit indices, 1 for 32-bit ones.
        uint reserved[5]; // Pads the struct to the size of its C++ counterpart.
    };
    static_assert(sizeof(DrawParams) == 96, "DrawParams must match metal4::DrawParams in Metal4Kernels.h");

    // Matches the layout `metal4::MeshParams` writes.
    struct MeshParams
    {
        ulong icb; // 0 for the writer kernel.
        ulong records;
        ulong count;  // 0 means `maxCount`.
        ulong output; // The execution range, or the native record the writer kernel writes.
        ulong root;
        ulong views;
        ulong samplers;
        uint maxCount;
        uint objectThreads[3]; // Zero without a task entry.
        uint meshThreads[3];
        uint reserved[3]; // Pads the struct to the size of its C++ counterpart.
    };
    static_assert(sizeof(MeshParams) == 96, "MeshParams must match metal4::MeshParams in Metal4Kernels.h");

    // Chapter 08's `DrawMeshTasksIndirectArgs`.
    struct MeshArgs
    {
        uint drawId; // Read by the task and mesh entries from offset 0 of the record bound at `buffer(3)`.
        uint x;
        uint y;
        uint z;
    };

    // `MTL::DispatchThreadgroupsIndirectArguments`.
    struct NativeMesh
    {
        uint x;
        uint y;
        uint z;
    };

    // Chapter 08's `DrawIndirectArgs`.
    struct DrawArgs
    {
        uint drawId; // Read by the vertex entry from offset 0 of the record bound at `buffer(3)`.
        uint vertexCount;
        uint instanceCount;
        uint firstVertex;
        uint firstInstance; // Not visible to shaders, so not passed.
    };

    // Chapter 08's `DrawIndexedIndirectArgs`.
    struct IndexedArgs
    {
        uint drawId; // Read by the vertex entry from offset 0 of the record bound at `buffer(3)`.
        uint indexCount;
        uint instanceCount;
        uint firstIndex;
        int baseVertex;
        uint firstInstance; // Not visible to shaders, so not passed.
    };

    // `MTL::DrawPrimitivesIndirectArguments`.
    struct NativeDraw
    {
        uint vertexCount;
        uint instanceCount;
        uint vertexStart;
        uint baseInstance; // 0, since no shader sees it.
    };

    // `MTL::DrawIndexedPrimitivesIndirectArguments`.
    struct NativeIndexed
    {
        uint indexCount;
        uint instanceCount;
        uint indexStart;
        int baseVertex;
        uint baseInstance; // 0, since no shader sees it.
    };

    struct Icb
    {
        metal::command_buffer commands;
    };

    // `maxCount` when the draw has no count.
    template<typename Params> static uint drawCount(constant Params& p)
    {
        return p.count != 0 ? metal::min(*reinterpret_cast<const device uint*>(p.count), p.maxCount) : p.maxCount;
    }

    // For a writer kernel, which reads its parameters from its run.
    template<typename Params> static uint drawCount(const device Params& p)
    {
        return p.count != 0 ? metal::min(*reinterpret_cast<const device uint*>(p.count), p.maxCount) : p.maxCount;
    }

    // `metal4::kDrawReservation`.
    constant ulong kDrawStride = 128;

    // `metal4::kMeshReservation`.
    constant ulong kMeshStride = 128;

    template<typename Params> static const device Params& runParams(const device uchar* params, uint index, ulong stride)
    {
        return *reinterpret_cast<const device Params*>(params + index * stride);
    }

    static metal::primitive_type primitiveOf(uint value)
    {
        switch (value)
        {
        case 0: return metal::primitive_type::point;
        case 1: return metal::primitive_type::line;
        case 2: return metal::primitive_type::line_strip;
        case 4: return metal::primitive_type::triangle_strip;
        default: return metal::primitive_type::triangle;
        }
    }

    // Thread 0 writes the execution range.
    template<typename Params> static uint writeRange(constant Params& p, uint index)
    {
        const uint count = drawCount(p);
        if (index == 0)
        {
            *reinterpret_cast<device uint2*>(p.output) = uint2(0, count);
        }
        return count;
    }

    // `command` inherits no buffer, so it binds them on every stage where an entry of the pipeline may read them.
    static void bindDrawBuffers(thread metal::render_command& command, constant DrawParams& p, const device void* record)
    {
        const device uchar* root = reinterpret_cast<const device uchar*>(p.root);
        const device uchar* views = reinterpret_cast<const device uchar*>(p.views);
        const device uchar* samplers = reinterpret_cast<const device uchar*>(p.samplers);
        command.set_vertex_buffer(root, 0);
        command.set_vertex_buffer(views, 1);
        command.set_vertex_buffer(samplers, 2);
        command.set_vertex_buffer(record, 3);
        command.set_fragment_buffer(root, 0);
        command.set_fragment_buffer(views, 1);
        command.set_fragment_buffer(samplers, 2);
        command.set_fragment_buffer(record, 3);
    }
} // namespace rojoRHI

// Thread 0 writes the execution range. Each command binds its own record at `buffer(3)` for the vertex entry's drawId.
// Threads at or past the count encode nothing, since the range ends before them.
kernel void rojorhi_encode_draw(constant rojoRHI::DrawParams& p [[buffer(0)]], uint index [[thread_position_in_grid]])
{
    if (index >= rojoRHI::writeRange(p, index))
    {
        return;
    }
    const device rojoRHI::DrawArgs* args = reinterpret_cast<const device rojoRHI::DrawArgs*>(p.records) + index;
    metal::render_command command(reinterpret_cast<device rojoRHI::Icb*>(p.icb)->commands, index);
    if (args->vertexCount == 0 || args->instanceCount == 0)
    {
        command.reset();
        return;
    }
    rojoRHI::bindDrawBuffers(command, p, args);
    command.draw_primitives(rojoRHI::primitiveOf(p.primitive), args->firstVertex, args->vertexCount, args->instanceCount, 0u);
}

// Metal takes the base vertex unsigned; the GPU adds it to each index modulo 2^32, which is the signed sum.
kernel void rojorhi_encode_indexed(constant rojoRHI::DrawParams& p [[buffer(0)]], uint index [[thread_position_in_grid]])
{
    if (index >= rojoRHI::writeRange(p, index))
    {
        return;
    }
    const device rojoRHI::IndexedArgs* record = reinterpret_cast<const device rojoRHI::IndexedArgs*>(p.records) + index;
    const rojoRHI::IndexedArgs args = *record;
    metal::render_command command(reinterpret_cast<device rojoRHI::Icb*>(p.icb)->commands, index);
    if (args.indexCount == 0 || args.instanceCount == 0)
    {
        command.reset();
        return;
    }
    rojoRHI::bindDrawBuffers(command, p, record);
    const metal::primitive_type primitive = rojoRHI::primitiveOf(p.primitive);
    const uint baseVertex = static_cast<uint>(args.baseVertex);
    if (p.indexType == 0)
    {
        const device ushort* indices = reinterpret_cast<const device ushort*>(p.indices) + args.firstIndex;
        command.draw_indexed_primitives(primitive, args.indexCount, indices, args.instanceCount, baseVertex, 0u);
    }
    else
    {
        const device uint* indices = reinterpret_cast<const device uint*>(p.indices) + args.firstIndex;
        command.draw_indexed_primitives(primitive, args.indexCount, indices, args.instanceCount, baseVertex, 0u);
    }
}

// The draw binds the record itself at `buffer(3)`. One thread per draw of a run of consecutive such draws.
kernel void rojorhi_write_draw(const device uchar* params [[buffer(0)]], uint index [[thread_position_in_grid]])
{
    const device rojoRHI::DrawParams& p = rojoRHI::runParams<rojoRHI::DrawParams>(params, index, rojoRHI::kDrawStride);
    const rojoRHI::DrawArgs args = *reinterpret_cast<const device rojoRHI::DrawArgs*>(p.records);
    *reinterpret_cast<device rojoRHI::NativeDraw*>(p.output) = rojoRHI::NativeDraw{
        args.vertexCount, rojoRHI::drawCount(p) != 0 ? args.instanceCount : 0u, args.firstVertex, 0u };
}

kernel void rojorhi_write_indexed(const device uchar* params [[buffer(0)]], uint index [[thread_position_in_grid]])
{
    const device rojoRHI::DrawParams& p = rojoRHI::runParams<rojoRHI::DrawParams>(params, index, rojoRHI::kDrawStride);
    const rojoRHI::IndexedArgs args = *reinterpret_cast<const device rojoRHI::IndexedArgs*>(p.records);
    *reinterpret_cast<device rojoRHI::NativeIndexed*>(p.output) = rojoRHI::NativeIndexed{
        args.indexCount, rojoRHI::drawCount(p) != 0 ? args.instanceCount : 0u, args.firstIndex, args.baseVertex, 0u };
}

// Thread 0 writes the execution range. The command inherits no buffer, so it binds the reserved buffers on the object,
// mesh and fragment stages.
kernel void rojorhi_encode_mesh(constant rojoRHI::MeshParams& p [[buffer(0)]], uint index [[thread_position_in_grid]])
{
    if (index >= rojoRHI::writeRange(p, index))
    {
        return;
    }
    const device rojoRHI::MeshArgs* args = reinterpret_cast<const device rojoRHI::MeshArgs*>(p.records) + index;
    metal::render_command command(reinterpret_cast<device rojoRHI::Icb*>(p.icb)->commands, index);
    if (args->x == 0 || args->y == 0 || args->z == 0)
    {
        command.reset();
        return;
    }
    const device uchar* root = reinterpret_cast<const device uchar*>(p.root);
    const device uchar* views = reinterpret_cast<const device uchar*>(p.views);
    const device uchar* samplers = reinterpret_cast<const device uchar*>(p.samplers);
    command.set_object_buffer(root, 0);
    command.set_object_buffer(views, 1);
    command.set_object_buffer(samplers, 2);
    command.set_object_buffer(args, 3);
    command.set_mesh_buffer(root, 0);
    command.set_mesh_buffer(views, 1);
    command.set_mesh_buffer(samplers, 2);
    command.set_mesh_buffer(args, 3);
    command.set_fragment_buffer(root, 0);
    command.set_fragment_buffer(views, 1);
    command.set_fragment_buffer(samplers, 2);
    command.set_fragment_buffer(args, 3);
    command.draw_mesh_threadgroups(
        uint3(args->x, args->y, args->z),
        uint3(p.objectThreads[0], p.objectThreads[1], p.objectThreads[2]),
        uint3(p.meshThreads[0], p.meshThreads[1], p.meshThreads[2]));
}

// The draw binds the record itself at `buffer(3)`.
kernel void rojorhi_write_mesh(const device uchar* params [[buffer(0)]], uint index [[thread_position_in_grid]])
{
    const device rojoRHI::MeshParams& p = rojoRHI::runParams<rojoRHI::MeshParams>(params, index, rojoRHI::kMeshStride);
    const rojoRHI::MeshArgs args = *reinterpret_cast<const device rojoRHI::MeshArgs*>(p.records);
    const bool drawn = rojoRHI::drawCount(p) != 0;
    *reinterpret_cast<device rojoRHI::NativeMesh*>(p.output) =
        rojoRHI::NativeMesh{ drawn ? args.x : 0u, drawn ? args.y : 0u, drawn ? args.z : 0u };
}
