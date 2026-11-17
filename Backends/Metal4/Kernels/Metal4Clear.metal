//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Clear.metal
/// @brief Defines the clear kernels, which write one value over one mip of a range of a texture made for storage only.
//----------------------------------------------------------------------------------------------------------------------
#include <metal_stdlib>

namespace rojoRHI {
    // Matches the layout `metal4::ClearParams` writes.
    struct ClearParams
    {
        float4 color;
        uint baseLayer; // Counts faces for cube types; 0 for a 3D texture.
        uint mip;
        uint width;
        uint height;      // 1 for 1D types.
        uint layers;      // Or the mip's depth for a 3D texture.
        uint reserved[3]; // Pads the struct to the size of its C++ counterpart.
    };
    static_assert(sizeof(ClearParams) == 48, "ClearParams must match metal4::ClearParams in Metal4Kernels.h");
} // namespace rojoRHI

// Numbered as `metal4::ClearKernel` numbers it: each pipeline specialises one function for one type.
constant uint kClearType [[function_constant(0)]];
constant bool kClear1D = kClearType == 0;
constant bool kClear1DArray = kClearType == 1;
constant bool kClear2D = kClearType == 2;
constant bool kClear2DArray = kClearType == 3;
constant bool kClearCube = kClearType == 5;
constant bool kClearCubeArray = kClearType == 6;

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
