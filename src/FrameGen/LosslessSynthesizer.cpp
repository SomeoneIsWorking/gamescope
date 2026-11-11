// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Barış Hamil

#include "LosslessSynthesizer.h"

#include <utility>

extern PFN_vkGetInstanceProcAddr g_pfn_vkGetInstanceProcAddr;

static LogScope lossless_log( "lossless" );

namespace gamescope::FrameGen
{
    CGamescopeQueue::CGamescopeQueue( LogScope &log )
        : m_Timestamps{ log }
    {
    }

    VkCommandBuffer CGamescopeQueue::Begin()
    {
        std::unique_ptr<CVulkanCmdBuffer> pCmdBuffer = g_device.commandBuffer();
        if ( !pCmdBuffer )
            return VK_NULL_HANDLE;
        VkCommandBuffer cmd = pCmdBuffer->rawBuffer();
        m_Recording[ cmd ] = std::move( pCmdBuffer );
        return cmd;
    }

    uint64_t CGamescopeQueue::Submit( VkCommandBuffer cmd )
    {
        auto it = m_Recording.find( cmd );
        std::unique_ptr<CVulkanCmdBuffer> pCmdBuffer = std::move( it->second );
        m_Recording.erase( it );
        return g_device.submit( std::move( pCmdBuffer ) );
    }

    uint64_t CGamescopeQueue::Completed()
    {
        return g_device.completedSeqNo();
    }

    void CGamescopeQueue::Wait( uint64_t ulSeqNo )
    {
        g_device.wait( ulSeqNo, false );
    }

    std::optional<uint32_t> CGamescopeQueue::BeginTiming( VkCommandBuffer cmd )
    {
        return m_Timestamps.Begin( Recording( cmd ) );
    }

    void CGamescopeQueue::EndTiming( VkCommandBuffer cmd, uint32_t uSlot, const std::string &sKey )
    {
        m_Timestamps.End( Recording( cmd ), uSlot, sKey );
    }

    void CGamescopeQueue::TimingSubmitted( uint64_t ulSeqNo )
    {
        m_Timestamps.Submitted( ulSeqNo );
    }

    CVulkanCmdBuffer *CGamescopeQueue::Recording( VkCommandBuffer cmd ) const
    {
        return m_Recording.at( cmd ).get();
    }

    std::unique_ptr<CLosslessSynthesizer> CLosslessSynthesizer::Create( const LosslessSettings_t &settings, std::string &sError )
    {
        std::unique_ptr<CLosslessSynthesizer> p{ new CLosslessSynthesizer() };
        p->m_pQueue = std::make_unique<CGamescopeQueue>( lossless_log );

        const framegen::DeviceDesc_t desc =
        {
            .pfnGetInstanceProcAddr = g_pfn_vkGetInstanceProcAddr,
            .instance = g_device.instance(),
            .physicalDevice = g_device.physDev(),
            .device = g_device.device(),
            .bFloat16 = g_device.supportsFp16(),
            .bStorageWriteWithoutFormat = g_device.supportsStorageImageWriteWithoutFormat(),
            .bStorageExtendedFormats = g_device.supportsStorageImageExtendedFormats(),
            .pQueue = p->m_pQueue.get(),
            .log = []( framegen::LogLevel eLevel, const std::string &sLine )
            {
                if ( eLevel == framegen::LogLevel::Error )
                    lossless_log.errorf( "%s", sLine.c_str() );
                else if ( eLevel == framegen::LogLevel::Info )
                    lossless_log.infof( "%s", sLine.c_str() );
                else
                    lossless_log.debugf( "%s", sLine.c_str() );
            },
        };
        std::shared_ptr<framegen::CDevice> pDevice = framegen::CDevice::Create( desc, sError );
        if ( !pDevice )
            return nullptr;
        p->m_pGenerator = framegen::CLosslessGenerator::Create( std::move( pDevice ), settings, sError );
        if ( !p->m_pGenerator )
            return nullptr;
        return p;
    }

    CLosslessSynthesizer::~CLosslessSynthesizer()
    {
        // The generator waits for its submissions before the textures they use go.
        m_pGenerator = nullptr;
    }

    void CLosslessSynthesizer::Hold( uint64_t ulSeqNo, const gamescope::Rc<CVulkanTexture> &pTexture )
    {
        const uint64_t ulCompleted = g_device.completedSeqNo();
        while ( !m_InFlight.empty() && m_InFlight.front().ulSeqNo <= ulCompleted )
            m_InFlight.pop_front();
        m_InFlight.push_back( { .ulSeqNo = ulSeqNo, .pTexture = pTexture } );
    }

    bool CLosslessSynthesizer::BeginSession( uint32_t uWidth, uint32_t uHeight )
    {
        m_pLastFrame = nullptr;
        return m_pGenerator->BeginSession( uWidth, uHeight );
    }

    bool CLosslessSynthesizer::AddRealFrame( const gamescope::Rc<CVulkanTexture> &pFrame )
    {
        // The UNORM view: raw values, as Lossless Scaling's capture sees them.
        const std::optional<framegen::Submission_t> oSubmission = m_pGenerator->AddRealFrame( { .view = pFrame->srgbView(), .uWidth = pFrame->width(), .uHeight = pFrame->height() } );
        if ( !oSubmission )
            return false;
        Hold( oSubmission->ulSeqNo, pFrame );
        m_pLastFrame = pFrame.get();
        return oSubmission->bComplete;
    }

    std::optional<uint64_t> CLosslessSynthesizer::Generate( const gamescope::Rc<CVulkanTexture> &pPrevious,
        const gamescope::Rc<CVulkanTexture> &pCurrent, float flTimestamp, const gamescope::Rc<CVulkanTexture> &pOutput )
    {
        if ( pCurrent.get() != m_pLastFrame || pPrevious == pCurrent )
        {
            lossless_log.errorf( "Generate needs the last two real frames; %lu added", (unsigned long)m_pGenerator->RealFrames() );
            return std::nullopt;
        }

        const framegen::OutputHooks_t hooks =
        {
            // Gamescope tracks this texture's layout: whole-image overwrite from GENERAL.
            .before = [&]( VkCommandBuffer cmd )
            {
                CVulkanCmdBuffer *pCmdBuffer = m_pQueue->Recording( cmd );
                pCmdBuffer->prepareDestImage( pOutput.get() );
                pCmdBuffer->insertBarrier();
            },
            .after = [&]( VkCommandBuffer cmd ) { m_pQueue->Recording( cmd )->markDirty( pOutput.get() ); },
        };
        // The UNORM view, which is the storage-capable one.
        const std::optional<framegen::Submission_t> oSubmission = m_pGenerator->Generate( flTimestamp, { .view = pOutput->srgbView(), .uWidth = pOutput->width(), .uHeight = pOutput->height() }, hooks );
        if ( !oSubmission )
            return std::nullopt;
        Hold( oSubmission->ulSeqNo, pOutput );
        if ( !oSubmission->bComplete )
            return std::nullopt;
        return oSubmission->ulSeqNo;
    }
}
