//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Clear.metal
/// @brief Defines the clear kernels, which write one value over one mip of a range of a texture made for storage only.
//----------------------------------------------------------------------------------------------------------------------
#include <metal_stdlib>

namespace rojoRHI {
    /// The parameters of one clear dispatch, laid out as `metal4::ClearParams` writes them.
    struct ClearParams
    {
        float4 color;     ///< The value written, red to alpha.
        uint baseLayer;   ///< The first layer cleared, counting faces for cube types; 0 for a 3D texture.
        uint mip;         ///< The mip cleared.
        uint width;       ///< The mip's width in texels.
        uint height;      ///< The mip's height in texels; 1 for 1D types.
        uint layers;      ///< The layers cleared, or the mip's depth for a 3D texture.
        uint reserved[3]; ///< Pads the struct to the size of its C++ counterpart.
    };
    static_assert(sizeof(ClearParams) == 48, "ClearParams must match metal4::ClearParams in Metal4Kernels.h");
} // namespace rojoRHI

/// The texture type a pipeline clears, as `metal4::ClearKernel` numbers it: each pipeline specialises one function for one type.
constant uint kClearType [[function_constant(0)]];
constant bool kClear1D = kClearType == 0;        ///< `MTL::TextureType1D`.
constant bool kClear1DArray = kClearType == 1;   ///< `MTL::TextureType1DArray`.
constant bool kClear2D = kClearType == 2;        ///< `MTL::TextureType2D`.
constant bool kClear2DArray = kClearType == 3;   ///< `MTL::TextureType2DArray`.
constant bool kClearCube = kClearType == 5;      ///< `MTL::TextureTypeCube`.
constant bool kClearCubeArray = kClearType == 6; ///< `MTL::TextureTypeCubeArray`.

//======================================================================================================================
/// Writes `p.color` to texel `id.x` of layer `p.baseLayer + id.z` of a 1D or 1D array texture, one thread per texel; threads
/// past the mip's extent write nothing.
kernel void rojorhi_clear_1d(
    constant rojoRHI::ClearParams& p [[buffer(0)]],
    metal::texture1d<float, metal::access::write> plain [[texture(0), function_constant(kClear1D)]],
    metal::texture1d_array<float, metal::access::write> array [[texture(0), function_constant(kClear1DArray)]],
    uint3 id [[thread_position_in_grid]]
)
{
    if (id.x >= p.width || id.y >= p.height || id.z >= p.layers)
    {
        return;
    }
    // A 1D texture has one mip (VU-rojo-texture-0042), so no write names a level.
    if (kClear1D)
    {
        plain.write(p.color, id.x);
    }
    else
    {
        array.write(p.color, id.x, p.baseLayer + id.z);
    }
}

//======================================================================================================================
/// Writes `p.color` to texel `id.xy` of layer `p.baseLayer + id.z` of mip `p.mip` of a 2D, 2D array, cube or cube array
/// texture, one thread per texel; threads past the mip's extent write nothing. A cube array's layer is a face of a cube.
kernel void rojorhi_clear_2d(
    constant rojoRHI::ClearParams& p [[buffer(0)]],
    metal::texture2d<float, metal::access::write> plain [[texture(0), function_constant(kClear2D)]],
    metal::texture2d_array<float, metal::access::write> array [[texture(0), function_constant(kClear2DArray)]],
    metal::texturecube<float, metal::access::write> cube [[texture(0), function_constant(kClearCube)]],
    metal::texturecube_array<float, metal::access::write> cubeArray [[texture(0), function_constant(kClearCubeArray)]],
    uint3 id [[thread_position_in_grid]]
)
{
    if (id.x >= p.width || id.y >= p.height || id.z >= p.layers)
    {
        return;
    }
    const uint layer = p.baseLayer + id.z;
    if (kClear2D)
    {
        plain.write(p.color, id.xy, p.mip);
    }
    else if (kClear2DArray)
    {
        array.write(p.color, id.xy, layer, p.mip);
    }
    else if (kClearCube)
    {
        cube.write(p.color, id.xy, layer, p.mip);
    }
    else
    {
        cubeArray.write(p.color, id.xy, layer % 6, layer / 6, p.mip);
    }
}

//======================================================================================================================
/// Writes `p.color` to texel `id` of mip `p.mip` of a 3D texture, one thread per texel; threads past the mip's extent write
/// nothing.
kernel void rojorhi_clear_3d(
    constant rojoRHI::ClearParams& p [[buffer(0)]], metal::texture3d<float, metal::access::write> volume [[texture(0)]], uint3 id [[thread_position_in_grid]]
)
{
    if (id.x >= p.width || id.y >= p.height || id.z >= p.layers)
    {
        return;
    }
    volume.write(p.color, id, p.mip);
}
