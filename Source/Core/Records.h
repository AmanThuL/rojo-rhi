//----------------------------------------------------------------------------------------------------------------------
/// @file Records.h
/// @brief Declares the record each handle type's pool stores and the table of hooks a backend registers to release them.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/Handles.h>

#include <string>
#include <type_traits>

namespace rojoRHI::core {
    /// What the core keeps about one live Buffer; later parts add its description and state.
    struct BufferRecord
    {
        std::string label;      ///< The label the object was created with.
        void* native = nullptr; ///< The backend's payload for the object; the core never reads it.
    };

    /// What the core keeps about one live Texture; later parts add its description and state.
    struct TextureRecord
    {
        std::string label;      ///< The label the object was created with.
        void* native = nullptr; ///< The backend's payload for the object; the core never reads it.
    };

    /// What the core keeps about one live Pipeline; later parts add its description and state.
    struct PipelineRecord
    {
        std::string label;      ///< The label the object was created with.
        void* native = nullptr; ///< The backend's payload for the object; the core never reads it.
    };

    /// What the core keeps about one live ShaderLibrary; later parts add its description and state.
    struct ShaderLibraryRecord
    {
        std::string label;      ///< The label the object was created with.
        void* native = nullptr; ///< The backend's payload for the object; the core never reads it.
    };

    /// What the core keeps about one live Heap; later parts add its description and state.
    struct HeapRecord
    {
        std::string label;      ///< The label the object was created with.
        void* native = nullptr; ///< The backend's payload for the object; the core never reads it.
    };

    /// What the core keeps about one live Fence; later parts add its description and state.
    struct FenceRecord
    {
        std::string label;      ///< The label the object was created with.
        void* native = nullptr; ///< The backend's payload for the object; the core never reads it.
    };

    /// What the core keeps about one live Upscaler; later parts add its description and state.
    struct UpscalerRecord
    {
        std::string label;      ///< The label the object was created with.
        void* native = nullptr; ///< The backend's payload for the object; the core never reads it.
    };

    /// Maps a handle type to the record its pool stores.
    template<typename Handle> struct RecordOf;

    /// Records of Buffer handles.
    template<> struct RecordOf<Buffer>
    {
        using Type = BufferRecord; ///< The record type.
    };
    /// Records of Texture handles.
    template<> struct RecordOf<Texture>
    {
        using Type = TextureRecord; ///< The record type.
    };
    /// Records of Pipeline handles.
    template<> struct RecordOf<Pipeline>
    {
        using Type = PipelineRecord; ///< The record type.
    };
    /// Records of ShaderLibrary handles.
    template<> struct RecordOf<ShaderLibrary>
    {
        using Type = ShaderLibraryRecord; ///< The record type.
    };
    /// Records of Heap handles.
    template<> struct RecordOf<Heap>
    {
        using Type = HeapRecord; ///< The record type.
    };
    /// Records of Fence handles.
    template<> struct RecordOf<Fence>
    {
        using Type = FenceRecord; ///< The record type.
    };
    /// Records of Upscaler handles.
    template<> struct RecordOf<Upscaler>
    {
        using Type = UpscalerRecord; ///< The record type.
    };

    /// The record type of `Handle`.
    template<typename Handle> using RecordFor = typename RecordOf<Handle>::Type;

    /// A function that releases the native payload of one retiring record; it receives the backend's `context`.
    template<typename Record> using RetireFn = void (*)(void* context, Record& record);

    /// The functions a backend registers so the core can hand it every record that leaves a pool. A null entry means the
    /// backend holds nothing for that type. A hook runs under its pool's lock and must not call the core's pools.
    struct RetireHooks
    {
        void* context = nullptr;                               ///< Passed to every hook; the backend's device state.
        RetireFn<BufferRecord> buffer = nullptr;               ///< Releases a retiring Buffer.
        RetireFn<TextureRecord> texture = nullptr;             ///< Releases a retiring Texture.
        RetireFn<PipelineRecord> pipeline = nullptr;           ///< Releases a retiring Pipeline.
        RetireFn<ShaderLibraryRecord> shaderLibrary = nullptr; ///< Releases a retiring ShaderLibrary.
        RetireFn<HeapRecord> heap = nullptr;                   ///< Releases a retiring Heap.
        RetireFn<FenceRecord> fence = nullptr;                 ///< Releases a retiring Fence.
        RetireFn<UpscalerRecord> upscaler = nullptr;           ///< Releases a retiring Upscaler.

        /// Returns the hook for `Handle`, which may be null.
        template<typename Handle> RetireFn<RecordFor<Handle>> forType() const
        {
            if constexpr (std::is_same_v<Handle, Buffer>)
            {
                return buffer;
            }
            else if constexpr (std::is_same_v<Handle, Texture>)
            {
                return texture;
            }
            else if constexpr (std::is_same_v<Handle, Pipeline>)
            {
                return pipeline;
            }
            else if constexpr (std::is_same_v<Handle, ShaderLibrary>)
            {
                return shaderLibrary;
            }
            else if constexpr (std::is_same_v<Handle, Heap>)
            {
                return heap;
            }
            else if constexpr (std::is_same_v<Handle, Fence>)
            {
                return fence;
            }
            else
            {
                static_assert(std::is_same_v<Handle, Upscaler>, "Handle is not a pooled handle type");
                return upscaler;
            }
        }
    };
} // namespace rojoRHI::core
