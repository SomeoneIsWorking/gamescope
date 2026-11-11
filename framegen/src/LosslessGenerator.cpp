// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Barış Hamil

#include "LosslessGeneratorImpl.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <format>
#include <utility>

namespace framegen
{
    namespace
    {
        uint32_t DivRoundUp( uint32_t u, uint32_t uDivisor )
        {
            return ( u + uDivisor - 1 ) / uDivisor;
        }

        uint32_t AlignUp( uint32_t u, uint32_t uAlignment )
        {
            return ( u + uAlignment - 1 ) / uAlignment * uAlignment;
        }

        // Every format the generator's images use, with what the shaders do to them.
        bool CheckFormats( const CDevice &device, std::string &sError )
        {
            const VkFormatFeatureFlags eNeeded = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
            for ( VkFormat eFormat : { VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8_UNORM } )
            {
                if ( ( device.OptimalTilingFeatures( eFormat ) & eNeeded ) != eNeeded )
                {
                    sError = std::format( "the GPU cannot sample, filter and store format {}", int( eFormat ) );
                    return false;
                }
            }
            return true;
        }
    }

    CLosslessGeneratorImpl::CLosslessGeneratorImpl( std::shared_ptr<const CDevice> pDevice )
        : m_pDevice{ std::move( pDevice ) }
    {
    }

    std::unique_ptr<CLosslessGeneratorImpl> CLosslessGeneratorImpl::Create( std::shared_ptr<const CDevice> pDevice, const LosslessSettings_t &settings, std::string &sError )
    {
        if ( !pDevice->SupportsStorageWriteWithoutFormat() || !pDevice->SupportsStorageExtendedFormats() )
        {
            sError = "the GPU lacks shaderStorageImageWriteWithoutFormat or shaderStorageImageExtendedFormats";
            return nullptr;
        }
        if ( !CheckFormats( *pDevice, sError ) )
            return nullptr;

        std::unique_ptr<CLosslessGeneratorImpl> p{ new CLosslessGeneratorImpl( std::move( pDevice ) ) };
        const CDevice &dev = *p->m_pDevice;
        p->m_Settings = settings;
        p->m_Settings.flFlowScale = std::clamp( settings.flFlowScale, k_flLosslessMinFlowScale, 1.0f );
        p->m_bFp16 = dev.SupportsFloat16();
        p->m_Layout = GetLosslessModeLayout( settings.eSizeMode );

        std::optional<LosslessShaderSet_t> oShaders = LoadLosslessShaders( settings.sDllPath, settings.eSizeMode, p->m_bFp16, sError );
        if ( !oShaders )
            return nullptr;
        p->m_pPipelines = CLosslessPipelines::Create( p->m_pDevice, *oShaders, settings.eSizeMode, p->m_bFp16, sError );
        if ( !p->m_pPipelines )
            return nullptr;

        VkDevice device = dev.Device();
        p->m_uUniformStride = AlignUp( uint32_t( sizeof( LevelConstants_t ) ), uint32_t( dev.Properties().limits.minUniformBufferOffsetAlignment ) );

        const VkBufferCreateInfo bufferInfo =
        {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = VkDeviceSize( p->m_uUniformStride ) * k_uLosslessMaxLevels * k_uSlots,
            .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        VkResult res = dev.vk().CreateBuffer( device, &bufferInfo, nullptr, &p->m_uniformBuffer );
        if ( res != VK_SUCCESS )
        {
            sError = std::format( "vkCreateBuffer failed: {}", int( res ) );
            return nullptr;
        }
        VkMemoryRequirements requirements;
        dev.vk().GetBufferMemoryRequirements( device, p->m_uniformBuffer, &requirements );
        const int32_t nMemoryType = dev.FindMemoryType( VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, requirements.memoryTypeBits );
        if ( nMemoryType < 0 )
        {
            sError = "no host-visible coherent memory for the constants";
            return nullptr;
        }
        const VkMemoryAllocateInfo allocateInfo =
        {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = requirements.size,
            .memoryTypeIndex = uint32_t( nMemoryType ),
        };
        res = dev.vk().AllocateMemory( device, &allocateInfo, nullptr, &p->m_uniformMemory );
        if ( res == VK_SUCCESS )
            res = dev.vk().BindBufferMemory( device, p->m_uniformBuffer, p->m_uniformMemory, 0 );
        void *pData = nullptr;
        if ( res == VK_SUCCESS )
            res = dev.vk().MapMemory( device, p->m_uniformMemory, 0, VK_WHOLE_SIZE, 0, &pData );
        if ( res != VK_SUCCESS )
        {
            sError = std::format( "constant buffer memory failed: {}", int( res ) );
            return nullptr;
        }
        p->m_pUniformData = static_cast<uint8_t *>( pData );

        // Sized for the largest submission: a generated frame at 7 and 6 levels.
        const std::array<VkDescriptorPoolSize, 4> poolSizes =
        { {
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, k_uMaxPassesPerSlot },
            { VK_DESCRIPTOR_TYPE_SAMPLER, k_uMaxPassesPerSlot * k_uLosslessMaxSamplers },
            { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, k_uMaxPassesPerSlot * k_uLosslessMaxInputs },
            { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, k_uMaxPassesPerSlot * k_uLosslessMaxOutputs },
        } };
        for ( Slot_t &slot : p->m_Slots )
        {
            const VkDescriptorPoolCreateInfo poolInfo =
            {
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                .maxSets = k_uMaxPassesPerSlot,
                .poolSizeCount = uint32_t( poolSizes.size() ),
                .pPoolSizes = poolSizes.data(),
            };
            res = dev.vk().CreateDescriptorPool( device, &poolInfo, nullptr, &slot.pool );
            if ( res != VK_SUCCESS )
            {
                sError = std::format( "vkCreateDescriptorPool failed: {}", int( res ) );
                return nullptr;
            }
        }

        p->Log( LogLevel::Info, std::format( "Lossless.dll {} loaded: {}, {}, flow scale {:.2f}, UI detection {}",
            oShaders->sFileVersion, settings.eSizeMode == LosslessSizeMode::Performance ? "performance" : "balanced",
            p->m_bFp16 ? "FP16" : "FP32", p->m_Settings.flFlowScale, settings.bUiDetection ? "on" : "off" ) );
        return p;
    }

    CLosslessGeneratorImpl::~CLosslessGeneratorImpl()
    {
        WaitIdle();
        VkDevice device = m_pDevice->Device();
        for ( Slot_t &slot : m_Slots )
        {
            if ( slot.pool != VK_NULL_HANDLE )
                m_pDevice->vk().DestroyDescriptorPool( device, slot.pool, nullptr );
        }
        if ( m_uniformBuffer != VK_NULL_HANDLE )
            m_pDevice->vk().DestroyBuffer( device, m_uniformBuffer, nullptr );
        if ( m_uniformMemory != VK_NULL_HANDLE )
            m_pDevice->vk().FreeMemory( device, m_uniformMemory, nullptr );
    }

    void CLosslessGeneratorImpl::WaitIdle()
    {
        // Our own newest submission only, never the host's newer ones.
        if ( m_ulLastSeqNo && m_pDevice->Queue().Completed() < m_ulLastSeqNo )
            m_pDevice->Queue().Wait( m_ulLastSeqNo );
    }

    bool CLosslessGeneratorImpl::BeginSession( uint32_t uWidth, uint32_t uHeight )
    {
        m_ulRealFrames = 0;
        m_uFrameSlot = 0;
        m_uFeatureSlot = 0;

        const LosslessGeometry_t geometry = ComputeLosslessGeometry( uWidth, uHeight, m_Settings.flFlowScale, 0 );
        if ( !m_pImages || m_pImages->Geometry().frame.uWidth != uWidth || m_pImages->Geometry().frame.uHeight != uHeight )
        {
            WaitIdle();
            m_pImages = nullptr;
            std::string sError;
            m_pImages = CLosslessImages::Create( m_pDevice, geometry, m_Layout, m_Settings.bUiDetection, sError );
            if ( !m_pImages )
            {
                Log( LogLevel::Error, std::format( "Images for {}x{}: {}", uWidth, uHeight, sError ) );
                return false;
            }
            m_sIngestKey = std::format( "ingest {}x{}", uWidth, uHeight );
            m_sGenerateKey = std::format( "generate {}x{}", uWidth, uHeight );
            Log( LogLevel::Info, std::format( "{}x{}: flow {}x{} (scale {:.2f}), {} and {} levels", uWidth, uHeight, geometry.flow.uWidth, geometry.flow.uHeight,
                geometry.flFlowScale, geometry.uLevels, geometry.uLevels2 ) );
        }
        return ClearImages();
    }

    bool CLosslessGeneratorImpl::ClearImages()
    {
        std::optional<Recording_t> oRecording = BeginRecording( 0.0f );
        if ( !oRecording )
            return false;
        VkCommandBuffer cmd = oRecording->cmd;

        const VkImageSubresourceRange range = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1 };
        std::vector<VkImageMemoryBarrier> barriers;
        for ( VkImage image : m_pImages->AllImages() )
        {
            barriers.push_back( {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .srcAccessMask = 0,
                .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = image,
                .subresourceRange = range,
            } );
        }
        m_pDevice->vk().CmdPipelineBarrier( cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
            uint32_t( barriers.size() ), barriers.data() );

        // History reads zero until real frames fill it (spec 7).
        const VkClearColorValue zero = {};
        for ( const VkImageMemoryBarrier &barrier : barriers )
            m_pDevice->vk().CmdClearColorImage( cmd, barrier.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range );

        const VkMemoryBarrier toCompute =
        {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
        };
        m_pDevice->vk().CmdPipelineBarrier( cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &toCompute, 0, nullptr, 0, nullptr );
        Submit( *oRecording, "clear" );
        return !oRecording->bFailed;
    }

    std::optional<CLosslessGeneratorImpl::Recording_t> CLosslessGeneratorImpl::BeginRecording( float flTimestamp )
    {
        Recording_t recording;
        recording.uSlot = m_uNextSlot;
        m_uNextSlot = ( m_uNextSlot + 1 ) % k_uSlots;

        Slot_t &slot = m_Slots[ recording.uSlot ];
        // Older than the newest submission, so this never waits for the host's newer work.
        IQueue &queue = m_pDevice->Queue();
        if ( slot.ulSeqNo && queue.Completed() < slot.ulSeqNo )
            queue.Wait( slot.ulSeqNo );
        m_pDevice->vk().ResetDescriptorPool( m_pDevice->Device(), slot.pool, 0 );

        if ( m_pImages )
        {
            const LosslessGeometry_t &geometry = m_pImages->Geometry();
            for ( uint32_t l = 0; l < geometry.uLevels; l++ )
            {
                const LevelConstants_t constants =
                {
                    .uInputOffset = { 0, 0 },
                    .uFirstIter = l == geometry.uLevels - 1 ? 1u : 0u,
                    .uFirstIterS = l == geometry.uLevels2 - 1 ? 1u : 0u,
                    // 8-bit SDR: no gamma or HDR scaling in the luma pass.
                    .uAdvancedColorKind = 0,
                    .uHdrSupport = 0,
                    .flResolutionInvScale = 1.0f / geometry.flFlowScale,
                    .flTimestamp = flTimestamp,
                    .flUiThreshold = k_flLosslessDefaultUiThreshold,
                    .uPadding = {},
                };
                memcpy( m_pUniformData + ( size_t( recording.uSlot ) * k_uLosslessMaxLevels + l ) * m_uUniformStride, &constants, sizeof( constants ) );
            }
        }

        recording.cmd = queue.Begin();
        if ( recording.cmd == VK_NULL_HANDLE )
            return std::nullopt;
        recording.ouTimingSlot = queue.BeginTiming( recording.cmd );

        // Earlier submissions (the frame copy, the composites reading old outputs) before ours.
        const VkMemoryBarrier barrier =
        {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
        };
        m_pDevice->vk().CmdPipelineBarrier( recording.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr );
        return recording;
    }

    void CLosslessGeneratorImpl::Dispatch( Recording_t &recording, const Pass_t &pass )
    {
        const LosslessShaderInterface_t &iface = m_pPipelines->Interface( pass.eShader );
        assert( pass.inputs.size() <= iface.uInputs );
        assert( pass.outputs.size() == iface.uOutputs );

        VkDescriptorSet set = VK_NULL_HANDLE;
        const VkDescriptorSetLayout setLayout = m_pPipelines->SetLayout();
        const VkDescriptorSetAllocateInfo allocateInfo =
        {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = m_Slots[ recording.uSlot ].pool,
            .descriptorSetCount = 1,
            .pSetLayouts = &setLayout,
        };
        const VkResult res = m_pDevice->vk().AllocateDescriptorSets( m_pDevice->Device(), &allocateInfo, &set );
        if ( res != VK_SUCCESS )
        {
            Log( LogLevel::Error, std::format( "vkAllocateDescriptorSets for {} failed: {}", LosslessShaderName( pass.eShader ), int( res ) ) );
            recording.bFailed = true;
            return;
        }

        const VkDescriptorBufferInfo bufferInfo = { .buffer = m_uniformBuffer, .offset = 0, .range = sizeof( LevelConstants_t ) };
        std::array<VkDescriptorImageInfo, k_uLosslessMaxSamplers + k_uLosslessMaxInputs + k_uLosslessMaxOutputs> imageInfos = {};
        std::array<VkWriteDescriptorSet, 1 + k_uLosslessMaxSamplers + k_uLosslessMaxInputs + k_uLosslessMaxOutputs> writes = {};
        uint32_t uWrites = 0;
        uint32_t uImageInfos = 0;
        auto Write = [&]( uint32_t uBinding, VkDescriptorType eType, VkDescriptorImageInfo info )
        {
            imageInfos[ uImageInfos ] = info;
            writes[ uWrites++ ] = {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = set,
                .dstBinding = uBinding,
                .descriptorCount = 1,
                .descriptorType = eType,
                .pImageInfo = &imageInfos[ uImageInfos++ ],
            };
        };

        writes[ uWrites++ ] = {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = set,
            .dstBinding = k_uLosslessUniformBinding,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
            .pBufferInfo = &bufferInfo,
        };
        Write( k_uLosslessSamplerBinding, VK_DESCRIPTOR_TYPE_SAMPLER, { .sampler = m_pPipelines->Sampler( pass.eSampler0 ) } );
        Write( k_uLosslessSamplerBinding + 1, VK_DESCRIPTOR_TYPE_SAMPLER, { .sampler = m_pPipelines->Sampler( LosslessSampler::Clamp ) } );
        for ( uint32_t i = 0; i < iface.uInputs; i++ )
        {
            const VkImageView view = i < pass.inputs.size() ? pass.inputs[ i ] : VK_NULL_HANDLE;
            Write( k_uLosslessInputBinding + i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, { .imageView = view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL } );
        }
        for ( uint32_t i = 0; i < iface.uOutputs; i++ )
            Write( k_uLosslessOutputBinding + i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, { .imageView = pass.outputs[ i ], .imageLayout = VK_IMAGE_LAYOUT_GENERAL } );
        m_pDevice->vk().UpdateDescriptorSets( m_pDevice->Device(), uWrites, writes.data(), 0, nullptr );

        VkCommandBuffer cmd = recording.cmd;
        const uint32_t uDynamicOffset = ( recording.uSlot * k_uLosslessMaxLevels + pass.uLevel ) * m_uUniformStride;
        m_pDevice->vk().CmdBindPipeline( cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pPipelines->Pipeline( pass.eShader ) );
        m_pDevice->vk().CmdBindDescriptorSets( cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_pPipelines->PipelineLayout(), 0, 1, &set, 1, &uDynamicOffset );
        m_pDevice->vk().CmdDispatch( cmd, pass.uGroupsX, pass.uGroupsY, 1 );

        // Almost every pass reads the one before, and the Quarter set is reused within a level.
        const VkMemoryBarrier barrier =
        {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
        };
        m_pDevice->vk().CmdPipelineBarrier( cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr );
    }

    uint64_t CLosslessGeneratorImpl::Submit( Recording_t &recording, const std::string &sTimingKey )
    {
        // Our writes before whatever reads them next: the composite, PipeWire, the next submission.
        const VkMemoryBarrier barrier =
        {
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        };
        m_pDevice->vk().CmdPipelineBarrier( recording.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &barrier, 0, nullptr, 0, nullptr );

        IQueue &queue = m_pDevice->Queue();
        if ( recording.ouTimingSlot )
            queue.EndTiming( recording.cmd, *recording.ouTimingSlot, sTimingKey );

        const uint64_t ulSeqNo = queue.Submit( recording.cmd );
        if ( recording.ouTimingSlot )
            queue.TimingSubmitted( ulSeqNo );
        m_Slots[ recording.uSlot ].ulSeqNo = ulSeqNo;
        m_ulLastSeqNo = ulSeqNo;
        return ulSeqNo;
    }

    void CLosslessGeneratorImpl::AddFeatures( std::vector<VkImageView> &views, uint32_t uSlot, uint32_t uLevel ) const
    {
        for ( uint32_t c = 0; c < m_Layout.uFeatureChannels; c++ )
            views.push_back( m_pImages->Feature( uSlot, c, uLevel ).View() );
    }

    std::vector<VkImageView> CLosslessGeneratorImpl::Quarters( uint32_t uFirst, uint32_t uCount, uint32_t uLevel ) const
    {
        std::vector<VkImageView> views;
        for ( uint32_t i = 0; i < uCount; i++ )
            views.push_back( m_pImages->Quarter( uFirst + i, uLevel ).View() );
        return views;
    }

    void CLosslessGeneratorImpl::RecordIngest( Recording_t &recording, VkImageView frame )
    {
        const LosslessGeometry_t &g = m_pImages->Geometry();
        const uint32_t nC = m_Layout.uFeatureChannels;

        m_uFrameSlot ^= 1;
        // The UNORM view: raw values, as Lossless Scaling's capture sees them.
        Dispatch( recording, { .eShader = LosslessShader::Copy, .inputs = { frame }, .outputs = { m_pImages->FrameCopy( m_uFrameSlot ).View() },
            .uGroupsX = DivRoundUp( g.frame.uWidth, 8 ), .uGroupsY = DivRoundUp( g.frame.uHeight, 8 ) } );

        Pass_t luma{ .eShader = LosslessShader::LumaPyramid, .inputs = { m_pImages->FrameCopy( m_uFrameSlot ).View() },
            .uGroupsX = DivRoundUp( g.flow.uWidth, 64 ), .uGroupsY = DivRoundUp( g.flow.uHeight, 64 ) };
        for ( uint32_t i = 0; i < k_uLosslessLumaLevels; i++ )
            luma.outputs.push_back( m_pImages->Luma( i ).View() );
        Dispatch( recording, luma );

        m_uFeatureSlot = ( m_uFeatureSlot + 1 ) % k_uLosslessFeatureSlots;
        for ( uint32_t l = g.uLevels; l-- > 0; )
        {
            const LosslessExtent_t half = g.Half( l );
            const LosslessExtent_t quarter = g.Quarter( l );
            auto Halves = [&]( uint32_t uFirst, uint32_t uCount )
            {
                std::vector<VkImageView> views;
                for ( uint32_t i = 0; i < uCount; i++ )
                    views.push_back( m_pImages->Half( uFirst + i, l ).View() );
                return views;
            };

            Dispatch( recording, { .eShader = LosslessShader::Feature1, .inputs = { m_pImages->Luma( std::min( l, k_uLosslessLumaLevels - 1 ) ).View() },
                .outputs = Halves( 0, nC / 2 ), .uLevel = l, .uGroupsX = DivRoundUp( half.uWidth, 8 ), .uGroupsY = DivRoundUp( half.uHeight, 8 ) } );
            Dispatch( recording, { .eShader = LosslessShader::Feature2, .inputs = Halves( 0, nC / 2 ), .outputs = Halves( nC / 2, nC / 2 ),
                .uLevel = l, .uGroupsX = DivRoundUp( half.uWidth, 8 ), .uGroupsY = DivRoundUp( half.uHeight, 8 ) } );
            Dispatch( recording, { .eShader = LosslessShader::Feature3, .inputs = Halves( nC / 2, nC / 2 ), .outputs = Quarters( 0, nC, l ),
                .uLevel = l, .uGroupsX = DivRoundUp( quarter.uWidth, 8 ), .uGroupsY = DivRoundUp( quarter.uHeight, 8 ) } );
            Pass_t feature{ .eShader = LosslessShader::Feature4, .inputs = Quarters( 0, nC, l ),
                .uLevel = l, .uGroupsX = DivRoundUp( quarter.uWidth, 8 ), .uGroupsY = DivRoundUp( quarter.uHeight, 8 ) };
            AddFeatures( feature.outputs, m_uFeatureSlot, l );
            Dispatch( recording, feature );
        }
        m_ulRealFrames++;
    }

    void CLosslessGeneratorImpl::RecordUiMask( Recording_t &recording )
    {
        const LosslessGeometry_t &g = m_pImages->Geometry();
        const LosslessExtent_t quarter = g.Quarter( 0 );
        const uint32_t uGroupsX = DivRoundUp( quarter.uWidth, 8 );
        const uint32_t uGroupsY = DivRoundUp( quarter.uHeight, 8 );
        const uint32_t k = m_uFeatureSlot;

        Pass_t first{ .eShader = LosslessShader::Ui1, .outputs = Quarters( 0, 2, 0 ), .eSampler0 = LosslessSampler::BorderWhite, .uGroupsX = uGroupsX, .uGroupsY = uGroupsY };
        // Oldest, previous, newest real frame.
        AddFeatures( first.inputs, ( k + 1 ) % k_uLosslessFeatureSlots, 0 );
        AddFeatures( first.inputs, ( k + 2 ) % k_uLosslessFeatureSlots, 0 );
        AddFeatures( first.inputs, k, 0 );
        Dispatch( recording, first );
        Dispatch( recording, { .eShader = LosslessShader::Ui2, .inputs = Quarters( 0, 2, 0 ), .outputs = Quarters( 2, 2, 0 ), .uGroupsX = uGroupsX, .uGroupsY = uGroupsY } );
        Dispatch( recording, { .eShader = LosslessShader::Ui3, .inputs = Quarters( 2, 2, 0 ), .outputs = Quarters( 0, 2, 0 ), .uGroupsX = uGroupsX, .uGroupsY = uGroupsY } );
        Dispatch( recording, { .eShader = LosslessShader::Ui4, .inputs = Quarters( 0, 2, 0 ), .outputs = Quarters( 2, 2, 0 ), .uGroupsX = uGroupsX, .uGroupsY = uGroupsY } );

        Pass_t mask{ .eShader = LosslessShader::UiMask, .inputs = Quarters( 2, 2, 0 ),
            .uGroupsX = DivRoundUp( quarter.uWidth, 32 ), .uGroupsY = DivRoundUp( quarter.uHeight, 32 ) };
        for ( uint32_t i = 0; i < k_uLosslessUiMaskLevels; i++ )
            mask.outputs.push_back( m_pImages->UiMask( i ).View() );
        Dispatch( recording, mask );
    }

    void CLosslessGeneratorImpl::RecordGenerate( Recording_t &recording, VkImageView output )
    {
        const LosslessGeometry_t &g = m_pImages->Geometry();
        const uint32_t nC = m_Layout.uFeatureChannels;
        const uint32_t fo = m_Layout.uFilterOutput;
        const uint32_t k = m_uFeatureSlot;
        const uint32_t p = ( k + 2 ) % k_uLosslessFeatureSlots;
        // The mask is meaningful once three real frames are in (spec 4.2).
        const bool bMask = m_pImages->HasUiMask() && m_ulRealFrames >= k_uLosslessFeatureSlots;

        struct Flow_t
        {
            LosslessShader eFilter1, eFilter2, eFilter3, eResolve;
        };

        for ( uint32_t l = g.uLevels; l-- > 0; )
        {
            const LosslessExtent_t quarter = g.Quarter( l );
            const uint32_t uGroupsX = DivRoundUp( quarter.uWidth, 8 );
            const uint32_t uGroupsY = DivRoundUp( quarter.uHeight, 8 );
            const VkImageView mask = bMask ? m_pImages->UiMask( std::min( l, k_uLosslessUiMaskLevels - 1 ) ).View() : VK_NULL_HANDLE;
            const VkImageView flowANext = l + 1 < g.uLevels ? m_pImages->FlowA( l + 1 ).View() : VK_NULL_HANDLE;
            const VkImageView flowBNext = l + 1 < g.uLevels2 ? m_pImages->FlowB( l + 1 ).View() : VK_NULL_HANDLE;
            const VkImageView flowCNext = l + 1 < g.uLevels2 ? m_pImages->FlowC( l + 1 ).View() : VK_NULL_HANDLE;

            // G1..G5 for FlowA, H1..H5 for FlowB.
            auto RecordFlow = [&]( const Flow_t &flow, VkImageView next, VkImageView target )
            {
                Pass_t entry{ .eShader = LosslessShader::FlowEntry, .outputs = Quarters( 0, 3, l ), .eSampler0 = LosslessSampler::BorderWhite,
                    .uLevel = l, .uGroupsX = uGroupsX, .uGroupsY = uGroupsY };
                AddFeatures( entry.inputs, p, l );
                AddFeatures( entry.inputs, k, l );
                entry.inputs.push_back( next );
                Dispatch( recording, entry );
                Dispatch( recording, { .eShader = flow.eFilter1, .inputs = Quarters( 0, 3, l ), .outputs = Quarters( fo, nC, l ), .uLevel = l, .uGroupsX = uGroupsX, .uGroupsY = uGroupsY } );
                Dispatch( recording, { .eShader = flow.eFilter2, .inputs = Quarters( fo, nC, l ), .outputs = Quarters( 0, nC, l ), .uLevel = l, .uGroupsX = uGroupsX, .uGroupsY = uGroupsY } );
                Dispatch( recording, { .eShader = flow.eFilter3, .inputs = Quarters( 0, nC, l ), .outputs = Quarters( fo, nC, l ), .uLevel = l, .uGroupsX = uGroupsX, .uGroupsY = uGroupsY } );
                Pass_t resolve{ .eShader = flow.eResolve, .inputs = Quarters( fo, nC, l ), .outputs = { target }, .uLevel = l, .uGroupsX = uGroupsX, .uGroupsY = uGroupsY };
                resolve.inputs.push_back( next );
                resolve.inputs.push_back( mask );
                Dispatch( recording, resolve );
            };

            RecordFlow( { LosslessShader::FlowAFilter1, LosslessShader::FlowAFilter2, LosslessShader::FlowAFilter3, LosslessShader::FlowAResolve },
                flowANext, m_pImages->FlowA( l ).View() );
            if ( l >= g.uLevels2 )
                continue;

            RecordFlow( { LosslessShader::FlowBFilter1, LosslessShader::FlowBFilter2, LosslessShader::FlowBFilter3, LosslessShader::FlowBResolve },
                flowBNext, m_pImages->FlowB( l ).View() );

            // H6..H10 for FlowC.
            const uint32_t uHalfC = nC / 2;
            Pass_t entry{ .eShader = LosslessShader::FlowEntryB, .outputs = Quarters( 0, uHalfC, l ), .eSampler0 = LosslessSampler::BorderWhite,
                .uLevel = l, .uGroupsX = uGroupsX, .uGroupsY = uGroupsY };
            AddFeatures( entry.inputs, p, l );
            AddFeatures( entry.inputs, k, l );
            entry.inputs.push_back( flowANext );
            entry.inputs.push_back( flowBNext );
            Dispatch( recording, entry );
            Dispatch( recording, { .eShader = LosslessShader::FlowCFilter1, .inputs = Quarters( 0, uHalfC, l ), .outputs = Quarters( uHalfC, uHalfC, l ), .uLevel = l, .uGroupsX = uGroupsX, .uGroupsY = uGroupsY } );
            Dispatch( recording, { .eShader = LosslessShader::FlowCFilter2, .inputs = Quarters( uHalfC, uHalfC, l ), .outputs = Quarters( 0, uHalfC, l ), .uLevel = l, .uGroupsX = uGroupsX, .uGroupsY = uGroupsY } );
            Dispatch( recording, { .eShader = LosslessShader::FlowCFilter3, .inputs = Quarters( 0, uHalfC, l ), .outputs = Quarters( uHalfC, uHalfC, l ), .uLevel = l, .uGroupsX = uGroupsX, .uGroupsY = uGroupsY } );
            Pass_t resolve{ .eShader = LosslessShader::FlowCResolve, .inputs = Quarters( uHalfC, uHalfC, l ), .outputs = { m_pImages->FlowC( l ).View() },
                .uLevel = l, .uGroupsX = uGroupsX, .uGroupsY = uGroupsY };
            resolve.inputs.push_back( flowCNext );
            Dispatch( recording, resolve );
        }

        Dispatch( recording, { .eShader = LosslessShader::Compose,
            .inputs = { m_pImages->FrameCopy( m_uFrameSlot ^ 1 ).View(), m_pImages->FrameCopy( m_uFrameSlot ).View(),
                m_pImages->FlowA( 0 ).View(), m_pImages->FlowB( 0 ).View(), m_pImages->FlowC( 0 ).View() },
            .outputs = { output },
            .uGroupsX = DivRoundUp( g.frame.uWidth, 16 ), .uGroupsY = DivRoundUp( g.frame.uHeight, 16 ) } );
    }

    std::optional<Submission_t> CLosslessGeneratorImpl::AddRealFrame( const GeneratorImage_t &frame )
    {
        if ( !m_pImages || frame.uWidth != m_pImages->Geometry().frame.uWidth || frame.uHeight != m_pImages->Geometry().frame.uHeight )
        {
            Log( LogLevel::Error, std::format( "Real frame {}x{} outside a session of its size", frame.uWidth, frame.uHeight ) );
            return std::nullopt;
        }

        std::optional<Recording_t> oRecording = BeginRecording( 0.0f );
        if ( !oRecording )
            return std::nullopt;
        RecordIngest( *oRecording, frame.view );
        if ( m_pImages->HasUiMask() && m_ulRealFrames >= k_uLosslessFeatureSlots )
            RecordUiMask( *oRecording );
        const uint64_t ulSeqNo = Submit( *oRecording, m_sIngestKey );
        return Submission_t{ .ulSeqNo = ulSeqNo, .bComplete = !oRecording->bFailed };
    }

    std::optional<Submission_t> CLosslessGeneratorImpl::Generate( float flTimestamp, const GeneratorImage_t &output, const OutputHooks_t &hooks )
    {
        if ( m_ulRealFrames < k_uLosslessFrameSlots )
        {
            Log( LogLevel::Error, std::format( "Generate needs the last two real frames; {} added", m_ulRealFrames ) );
            return std::nullopt;
        }

        std::optional<Recording_t> oRecording = BeginRecording( flTimestamp );
        if ( !oRecording )
            return std::nullopt;
        if ( hooks.before )
            hooks.before( oRecording->cmd );
        RecordGenerate( *oRecording, output.view );
        if ( hooks.after )
            hooks.after( oRecording->cmd );
        const uint64_t ulSeqNo = Submit( *oRecording, m_sGenerateKey );
        return Submission_t{ .ulSeqNo = ulSeqNo, .bComplete = !oRecording->bFailed };
    }

    std::unique_ptr<CLosslessGenerator> CLosslessGenerator::Create( std::shared_ptr<const CDevice> pDevice, const LosslessSettings_t &settings, std::string &sError )
    {
        return CLosslessGeneratorImpl::Create( std::move( pDevice ), settings, sError );
    }
}
