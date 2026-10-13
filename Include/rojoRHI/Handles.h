//----------------------------------------------------------------------------------------------------------------------
/// @file Handles.h
/// @brief Declares the 64-bit generation-checked resource handles.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <cstdint>

namespace rojoRHI {
    /// A handle naming one buffer: a 64-bit identifier of slot index, generation and type tag whose bit positions
    /// are not part of the interface. The all-zero value is the null handle. It is meaningful only to the device that
    /// created it.
    struct Buffer
    {
        uint64_t bits = 0; ///< Opaque identifier; copy, compare and hash it, but do not decode it.

        /// Compares two handles by their bits.
        friend bool operator==(Buffer, Buffer) = default;
    };

    /// A handle naming one texture: a 64-bit identifier of slot index, generation and type tag whose bit positions
    /// are not part of the interface. The all-zero value is the null handle. It is meaningful only to the device that
    /// created it.
    struct Texture
    {
        uint64_t bits = 0; ///< Opaque identifier; copy, compare and hash it, but do not decode it.

        /// Compares two handles by their bits.
        friend bool operator==(Texture, Texture) = default;
    };

    /// A handle naming one pipeline: a 64-bit identifier of slot index, generation and type tag whose bit positions
    /// are not part of the interface. The all-zero value is the null handle. It is meaningful only to the device that
    /// created it.
    struct Pipeline
    {
        uint64_t bits = 0; ///< Opaque identifier; copy, compare and hash it, but do not decode it.

        /// Compares two handles by their bits.
        friend bool operator==(Pipeline, Pipeline) = default;
    };

    /// A handle naming one shader library: a 64-bit identifier of slot index, generation and type tag whose bit positions
    /// are not part of the interface. The all-zero value is the null handle. It is meaningful only to the device that
    /// created it.
    struct ShaderLibrary
    {
        uint64_t bits = 0; ///< Opaque identifier; copy, compare and hash it, but do not decode it.

        /// Compares two handles by their bits.
        friend bool operator==(ShaderLibrary, ShaderLibrary) = default;
    };

    /// A handle naming one heap: a 64-bit identifier of slot index, generation and type tag whose bit positions
    /// are not part of the interface. The all-zero value is the null handle. It is meaningful only to the device that
    /// created it.
    struct Heap
    {
        uint64_t bits = 0; ///< Opaque identifier; copy, compare and hash it, but do not decode it.

        /// Compares two handles by their bits.
        friend bool operator==(Heap, Heap) = default;
    };

    /// A handle naming one fence: a 64-bit identifier of slot index, generation and type tag whose bit positions
    /// are not part of the interface. The all-zero value is the null handle. It is meaningful only to the device that
    /// created it.
    struct Fence
    {
        uint64_t bits = 0; ///< Opaque identifier; copy, compare and hash it, but do not decode it.

        /// Compares two handles by their bits.
        friend bool operator==(Fence, Fence) = default;
    };

    /// A handle naming one upscaler: a 64-bit identifier of slot index, generation and type tag whose bit positions
    /// are not part of the interface. The all-zero value is the null handle. It is meaningful only to the device that
    /// created it.
    struct Upscaler
    {
        uint64_t bits = 0; ///< Opaque identifier; copy, compare and hash it, but do not decode it.

        /// Compares two handles by their bits.
        friend bool operator==(Upscaler, Upscaler) = default;
    };
} // namespace rojoRHI
