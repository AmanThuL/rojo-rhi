//----------------------------------------------------------------------------------------------------------------------
/// @file RojoRHI.slang
/// @brief The RojoRHI shader module: the reserved bindings and the entries every shader imports.
//----------------------------------------------------------------------------------------------------------------------

// Lowerings, as emitted by the pinned Slang (v2026.14.1):
//
//   root<T>()      Metal   a raw `device` buffer at `[[buffer(0)]]`, read at offset 0; the push-constant
//                          declaration exists only under the SPIR-V target macro, so Metal takes no index for it
//                  SPIR-V  `PushConstant` block holding the root `Addr`, converted with `OpConvertUToPtr` to a
//                          `PhysicalStorageBuffer` pointer to T
//                  HLSL    `ByteAddressBuffer gRootBytes : register(t0)`, bound as a root SRV
//   load / store   Metal   `device` pointer from (hi << 32) | lo, plus `index * sizeof(T)`
//                  SPIR-V  the same address as a `PhysicalStorageBuffer` pointer
//                  HLSL    `ByteAddressBuffer` from `ResourceDescriptorHeap[base.x]` for load and
//                          `RWByteAddressBuffer` from `ResourceDescriptorHeap[base.x + 1]` for store, at byte
//                          offset `base.y + index * sizeof(T)`: the D3D12 `Addr` is (heap index, byte offset)
//   nonUniform     Metal   returns its argument
//                  HLSL    `NonUniformResourceIndex` on the heap index
//                  SPIR-V  `NonUniformResourceIndex`, which Slang decorates `NonUniform` only where the index
//                          reaches a descriptor; an index used for a pointer carries no decoration, and the
//                          marker on the index does not survive `DescriptorHandle`, so every SPIR-V texture,
//                          storage-texture and sampler fetch wraps its handle in `nonuniform(...)` instead
//                          (`OpCapability ShaderNonUniform`, correct for a uniform index too)
//
//   (D3D12 note)   the emitted HLSL has one `NonUniformResourceIndex(...)`, in the return of the helper behind
//                  `nonUniform(uint)`; every heap indexing site (`load`, `store` at `base.x + 1`, texture and
//                  sampler) is a plain `ResourceDescriptorHeap[...]` or `SamplerDescriptorHeap[...]`. Whether DXC
//                  keeps the non-uniform qualifier through that function return, the `uint2` pack and unpack
//                  and the `+ 1` is to be verified with the D3D12 backend, by inspecting the DXIL handle flags
//                  for each fetch kind
//   texture*,      Metal   `((device T*)gViewIds)[view]`, T the MSL texture type, from the ID table at
//   rwTexture*             `[[buffer(1)]]`; written as `__intrinsic_asm` because Slang's `Load<Texture>` emits an
//                          `as_type` from `ulong` that the Metal compiler rejects
//                  HLSL    `ResourceDescriptorHeap[view]`
//                  SPIR-V  the runtime descriptor arrays Slang names `__slang_resource_heap`
//                          (`DescriptorHandle`); `-capability spvDescriptorHeapEXT` is not used. Layout Slang
//                          picked: samplers at set 1 binding 0; sampled and storage images at set 1 binding 2
//   sampler,       Metal   the same read from the sampler ID table at `[[buffer(2)]]`
//   comparisonSampler HLSL `SamplerDescriptorHeap[index]`; SPIR-V as for textures
//   VertexIds      the vertex entry point's parameter, its fields internal to the module (a consumer's entry
//                  point still compiles taking it). Metal: `SV_VertexID`, `SV_InstanceID` and
//                  `SV_StartInstanceLocation`, emitted as `[[vertex_id]]`, `[[instance_id]]` and
//                  `[[base_instance]]` entry parameters. SPIR-V: `SV_VulkanVertexID`, `SV_VulkanInstanceID`
//                  and `SV_StartInstanceLocation`, the raw `VertexIndex`, `InstanceIndex` and `BaseInstance`
//                  (Slang's plain `SV_VertexID` and `SV_InstanceID` already subtract the bases there). HLSL:
//                  `SV_VertexID` and `SV_InstanceID` only, since `SV_StartInstanceLocation` needs Shader Model
//                  6.8, above the 6.6 floor. A module-level `in` global for the base
//                  instance was rejected: with a user-semantic vertex output Slang moved it into `[[stage_in]]`
//   vertexIndex    every target returns `vertexId`
//   instanceIndex  Metal, SPIR-V   `instanceId - baseInstance`; HLSL   `instanceId`, which excludes the base
//   drawId(ids)    Metal   the base instance; HLSL   the root constant `b1`; SPIR-V   as for the record below
//   drawId()       Metal task and mesh   offset 0 of the record at `[[buffer(3)]]`
//                  HLSL    the root constant `b1`
//                  SPIR-V  offset 0 of record `DrawIndex`, from the record base and stride in the push data;
//                          the push data is {root Addr, record Addr, record stride}
//   multisample    `texture2DMS` and `texture2DMSArray` compile everywhere, but the pinned Slang lowers their
//                  Metal `Load` and `GetDimensions` to MSL the Metal compiler rejects, so Metal cannot read them
//   target macro   the build passes exactly one of ROJORHI_TARGET_METAL, ROJORHI_TARGET_SPIRV and
//                  ROJORHI_TARGET_HLSL (HLSL also for DXIL); the `#error`s above fail a compile with none or
//                  several. Each target's declarations are under its own `#if`, so no target sees another's
//                  bindings, and the function bodies that touch them use the macro too
//
// The HLSL and DXIL invocations compile with `-profile sm_6_6` for `ResourceDescriptorHeap`.

#if !defined(ROJORHI_TARGET_METAL) && !defined(ROJORHI_TARGET_SPIRV) && !defined(ROJORHI_TARGET_HLSL)
#error "RojoRHI: compile with exactly one of -D ROJORHI_TARGET_METAL, -D ROJORHI_TARGET_SPIRV, -D ROJORHI_TARGET_HLSL"
#endif
#if (defined(ROJORHI_TARGET_METAL) && defined(ROJORHI_TARGET_SPIRV)) || \
    (defined(ROJORHI_TARGET_METAL) && defined(ROJORHI_TARGET_HLSL)) || \
    (defined(ROJORHI_TARGET_SPIRV) && defined(ROJORHI_TARGET_HLSL))
#error "RojoRHI: more than one of ROJORHI_TARGET_METAL, ROJORHI_TARGET_SPIRV, ROJORHI_TARGET_HLSL is defined"
#endif

module RojoRHI;

namespace rojoRHI
{
    /// A 64-bit device address, the same two-word pair the C++ `Addr` carries.
    public typealias Addr = uint2;

    // The SPIR-V push data: the root, the draw records' base, and the byte stride between records.
    struct PushData
    {
        Addr root;
        Addr record;
        uint recordStride;
    };

    // The D3D12 root constant at b1: the draw's record, whose leading uint is the draw identifier.
    struct DrawRecord
    {
        uint drawId;
    };

#if defined(ROJORHI_TARGET_SPIRV)
    [[vk::push_constant]] ConstantBuffer<PushData> gPush;

    // The draw index, an entry-point input of the task, mesh and vertex shaders that read a record.
    in uint gDrawIndex : SV_DrawIndex;
#elif defined(ROJORHI_TARGET_METAL)
    // The binding attributes are not read by the Metal lowering; they stop Slang warning E39029 for a register.
    [[vk::binding(0, 0)]] ByteAddressBuffer gRootBytes : register(t0);
    [[vk::binding(1, 0)]] ByteAddressBuffer gViewIds : register(t1);
    [[vk::binding(2, 0)]] ByteAddressBuffer gSamplerIds : register(t2);
    [[vk::binding(3, 0)]] ByteAddressBuffer gDrawRecordBytes : register(t3);
#else
    ByteAddressBuffer gRootBytes : register(t0);
    ConstantBuffer<DrawRecord> gDrawRecord : register(b1);
#endif

    /// The vertex-stage system values the built-ins read; a vertex entry point takes it as a parameter.
    public struct VertexIds
    {
#if defined(ROJORHI_TARGET_SPIRV)
        // The raw Vulkan indices: Slang's SV_VertexID and SV_InstanceID on SPIR-V subtract the base.
        uint vertexId : SV_VulkanVertexID;
        uint instanceId : SV_VulkanInstanceID;
        uint baseInstance : SV_StartInstanceLocation;
#elif defined(ROJORHI_TARGET_METAL)
        uint vertexId : SV_VertexID;
        uint instanceId : SV_InstanceID;
        uint baseInstance : SV_StartInstanceLocation;
#else
        // No base instance on D3D12: SV_StartInstanceLocation needs Shader Model 6.8, above the 6.6 floor.
        uint vertexId : SV_VertexID;
        uint instanceId : SV_InstanceID;
#endif
    };

    // The pointer `index` elements of T past `base`, the address read as (hi << 32) | lo.
    T* pointerAt<T>(Addr base, uint index)
    {
        uint64_t address = (uint64_t(base.y) << 32) | uint64_t(base.x);
        return (T*)(address + uint64_t(index) * uint64_t(sizeof(T)));
    }

    public T root<T>()
    {
#if defined(ROJORHI_TARGET_SPIRV)
        return *pointerAt<T>(gPush.root, 0);
#else
        return gRootBytes.Load<T>(0);
#endif
    }

    public T load<T>(Addr base, uint index)
    {
#if defined(ROJORHI_TARGET_HLSL)
        ByteAddressBuffer heap = ResourceDescriptorHeap[base.x];
        return heap.Load<T>(base.y + index * uint(sizeof(T)));
#else
        return *pointerAt<T>(base, index);
#endif
    }

    public void store<T>(Addr base, uint index, T value)
    {
#if defined(ROJORHI_TARGET_HLSL)
        RWByteAddressBuffer heap = ResourceDescriptorHeap[base.x + 1];
        heap.Store<T>(base.y + index * uint(sizeof(T)), value);
#else
        *pointerAt<T>(base, index) = value;
#endif
    }

    public uint nonUniform(uint index)
    {
#if defined(ROJORHI_TARGET_METAL)
        return index;
#else
        return NonUniformResourceIndex(index);
#endif
    }

    public Addr nonUniform(Addr base)
    {
        return Addr(nonUniform(base.x), base.y);
    }

#if defined(ROJORHI_TARGET_METAL)
    // Entry `index` of an 8-byte resource ID table, as the MSL type V. Slang's own `Load<V>` of a texture
    // emits an `as_type` from `ulong`, which the Metal compiler rejects; a `device V*` indexed by the entry
    // is the form MSL defines for argument-buffer textures and samplers.
    V metalTableAt<V>(ByteAddressBuffer table, uint index)
    {
        __intrinsic_asm "(((device $TR*)($0))[$1])";
    }
#endif

    // The descriptor at `slot` of the global view or sampler heap, as the opaque type V. On SPIR-V the handle
    // is always marked non-uniform, which is correct for a uniform index too and which the marker on the index
    // alone does not carry through `DescriptorHandle`.
    V heapAt<V : IOpaqueDescriptor>(uint slot)
    {
#if defined(ROJORHI_TARGET_SPIRV)
        return getDescriptorFromHandle(nonuniform(DescriptorHandle<V>(uint2(slot, 0))));
#else
        return getDescriptorFromHandle(DescriptorHandle<V>(uint2(slot, 0)));
#endif
    }

    // The descriptor at `view` of the global view heap, as the texture type V.
    V viewAt<V : IOpaqueDescriptor>(uint view)
    {
#if defined(ROJORHI_TARGET_METAL)
        return metalTableAt<V>(gViewIds, view);
#else
        return heapAt<V>(view);
#endif
    }

    public Texture1D<T> texture1D<T : ITexelElement = float4>(uint view)
    {
        return viewAt<Texture1D<T>>(view);
    }

    public Texture1DArray<T> texture1DArray<T : ITexelElement = float4>(uint view)
    {
        return viewAt<Texture1DArray<T>>(view);
    }

    public Texture2D<T> texture2D<T : ITexelElement = float4>(uint view)
    {
        return viewAt<Texture2D<T>>(view);
    }

    public Texture2DArray<T> texture2DArray<T : ITexelElement = float4>(uint view)
    {
        return viewAt<Texture2DArray<T>>(view);
    }

    public Texture2DMS<T> texture2DMS<T : ITexelElement = float4>(uint view)
    {
        return viewAt<Texture2DMS<T>>(view);
    }

    public Texture2DMSArray<T> texture2DMSArray<T : ITexelElement = float4>(uint view)
    {
        return viewAt<Texture2DMSArray<T>>(view);
    }

    public Texture3D<T> texture3D<T : ITexelElement = float4>(uint view)
    {
        return viewAt<Texture3D<T>>(view);
    }

    public TextureCube<T> textureCube<T : ITexelElement = float4>(uint view)
    {
        return viewAt<TextureCube<T>>(view);
    }

    public TextureCubeArray<T> textureCubeArray<T : ITexelElement = float4>(uint view)
    {
        return viewAt<TextureCubeArray<T>>(view);
    }

    public RWTexture1D<T> rwTexture1D<T : ITexelElement = float4>(uint view)
    {
        return viewAt<RWTexture1D<T>>(view);
    }

    public RWTexture1DArray<T> rwTexture1DArray<T : ITexelElement = float4>(uint view)
    {
        return viewAt<RWTexture1DArray<T>>(view);
    }

    public RWTexture2D<T> rwTexture2D<T : ITexelElement = float4>(uint view)
    {
        return viewAt<RWTexture2D<T>>(view);
    }

    public RWTexture2DArray<T> rwTexture2DArray<T : ITexelElement = float4>(uint view)
    {
        return viewAt<RWTexture2DArray<T>>(view);
    }

    public RWTexture3D<T> rwTexture3D<T : ITexelElement = float4>(uint view)
    {
        return viewAt<RWTexture3D<T>>(view);
    }

    public SamplerState sampler(uint index)
    {
#if defined(ROJORHI_TARGET_METAL)
        return metalTableAt<SamplerState>(gSamplerIds, index);
#else
        return heapAt<SamplerState>(index);
#endif
    }

    public SamplerComparisonState comparisonSampler(uint index)
    {
#if defined(ROJORHI_TARGET_METAL)
        return metalTableAt<SamplerComparisonState>(gSamplerIds, index);
#else
        return heapAt<SamplerComparisonState>(index);
#endif
    }

    public uint vertexIndex(VertexIds ids)
    {
        return ids.vertexId;
    }

    public uint instanceIndex(VertexIds ids)
    {
#if defined(ROJORHI_TARGET_HLSL)
        return ids.instanceId;
#else
        return ids.instanceId - ids.baseInstance;
#endif
    }

    // The leading uint of the draw's record, read where the record is bound rather than carried as a base instance.
    uint recordDrawId()
    {
#if defined(ROJORHI_TARGET_METAL)
        return gDrawRecordBytes.Load<uint>(0);
#elif defined(ROJORHI_TARGET_SPIRV)
        uint64_t record = (uint64_t(gPush.record.y) << 32) | uint64_t(gPush.record.x);
        return *(uint*)(record + uint64_t(gDrawIndex) * uint64_t(gPush.recordStride));
#else
        return gDrawRecord.drawId;
#endif
    }

    /// The draw identifier from a vertex entry point.
    public uint drawId(VertexIds ids)
    {
#if defined(ROJORHI_TARGET_METAL)
        return ids.baseInstance;
#else
        return recordDrawId();
#endif
    }

    /// The draw identifier from a task or mesh entry point.
    public uint drawId()
    {
        return recordDrawId();
    }
}

/// The shader ABI version; changing a reserved binding bumps it.
public static const uint ROJORHI_SHADER_ABI_VERSION = 1;
