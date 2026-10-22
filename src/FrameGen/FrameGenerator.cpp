#include "FrameGenerator.h"

#include "wlserver.hpp"
#include "rendervulkan.hpp"
#include "steamcompmgr.hpp"
#include "steamcompmgr_shared.hpp"
#include "commit.h"
#include "log.hpp"

#include "LinearBlendSynthesizer.h"
#include "TextureComposite.h"

#include <algorithm>
#include <drm_fourcc.h>

static LogScope framegen_log( "framegen" );

namespace gamescope::FrameGen
{
    static constexpr uint64_t k_ulStatsPeriodNs = 2'000'000'000ul;

    CFrameGenerator &CFrameGenerator::Get()
    {
        // Never destroyed, like g_device, so no Vulkan teardown runs in exit handlers.
        static CFrameGenerator *s_pGenerator = new CFrameGenerator();
        return *s_pGenerator;
    }

    CFrameGenerator::CFrameGenerator()
        : m_pSynthesizer{ std::make_unique<CLinearBlendSynthesizer>() }
    {
    }

    // 8-bit SDR content only.
    static bool IsSupportedCommit( const commit_t *pCommit, GamescopeAppTextureColorspace eColorspace )
    {
        if ( !pCommit->vulkanTex || pCommit->vulkanTex->isYcbcr() || ColorspaceIsHDR( eColorspace ) )
            return false;

        switch ( pCommit->vulkanTex->format() )
        {
            case VK_FORMAT_B8G8R8A8_UNORM:
            case VK_FORMAT_B8G8R8A8_SRGB:
            case VK_FORMAT_R8G8B8A8_UNORM:
            case VK_FORMAT_R8G8B8A8_SRGB:
                return true;
            default:
                return false;
        }
    }

    void CFrameGenerator::SetSettings( const Settings_t &settings )
    {
        if ( settings == m_Settings )
            return;

        framegen_log.infof( "Frame generation %s, ratio %.3f, dll '%s'",
            settings.bEnabled ? "on" : "off", settings.pacing.flRatio, settings.sDllPath.c_str() );

        m_Settings = settings;
        EndSession();
    }

    bool CFrameGenerator::BeginSession( steamcompmgr_win_t *pWindow, uint32_t uWidth, uint32_t uHeight )
    {
        EndSession();

        m_Pacer.Configure( m_Settings.pacing );
        m_Pacer.Reset();
        m_uMaxGenerated = m_Pacer.GetMaxGeneratedPerFrame();

        CVulkanTexture::createFlags flags;
        flags.bStorage = true;
        flags.bSampled = true;

        auto createTexture = [&]() -> gamescope::Rc<CVulkanTexture>
        {
            gamescope::Rc<CVulkanTexture> pTexture = new CVulkanTexture();
            if ( !pTexture->BInit( uWidth, uHeight, 1u, DRM_FORMAT_ABGR8888, flags ) )
                return nullptr;
            return pTexture;
        };

        for ( gamescope::Rc<CVulkanTexture> &pSource : m_pSources )
        {
            pSource = createTexture();
            if ( !pSource )
                return false;
        }

        // Two sets, so one interval's images stay on screen while the next is generated.
        m_pDestinations.resize( 2 * m_uMaxGenerated );
        for ( gamescope::Rc<CVulkanTexture> &pDestination : m_pDestinations )
        {
            pDestination = createTexture();
            if ( !pDestination )
                return false;
        }

        m_pWindow = pWindow;
        m_ulWindowSeq = pWindow->seq;
        m_uWidth = uWidth;
        m_uHeight = uHeight;
        framegen_log.infof( "Generating for window 0x%lx at %ux%u, up to %u images per real frame",
            (unsigned long)pWindow->id(), uWidth, uHeight, m_uMaxGenerated );
        return true;
    }

    void CFrameGenerator::EndSession()
    {
        m_pWindow = nullptr;
        m_ulWindowSeq = 0;
        m_uWidth = 0;
        m_uHeight = 0;
        m_pSources = {};
        m_pDestinations.clear();
        m_ulRealFrames = 0;
        m_ulLastCommitId = 0;
        m_Intervals.clear();
        m_Pacer.Reset();
        if ( m_oDisplay )
        {
            m_oDisplay = std::nullopt;
            m_ulDisplaySerial++;
        }
    }

    void CFrameGenerator::Update( steamcompmgr_win_t *pWindow, const gamescope::Rc<commit_t> &pCommit, uint64_t ulNowNs )
    {
        if ( !m_Settings.bEnabled )
            return;

        if ( !pWindow || pCommit == nullptr )
        {
            EndSession();
            return;
        }

        const bool bSameWindow = m_pWindow == pWindow && m_ulWindowSeq == pWindow->seq;
        if ( bSameWindow && pCommit->commitID == m_ulLastCommitId )
            return;

        const GamescopeAppTextureColorspace eColorspace = pCommit->colorspace();
        if ( !IsSupportedCommit( pCommit.get(), eColorspace ) )
        {
            if ( m_pWindow )
                framegen_log.infof( "Window 0x%lx shows content frame generation does not support", (unsigned long)pWindow->id() );
            EndSession();
            return;
        }

        const uint32_t uWidth = pCommit->vulkanTex->width();
        const uint32_t uHeight = pCommit->vulkanTex->height();
        if ( !bSameWindow || uWidth != m_uWidth || uHeight != m_uHeight )
        {
            if ( !BeginSession( pWindow, uWidth, uHeight ) )
            {
                framegen_log.errorf( "Failed to start frame generation at %ux%u", uWidth, uHeight );
                EndSession();
                return;
            }
        }

        // The copies hold the raw values, which decode like any sRGB layer.
        m_eColorspace = eColorspace == GAMESCOPE_APP_TEXTURE_COLORSPACE_LINEAR ? GAMESCOPE_APP_TEXTURE_COLORSPACE_SRGB : eColorspace;
        m_ulLastCommitId = pCommit->commitID;
        if ( !Capture( pCommit, ulNowNs ) )
            EndSession();
    }

    bool CFrameGenerator::Capture( const gamescope::Rc<commit_t> &pCommit, uint64_t ulNowNs )
    {
        const uint64_t ulFrame = m_ulRealFrames;

        // Images that the copy and the new interval overwrite.
        if ( ulFrame >= 1 )
        {
            m_Pacer.DropOlderThan( ulFrame - 1, ulFrame - 1 );
            if ( m_oDisplay && ( m_oDisplay->oGenerated ? m_oDisplay->ulInterval : m_oDisplay->ulRealFrame ) + 1 < ulFrame )
            {
                m_oDisplay = PacedOutput_t{ .ulInterval = ulFrame - 1, .ulRealFrame = ulFrame - 1 };
                m_ulDisplaySerial++;
            }
        }

        const gamescope::Rc<CVulkanTexture> &pCurrent = m_pSources[ ulFrame % 2 ];
        const gamescope::Rc<CVulkanTexture> &pPrevious = m_pSources[ ( ulFrame + 1 ) % 2 ];

        FrameInfo_t frameInfo = {};
        AddPassthroughLayer( frameInfo, pCommit->vulkanTex, 1.0f );
        if ( !CompositeIntoTexture( frameInfo, pCurrent, g_device.commandBuffer() ) )
        {
            framegen_log.errorf( "Failed to copy frame %lu", (unsigned long)ulFrame );
            return false;
        }

        IntervalPlan_t plan = m_Pacer.OnRealFrame( ulFrame, ulNowNs );
        Interval_t interval{ .ulFrame = ulFrame };
        const size_t uFirstDestination = ( ulFrame % 2 ) * m_uMaxGenerated;
        for ( size_t i = 0; i < plan.flTimestamps.size(); i++ )
        {
            std::optional<uint64_t> oSequence = m_pSynthesizer->Generate( pPrevious, pCurrent, plan.flTimestamps[ i ], m_pDestinations[ uFirstDestination + i ] );
            if ( !oSequence )
            {
                framegen_log.errorf( "Failed to generate an image for frame %lu", (unsigned long)ulFrame );
                return false;
            }
            interval.ulSequences.push_back( *oSequence );
        }

        m_Intervals.push_back( std::move( interval ) );
        while ( m_Intervals.size() > 2 )
            m_Intervals.pop_front();

        m_ulRealFrames++;
        m_ulStatsReal++;

        std::string sTimestamps;
        for ( float flTimestamp : plan.flTimestamps )
            sTimestamps += std::to_string( flTimestamp ) + " ";
        framegen_log.debugf( "real %lu at %.3f ms: generate [ %s]", (unsigned long)ulFrame, ulNowNs / 1.0e6, sTimestamps.c_str() );
        return true;
    }

    bool CFrameGenerator::IsReady( const PacedOutput_t &output ) const
    {
        if ( !output.oGenerated )
            return true;

        auto it = std::find_if( m_Intervals.begin(), m_Intervals.end(), [&]( const Interval_t &interval ) { return interval.ulFrame == output.ulInterval; } );
        if ( it == m_Intervals.end() || *output.oGenerated >= it->ulSequences.size() )
            return false;

        return g_device.completedSeqNo() >= it->ulSequences[ *output.oGenerated ];
    }

    gamescope::Rc<CVulkanTexture> CFrameGenerator::GetTexture( const PacedOutput_t &output ) const
    {
        if ( output.oGenerated )
            return m_pDestinations[ ( output.ulInterval % 2 ) * m_uMaxGenerated + *output.oGenerated ];
        return m_pSources[ output.ulRealFrame % 2 ];
    }

    bool CFrameGenerator::OnVBlank( uint64_t ulNowNs, uint64_t ulRefreshNs )
    {
        if ( !m_pWindow )
            return false;

        std::optional<PacedOutput_t> oOutput = m_Pacer.OnVBlank( ulNowNs, ulRefreshNs,
            [this]( const PacedOutput_t &output ) { return IsReady( output ); } );

        LogStats( ulNowNs );

        if ( !oOutput )
        {
            if ( const PacedOutput_t *pNext = m_Pacer.PeekNext() )
                framegen_log.debugf( "vblank %.3f ms: hold, next due %.3f ms, ready %d", ulNowNs / 1.0e6, pNext->ulDesiredPresentNs / 1.0e6, IsReady( *pNext ) );
            return false;
        }

        m_oDisplay = oOutput;
        m_ulDisplaySerial++;
        m_ulStatsShown++;
        if ( oOutput->oGenerated )
            m_ulStatsShownGenerated++;

        framegen_log.debugf( "vblank %.3f ms: show %s %lu t=%.3f (interval %lu)", ulNowNs / 1.0e6,
            oOutput->oGenerated ? "generated" : "real", (unsigned long)( oOutput->oGenerated ? *oOutput->oGenerated : oOutput->ulRealFrame ),
            oOutput->flTimestamp, (unsigned long)oOutput->ulInterval );
        return true;
    }

    void CFrameGenerator::LogStats( uint64_t ulNowNs )
    {
        if ( !m_ulStatsStartNs )
        {
            m_ulStatsStartNs = ulNowNs;
            m_ulStatsDroppedBase = m_Pacer.GetDroppedOutputs();
            return;
        }

        const uint64_t ulElapsedNs = ulNowNs - m_ulStatsStartNs;
        if ( ulElapsedNs < k_ulStatsPeriodNs )
            return;

        const double flSeconds = ulElapsedNs / 1.0e9;
        framegen_log.infof( "%.1f real/s, %.1f shown/s (%.1f generated/s), %lu dropped",
            m_ulStatsReal / flSeconds, m_ulStatsShown / flSeconds, m_ulStatsShownGenerated / flSeconds,
            (unsigned long)( m_Pacer.GetDroppedOutputs() - m_ulStatsDroppedBase ) );

        m_ulStatsStartNs = ulNowNs;
        m_ulStatsReal = 0;
        m_ulStatsShown = 0;
        m_ulStatsShownGenerated = 0;
        m_ulStatsDroppedBase = m_Pacer.GetDroppedOutputs();
    }

    std::optional<DisplayImage_t> CFrameGenerator::GetDisplayImage( const steamcompmgr_win_t *pWindow ) const
    {
        if ( !m_oDisplay || !pWindow || pWindow != m_pWindow || pWindow->seq != m_ulWindowSeq )
            return std::nullopt;

        return DisplayImage_t{ .pTexture = GetTexture( *m_oDisplay ), .eColorspace = m_eColorspace };
    }
}
