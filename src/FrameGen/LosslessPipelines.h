// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Barış Hamil

// The device objects of the Lossless generator that do not depend on the frame size: one
// compute pipeline per shader of a size mode, the descriptor layout they share and the three
// samplers (spec sections 2.3 and 6).

#pragma once

#include "LosslessShaders.h"

#include "rendervulkan.hpp"

#include <array>
#include <memory>
#include <string>

namespace gamescope::FrameGen
{
    enum class LosslessSampler : uint8_t
    {
        // Trilinear, border (0, 0, 0, 0).
        BorderBlack,
        // Trilinear, border (1, 1, 1, 1).
        BorderWhite,
        // Trilinear, clamp to edge.
        Clamp,
        Count,
    };

    class CLosslessPipelines
    {
    public:
        static std::unique_ptr<CLosslessPipelines> Create( const LosslessShaderSet_t &shaders, LosslessSizeMode eMode, bool bFp16, std::string &sError );
        ~CLosslessPipelines();

        CLosslessPipelines( const CLosslessPipelines & ) = delete;
        CLosslessPipelines &operator=( const CLosslessPipelines & ) = delete;

        // One set layout for every shader: each binds a subset of its slots.
        VkDescriptorSetLayout SetLayout() const { return m_setLayout; }
        VkPipelineLayout PipelineLayout() const { return m_pipelineLayout; }
        VkPipeline Pipeline( LosslessShader eShader ) const { return m_pipelines[ uint32_t( eShader ) ]; }
        VkSampler Sampler( LosslessSampler eSampler ) const { return m_samplers[ uint32_t( eSampler ) ]; }
        const LosslessShaderInterface_t &Interface( LosslessShader eShader ) const { return m_interfaces[ uint32_t( eShader ) ]; }

    private:
        CLosslessPipelines() = default;

        VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
        VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
        std::array<VkPipeline, k_uLosslessShaderCount> m_pipelines = {};
        std::array<VkSampler, uint32_t( LosslessSampler::Count )> m_samplers = {};
        std::array<LosslessShaderInterface_t, k_uLosslessShaderCount> m_interfaces = {};
    };
}
