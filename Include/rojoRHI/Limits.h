//----------------------------------------------------------------------------------------------------------------------
/// @file Limits.h
/// @brief Declares the numeric limits a device reports.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <cstdint>

namespace rojoRHI {
    /// The numeric limits every other entry checks its arguments against; fixed for the lifetime of the device and at least
    /// the floor the reference states for each field.
    struct Limits
    {
        uint32_t maxViews = 0;                ///< ViewIndex slots in the resource heap.
        uint32_t maxSamplers = 0;             ///< SamplerIndex slots in the sampler heap.
        uint32_t maxRootBlockSize = 0;        ///< Bytes of one root block.
        uint32_t rootAlignment = 0;           ///< Byte alignment of a root block address.
        uint32_t frameDataAlignment = 0;      ///< Default uploadFrameData alignment.
        uint64_t maxBufferSize = 0;           ///< Bytes of one buffer.
        uint64_t maxAddrOffset = 0;           ///< Bytes an Addr reaches from its buffer's start.
        uint32_t maxTextureDimension1D = 0;   ///< Texels.
        uint32_t maxTextureDimension2D = 0;   ///< Texels, width and height.
        uint32_t maxTextureDimension3D = 0;   ///< Texels, each axis.
        uint32_t maxTextureDimensionCube = 0; ///< Texels, face width and height.
        uint32_t maxTextureLayers = 0;        ///< Array layers, six per cube.
        uint32_t maxColorAttachments = 0;     ///< ColorAttachment entries in one render pass.
        uint32_t maxColorBytesPerSample = 0;  ///< Summed attachment storage per sample.
        uint32_t maxSamplerAnisotropy = 0;    ///< SamplerDesc anisotropy.
        uint32_t maxDrawIndirectCount = 0;    ///< maxCount of one indirect draw call.
        uint32_t maxDispatchGroups = 0;       ///< Threadgroups per dispatch dimension.
        uint32_t maxThreadsPerGroup = 0;      ///< Threads in one compute threadgroup.
        uint32_t maxGroupSize[3] = {};        ///< Threads per threadgroup axis, x, y, z.
        uint32_t maxGroupSharedMemory = 0;    ///< Bytes of threadgroup (groupshared) memory.
        uint32_t maxMeshTaskGroups = 0;       ///< Mesh task groups per dimension.
        uint32_t maxMeshTaskGroupsTotal = 0;  ///< Product of the three mesh task dimensions.
    };
} // namespace rojoRHI
