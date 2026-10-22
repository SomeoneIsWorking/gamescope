// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Barış Hamil

// The resource interface of a SPIR-V compute module, read from its words: each descriptor's
// name, set, binding and kind, storage image formats, uniform block member offsets, the
// local size and whether it needs 16-bit floats.

#pragma once

#include "spirv.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace gamescope::FrameGen
{
    enum class SpirvDescriptorKind : uint8_t
    {
        UniformBuffer,
        Sampler,
        SampledImage,
        StorageImage,
    };

    const char *SpirvDescriptorKindName( SpirvDescriptorKind eKind );

    struct SpirvDescriptor_t
    {
        std::string sName;
        uint32_t uSet = 0;
        SpirvDescriptorKind eKind = SpirvDescriptorKind::UniformBuffer;
        // Storage images only.
        spv::ImageFormat eFormat = spv::ImageFormatUnknown;
        // Uniform buffers only.
        std::vector<uint32_t> uMemberOffsets;
    };

    struct SpirvInterface_t
    {
        std::array<uint32_t, 3> uLocalSize = {};
        bool bFloat16 = false;
        // By binding.
        std::map<uint32_t, SpirvDescriptor_t> descriptors;
    };

    // Nothing, with the reason, when the words are not a SPIR-V module with one compute entry point.
    std::optional<SpirvInterface_t> ReflectSpirv( std::span<const uint32_t> words, std::string &sError );
}
