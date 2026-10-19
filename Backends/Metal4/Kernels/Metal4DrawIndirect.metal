//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4DrawIndirect.metal
/// @brief Defines the counted-draw kernels: three encode records into an indirect command buffer, three write one native record.
//----------------------------------------------------------------------------------------------------------------------
#include <metal_stdlib>

namespace rojoRHI {
    /// The parameters of one counted draw's kernel, laid out as `metal4::DrawParams` writes them.
    struct DrawParams
    {
        ulong icb;          ///< The address of the indirect command buffer's resource ID; 0 for the writer kernels.
        ulong records;      ///< The first record.
        ulong count;        ///< The draw count, or 0 for none, which means `maxCount`.
        ulong output;       ///< The execution range, or the native record the writer kernels write.
        ulong indices;      ///< The index buffer of an indexed draw.
        uint maxCount;      ///< The most records drawn.
        uint primitive;     ///< The `MTL::PrimitiveType` value of the draws.
        uint indexType;     ///< The `MTL::IndexType` value of an indexed draw: 0 for 16-bit indices, 1 for 32-bit ones.
        uint reserved[3];   ///< Pads the struct to the size of its C++ counterpart.
    };
    static_assert(sizeof(DrawParams) == 64, "DrawParams must match metal4::DrawParams in Metal4Kernels.h");

    /// The parameters of one counted mesh draw's kernel, laid out as `metal4::MeshParams` writes them.
    struct MeshParams
    {
        ulong icb;               ///< The address of the indirect command buffer's resource ID; 0 for the writer kernel.
        ulong records;           ///< The first record.
        ulong count;             ///< The draw count, or 0 for none, which means `maxCount`.
        ulong output;            ///< The execution range, or the native record the writer kernel writes.
        ulong root;              ///< What each command binds at `buffer(0)`: the root.
        ulong views;             ///< What each command binds at `buffer(1)`: the view table.
        ulong samplers;          ///< What each command binds at `buffer(2)`: the sampler table.
        uint maxCount;           ///< The most records drawn.
        uint objectThreads[3];   ///< The pipeline's task threadgroup size; zero without a task entry.
        uint meshThreads[3];     ///< The pipeline's mesh threadgroup size.
        uint reserved[3];        ///< Pads the struct to the size of its C++ counterpart.
    };
    static_assert(sizeof(MeshParams) == 96, "MeshParams must match metal4::MeshParams in Metal4Kernels.h");

    /// Chapter 08's `DrawMeshTasksIndirectArgs`.
    struct MeshArgs
    {
        uint drawId; ///< Read by the task and mesh entries from offset 0 of the record bound at `buffer(3)`.
        uint x;      ///< Threadgroups of the first stage along x.
        uint y;      ///< Along y.
        uint z;      ///< Along z.
    };

    /// `MTL::DispatchThreadgroupsIndirectArguments`.
    struct NativeMesh
    {
        uint x; ///< Threadgroups along x.
        uint y; ///< Along y.
        uint z; ///< Along z.
    };

    /// Chapter 08's `DrawIndirectArgs`.
    struct DrawArgs
    {
        uint drawId;        ///< Becomes the draw's base instance.
        uint vertexCount;   ///< Vertices per instance.
        uint instanceCount; ///< Instances.
        uint firstVertex;   ///< The first vertex.
        uint firstInstance; ///< Not visible to shaders, so not passed.
    };

    /// Chapter 08's `DrawIndexedIndirectArgs`.
    struct IndexedArgs
    {
        uint drawId;        ///< Becomes the draw's base instance.
        uint indexCount;    ///< Indices per instance.
        uint instanceCount; ///< Instances.
        uint firstIndex;    ///< The first index read.
        int baseVertex;     ///< Added to each index.
        uint firstInstance; ///< Not visible to shaders, so not passed.
    };

    /// `MTL::DrawPrimitivesIndirectArguments`.
    struct NativeDraw
    {
        uint vertexCount;   ///< Vertices per instance.
        uint instanceCount; ///< Instances.
        uint vertexStart;   ///< The first vertex.
        uint baseInstance;  ///< The base instance: the record's drawId.
    };

    /// `MTL::DrawIndexedPrimitivesIndirectArguments`.
    struct NativeIndexed
    {
        uint indexCount;    ///< Indices per instance.
        uint instanceCount; ///< Instances.
        uint indexStart;    ///< The first index read.
        int baseVertex;     ///< Added to each index.
        uint baseInstance;  ///< The base instance: the record's drawId.
    };

    /// What the indirect command buffer's resource ID is read as.
    struct Icb
    {
        metal::command_buffer commands; ///< The indirect command buffer.
    };

    //==================================================================================================================
    /// The draws a counted draw issues: its count, at most `maxCount`, or `maxCount` when it has none.
    template<typename Params> static uint drawCount(constant Params& p)
    {
        return p.count != 0 ? metal::min(*reinterpret_cast<const device uint*>(p.count), p.maxCount) : p.maxCount;
    }

    //==================================================================================================================
    /// The primitive type `value`, an `MTL::PrimitiveType`, names.
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

    //==================================================================================================================
    /// Returns the draws of the kernel's counted draw, and writes them as the execution range when `index` is 0.
    template<typename Params> static uint writeRange(constant Params& p, uint index)
    {
        const uint count = drawCount(p);
        if (index == 0)
        {
            *reinterpret_cast<device uint2*>(p.output) = uint2(0, count);
        }
        return count;
    }
} // namespace rojoRHI

//======================================================================================================================
/// Encodes record `index` of a counted draw as command `index` of the indirect command buffer, its drawId as the base
/// instance, one thread per record; thread 0 writes the execution range. A record with no vertices or instances becomes
/// an empty command, and threads at or past the count encode nothing, since the range ends before them.
kernel void rojorhi_encode_draw(constant rojoRHI::DrawParams& p [[buffer(0)]], uint index [[thread_position_in_grid]])
{
    if (index >= rojoRHI::writeRange(p, index))
    {
        return;
    }
    const rojoRHI::DrawArgs args = reinterpret_cast<const device rojoRHI::DrawArgs*>(p.records)[index];
    metal::render_command command(reinterpret_cast<device rojoRHI::Icb*>(p.icb)->commands, index);
    if (args.vertexCount == 0 || args.instanceCount == 0)
    {
        command.reset();
        return;
    }
    command.draw_primitives(rojoRHI::primitiveOf(p.primitive), args.firstVertex, args.vertexCount, args.instanceCount, args.drawId);
}

//======================================================================================================================
/// Encodes record `index` of a counted indexed draw as `rojorhi_encode_draw` does, reading indices of the run-time index
/// type from `firstIndex`. Metal takes the base vertex unsigned; the GPU adds it to each index modulo 2^32, which is the
/// signed sum.
kernel void rojorhi_encode_indexed(constant rojoRHI::DrawParams& p [[buffer(0)]], uint index [[thread_position_in_grid]])
{
    if (index >= rojoRHI::writeRange(p, index))
    {
        return;
    }
    const rojoRHI::IndexedArgs args = reinterpret_cast<const device rojoRHI::IndexedArgs*>(p.records)[index];
    metal::render_command command(reinterpret_cast<device rojoRHI::Icb*>(p.icb)->commands, index);
    if (args.indexCount == 0 || args.instanceCount == 0)
    {
        command.reset();
        return;
    }
    const metal::primitive_type primitive = rojoRHI::primitiveOf(p.primitive);
    const uint baseVertex = static_cast<uint>(args.baseVertex);
    if (p.indexType == 0)
    {
        const device ushort* indices = reinterpret_cast<const device ushort*>(p.indices) + args.firstIndex;
        command.draw_indexed_primitives(primitive, args.indexCount, indices, args.instanceCount, baseVertex, args.drawId);
    }
    else
    {
        const device uint* indices = reinterpret_cast<const device uint*>(p.indices) + args.firstIndex;
        command.draw_indexed_primitives(primitive, args.indexCount, indices, args.instanceCount, baseVertex, args.drawId);
    }
}

//======================================================================================================================
/// Writes the one record of a counted draw of `maxCount` 1 as a native record, its drawId as the base instance and no
/// instances when the count is 0. One thread.
kernel void rojorhi_write_draw(constant rojoRHI::DrawParams& p [[buffer(0)]])
{
    const rojoRHI::DrawArgs args = *reinterpret_cast<const device rojoRHI::DrawArgs*>(p.records);
    *reinterpret_cast<device rojoRHI::NativeDraw*>(p.output) = rojoRHI::NativeDraw{
        args.vertexCount, rojoRHI::drawCount(p) != 0 ? args.instanceCount : 0u, args.firstVertex, args.drawId };
}

//======================================================================================================================
/// Writes the one record of a counted indexed draw of `maxCount` 1 as a native record, as `rojorhi_write_draw` does.
kernel void rojorhi_write_indexed(constant rojoRHI::DrawParams& p [[buffer(0)]])
{
    const rojoRHI::IndexedArgs args = *reinterpret_cast<const device rojoRHI::IndexedArgs*>(p.records);
    *reinterpret_cast<device rojoRHI::NativeIndexed*>(p.output) = rojoRHI::NativeIndexed{
        args.indexCount, rojoRHI::drawCount(p) != 0 ? args.instanceCount : 0u, args.firstIndex, args.baseVertex, args.drawId };
}

//======================================================================================================================
/// Encodes mesh record `index` of a counted mesh draw as command `index` of a mesh indirect command buffer, one thread per
/// record; thread 0 writes the execution range. The command inherits no buffer, so it binds the root, the view and sampler
/// tables and its own record at `buffer(0)` to `buffer(3)` on the object, mesh and fragment stages, where an entry of the
/// pipeline may read them. A record with a zero group count becomes an empty command.
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

//======================================================================================================================
/// Writes the one record of a counted mesh draw of `maxCount` 1 as a native record, with no groups when the count is 0. The
/// draw binds the record itself at `buffer(3)`. One thread.
kernel void rojorhi_write_mesh(constant rojoRHI::MeshParams& p [[buffer(0)]])
{
    const rojoRHI::MeshArgs args = *reinterpret_cast<const device rojoRHI::MeshArgs*>(p.records);
    const bool drawn = rojoRHI::drawCount(p) != 0;
    *reinterpret_cast<device rojoRHI::NativeMesh*>(p.output) =
        rojoRHI::NativeMesh{ drawn ? args.x : 0u, drawn ? args.y : 0u, drawn ? args.z : 0u };
}
