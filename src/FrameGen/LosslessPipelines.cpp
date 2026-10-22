// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Barış Hamil

#include "LosslessPipelines.h"

#include <format>
#include <vector>

namespace gamescope::FrameGen
{
    std::unique_ptr<CLosslessPipelines> CLosslessPipelines::Create( const LosslessShaderSet_t &shaders, LosslessSizeMode eMode, bool bFp16, std::string &sError )
    {
        std::unique_ptr<CLosslessPipelines> p{ new CLosslessPipelines() };
        VkDevice device = g_device.device();

        std::vector<VkDescriptorSetLayoutBinding> bindings;
        bindings.push_back( { .binding = k_uLosslessUniformBinding, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT } );
        for ( uint32_t i = 0; i < k_uLosslessMaxSamplers; i++ )
            bindings.push_back( { .binding = k_uLosslessSamplerBinding + i, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT } );
        for ( uint32_t i = 0; i < k_uLosslessMaxInputs; i++ )
            bindings.push_back( { .binding = k_uLosslessInputBinding + i, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT } );
        for ( uint32_t i = 0; i < k_uLosslessMaxOutputs; i++ )
            bindings.push_back( { .binding = k_uLosslessOutputBinding + i, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT } );

        const VkDescriptorSetLayoutCreateInfo setLayoutInfo =
        {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = uint32_t( bindings.size() ),
            .pBindings = bindings.data(),
        };
        VkResult res = g_device.vk.CreateDescriptorSetLayout( device, &setLayoutInfo, nullptr, &p->m_setLayout );
        if ( res != VK_SUCCESS )
        {
            sError = std::format( "vkCreateDescriptorSetLayout failed: {}", int( res ) );
            return nullptr;
        }

        const VkPipelineLayoutCreateInfo pipelineLayoutInfo =
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1,
            .pSetLayouts = &p->m_setLayout,
        };
        res = g_device.vk.CreatePipelineLayout( device, &pipelineLayoutInfo, nullptr, &p->m_pipelineLayout );
        if ( res != VK_SUCCESS )
        {
            sError = std::format( "vkCreatePipelineLayout failed: {}", int( res ) );
            return nullptr;
        }

        std::string sFailure;
        std::array<VkShaderModule, k_uLosslessShaderCount> modules = {};
        std::array<VkComputePipelineCreateInfo, k_uLosslessShaderCount> pipelineInfos = {};
        for ( uint32_t i = 0; i < k_uLosslessShaderCount; i++ )
        {
            p->m_interfaces[ i ] = GetLosslessShaderInterface( LosslessShader( i ), eMode, bFp16 );
            const VkShaderModuleCreateInfo moduleInfo =
            {
                .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                .codeSize = shaders.modules[ i ].size() * sizeof( uint32_t ),
                .pCode = shaders.modules[ i ].data(),
            };
            res = g_device.vk.CreateShaderModule( device, &moduleInfo, nullptr, &modules[ i ] );
            if ( res != VK_SUCCESS )
            {
                sFailure = std::format( "vkCreateShaderModule for {} failed: {}", LosslessShaderName( LosslessShader( i ) ), int( res ) );
                break;
            }
            pipelineInfos[ i ] =
            {
                .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                .stage =
                {
                    .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                    .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                    .module = modules[ i ],
                    .pName = "main",
                },
                .layout = p->m_pipelineLayout,
            };
        }
        if ( sFailure.empty() )
        {
            res = g_device.vk.CreateComputePipelines( device, VK_NULL_HANDLE, k_uLosslessShaderCount, pipelineInfos.data(), nullptr, p->m_pipelines.data() );
            if ( res != VK_SUCCESS )
                sFailure = std::format( "vkCreateComputePipelines failed: {}", int( res ) );
        }
        for ( VkShaderModule module : modules )
        {
            if ( module != VK_NULL_HANDLE )
                g_device.vk.DestroyShaderModule( device, module, nullptr );
        }
        if ( !sFailure.empty() )
        {
            sError = sFailure;
            return nullptr;
        }

        auto CreateSampler = [&]( LosslessSampler eSampler, VkSamplerAddressMode eAddress, VkBorderColor eBorder )
        {
            const VkSamplerCreateInfo samplerInfo =
            {
                .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                .magFilter = VK_FILTER_LINEAR,
                .minFilter = VK_FILTER_LINEAR,
                .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
                .addressModeU = eAddress,
                .addressModeV = eAddress,
                .addressModeW = eAddress,
                .compareOp = VK_COMPARE_OP_NEVER,
                .maxLod = VK_LOD_CLAMP_NONE,
                .borderColor = eBorder,
            };
            return g_device.vk.CreateSampler( device, &samplerInfo, nullptr, &p->m_samplers[ uint32_t( eSampler ) ] );
        };
        if ( CreateSampler( LosslessSampler::BorderBlack, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK ) != VK_SUCCESS ||
             CreateSampler( LosslessSampler::BorderWhite, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE ) != VK_SUCCESS ||
             CreateSampler( LosslessSampler::Clamp, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK ) != VK_SUCCESS )
        {
            sError = "vkCreateSampler failed";
            return nullptr;
        }
        return p;
    }

    CLosslessPipelines::~CLosslessPipelines()
    {
        VkDevice device = g_device.device();
        for ( VkSampler sampler : m_samplers )
        {
            if ( sampler != VK_NULL_HANDLE )
                g_device.vk.DestroySampler( device, sampler, nullptr );
        }
        for ( VkPipeline pipeline : m_pipelines )
        {
            if ( pipeline != VK_NULL_HANDLE )
                g_device.vk.DestroyPipeline( device, pipeline, nullptr );
        }
        if ( m_pipelineLayout != VK_NULL_HANDLE )
            g_device.vk.DestroyPipelineLayout( device, m_pipelineLayout, nullptr );
        if ( m_setLayout != VK_NULL_HANDLE )
            g_device.vk.DestroyDescriptorSetLayout( device, m_setLayout, nullptr );
    }
}
