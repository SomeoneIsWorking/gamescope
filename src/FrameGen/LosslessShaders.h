// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Barış Hamil

// The LSFG 3 compute shaders of the player's Lossless.dll: which resource holds each one, the
// interface each must have, and loading them after checking that interface
// (docs/design/frame-generation-spec.md in openSU, sections 2 and 8).

#pragma once

#include "SpirvInterface.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace gamescope::FrameGen
{
    enum class LosslessSizeMode : uint8_t
    {
        Balanced = 0,
        Performance = 1,
    };

    // In resource order: Copy, LumaPyramid and Compose are shared, the rest are 257 + index
    // (balanced) or 280 + index (performance) for index = shader - FlowEntry.
    enum class LosslessShader : uint8_t
    {
        Copy,
        LumaPyramid,
        Compose,
        FlowEntry,
        FlowEntryB,
        FlowAFilter1,
        FlowAFilter2,
        FlowAFilter3,
        FlowAResolve,
        FlowBFilter1,
        FlowBFilter2,
        FlowBFilter3,
        FlowBResolve,
        Feature1,
        Feature2,
        Feature3,
        Feature4,
        FlowCFilter1,
        FlowCFilter2,
        FlowCFilter3,
        FlowCResolve,
        Ui1,
        Ui2,
        Ui3,
        Ui4,
        UiMask,
        Count,
    };

    inline constexpr uint32_t k_uLosslessShaderCount = uint32_t( LosslessShader::Count );

    // SPIR-V descriptor set 0 bindings: DXC's shifts of b0, s0.., t0.., u0...
    inline constexpr uint32_t k_uLosslessUniformBinding = 0;
    inline constexpr uint32_t k_uLosslessSamplerBinding = 16;
    inline constexpr uint32_t k_uLosslessInputBinding = 32;
    inline constexpr uint32_t k_uLosslessOutputBinding = 48;
    inline constexpr uint32_t k_uLosslessMaxSamplers = 2;
    inline constexpr uint32_t k_uLosslessMaxInputs = 12;
    inline constexpr uint32_t k_uLosslessMaxOutputs = 7;

    // What a shader must declare, per the spec.
    struct LosslessShaderInterface_t
    {
        uint32_t uLocalSize = 8;
        // Members of the cb block; 0 when the module declares none.
        uint32_t uUniformMembers = 0;
        uint32_t uSamplers = 0;
        uint32_t uInputs = 0;
        uint32_t uOutputs = 0;
        spv::ImageFormat eOutputFormat = spv::ImageFormatUnknown;
        bool bFloat16 = false;
    };

    LosslessShaderInterface_t GetLosslessShaderInterface( LosslessShader eShader, LosslessSizeMode eMode, bool bFp16 );
    const char *LosslessShaderName( LosslessShader eShader );
    // The DLL's resource id of a shader's SPIR-V module.
    uint32_t LosslessShaderResourceId( LosslessShader eShader, LosslessSizeMode eMode, bool bFp16 );

    struct LosslessShaderSet_t
    {
        std::string sFileVersion;
        std::array<std::vector<uint32_t>, k_uLosslessShaderCount> modules;
    };

    // Every module of one size mode and precision, or nothing with the reason when the DLL or
    // any module differs from the spec.
    std::optional<LosslessShaderSet_t> LoadLosslessShaders( const std::string &sDllPath, LosslessSizeMode eMode, bool bFp16, std::string &sError );
}
