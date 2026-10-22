#include "FramePacer.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <utility>

namespace gamescope::FrameGen
{
    const char *PacingStateName( PacingState eState )
    {
        switch ( eState )
        {
            case PacingState::Measuring: return "measuring";
            case PacingState::BelowFloor: return "below the floor";
            case PacingState::AtTarget: return "at the target";
            case PacingState::WithinTolerance: return "within the tolerance";
            case PacingState::Generating: return "generating";
        }
        return "unknown";
    }

    void CFrameIntervalEstimator::Reset()
    {
        m_oulLastFrameNs = std::nullopt;
        m_nIntervalCount = 0;
        m_nNextInterval = 0;
    }

    void CFrameIntervalEstimator::AddFrame( uint64_t ulTimeNs )
    {
        std::optional<uint64_t> oulLastFrameNs = std::exchange( m_oulLastFrameNs, ulTimeNs );
        if ( !oulLastFrameNs || ulTimeNs <= *oulLastFrameNs )
            return;

        const double flInterval = double( ulTimeNs - *oulLastFrameNs );
        if ( std::optional<double> oflMean = GetMeanIntervalNs() )
        {
            if ( flInterval > *oflMean * k_flFrameIntervalResetFactor || flInterval * k_flFrameIntervalResetFactor < *oflMean )
            {
                m_nIntervalCount = 0;
                m_nNextInterval = 0;
            }
        }

        m_flIntervals[ m_nNextInterval ] = flInterval;
        m_nNextInterval = ( m_nNextInterval + 1 ) % m_flIntervals.size();
        m_nIntervalCount = std::min( m_nIntervalCount + 1, m_flIntervals.size() );
    }

    std::optional<double> CFrameIntervalEstimator::GetMeanIntervalNs() const
    {
        if ( !m_nIntervalCount )
            return std::nullopt;

        return std::accumulate( m_flIntervals.begin(), m_flIntervals.begin() + m_nIntervalCount, 0.0 ) / double( m_nIntervalCount );
    }

    void CFramePacer::Configure( const PacingSettings_t &settings )
    {
        if ( settings == m_Settings )
            return;

        m_Settings = settings;
        Reset();
    }

    void CFramePacer::Reset()
    {
        m_Intervals.Reset();
        m_flPhase = 0.0;
        m_flTimelineNs = 0.0;
        m_nArrivalLags = 0;
        m_nNextArrivalLag = 0;
        m_bTimelineValid = false;
        m_oflLastClaimedVBlankNs = std::nullopt;
        m_obGenerating = std::nullopt;
        m_uDecisionFrames = 0;
        m_Queue.clear();
    }

    uint32_t CFramePacer::GetMaxGeneratedPerFrame() const
    {
        if ( m_Settings.eMode == PacingMode::Adaptive )
            return std::clamp( m_Settings.uMaxGenerated, 1u, k_uMaxGeneratedLimit );

        return std::clamp( uint32_t( std::ceil( m_Settings.flRatio ) ), 1u, k_uMaxGeneratedLimit );
    }

    double CFramePacer::GetTargetFps( uint64_t ulRefreshNs ) const
    {
        const double flRefreshFps = ulRefreshNs ? 1.0e9 / double( ulRefreshNs ) : 0.0;
        if ( m_Settings.flTargetFps <= 0.0 )
            return flRefreshFps;
        return flRefreshFps > 0.0 ? std::min( m_Settings.flTargetFps, flRefreshFps ) : m_Settings.flTargetFps;
    }

    bool CFramePacer::DecideGenerating( double flBaseFps, double flTargetFps )
    {
        const double flStopFps = flTargetFps - m_Settings.flToleranceFps;
        const double flStartFps = flStopFps - m_Settings.flToleranceFps * k_flToleranceHysteresis;
        if ( !m_obGenerating )
        {
            m_obGenerating = flBaseFps < flStopFps;
            m_uDecisionFrames = 0;
            return *m_obGenerating;
        }

        const bool bWantsOther = *m_obGenerating ? flBaseFps >= flStopFps : flBaseFps < flStartFps;
        m_uDecisionFrames = bWantsOther ? m_uDecisionFrames + 1 : 0;
        if ( m_uDecisionFrames >= k_uDecisionHoldFrames )
        {
            m_obGenerating = !*m_obGenerating;
            m_uDecisionFrames = 0;
        }
        return *m_obGenerating;
    }

    bool CFramePacer::AdvanceTimeline( uint64_t ulTimeNs, double flMeanIntervalNs )
    {
        const double flPredictedNs = m_flTimelineNs + flMeanIntervalNs;
        const double flErrorNs = double( ulTimeNs ) - flPredictedNs;
        // A frame more than an interval off the prediction (a stall) restarts the timeline.
        if ( std::abs( flErrorNs ) > flMeanIntervalNs )
            return false;

        m_flTimelineNs = flPredictedNs + flErrorNs * k_flTimelineCorrection;
        m_flArrivalLagsNs[ m_nNextArrivalLag ] = std::max( double( ulTimeNs ) - m_flTimelineNs, 0.0 );
        m_nNextArrivalLag = ( m_nNextArrivalLag + 1 ) % m_flArrivalLagsNs.size();
        m_nArrivalLags = std::min( m_nArrivalLags + 1, m_flArrivalLagsNs.size() );
        return true;
    }

    double CFramePacer::GetArrivalLagNs() const
    {
        if ( !m_nArrivalLags )
            return 0.0;
        return *std::max_element( m_flArrivalLagsNs.begin(), m_flArrivalLagsNs.begin() + m_nArrivalLags );
    }

    bool CFramePacer::ClaimVBlank( uint64_t ulPresentNs, uint64_t ulRefreshNs )
    {
        if ( !ulRefreshNs || !m_ulLastVBlankNs )
            return true;

        // The vblank OnVBlank shows it from: the nearest one.
        const double flRefreshNs = double( ulRefreshNs );
        const double flVBlankNs = double( m_ulLastVBlankNs ) + std::round( ( double( ulPresentNs ) - double( m_ulLastVBlankNs ) ) / flRefreshNs ) * flRefreshNs;
        if ( m_oflLastClaimedVBlankNs && flVBlankNs < *m_oflLastClaimedVBlankNs + flRefreshNs / 2.0 )
            return false;

        m_oflLastClaimedVBlankNs = flVBlankNs;
        return true;
    }

    void CFramePacer::ShowRealFrame( uint64_t ulFrame, uint64_t ulTimeNs )
    {
        // A real frame shown as it comes supersedes everything still queued.
        m_ulDroppedOutputs += m_Queue.size();
        m_Queue.clear();
        m_oflLastClaimedVBlankNs = std::nullopt;
        m_Queue.push_back( PacedOutput_t{ .ulInterval = ulFrame, .ulRealFrame = ulFrame, .ulDesiredPresentNs = ulTimeNs } );
    }

    void CFramePacer::StartTimeline( uint64_t ulFrame, uint64_t ulTimeNs, uint64_t ulRefreshNs )
    {
        m_flPhase = 0.0;
        m_flTimelineNs = double( ulTimeNs );
        m_nArrivalLags = 0;
        m_nNextArrivalLag = 0;
        m_bTimelineValid = true;
        m_oflLastClaimedVBlankNs = std::nullopt;
        ClaimVBlank( ulTimeNs, ulRefreshNs );
        m_Queue.push_back( PacedOutput_t{ .ulInterval = ulFrame, .ulRealFrame = ulFrame, .ulDesiredPresentNs = ulTimeNs } );
    }

    void CFramePacer::QueueOutput( IntervalPlan_t &plan, uint64_t ulFrame, double flTimestamp, uint64_t ulDesiredPresentNs )
    {
        PacedOutput_t output =
        {
            .ulInterval = ulFrame,
            .ulRealFrame = ulFrame,
            .flTimestamp = float( flTimestamp ),
            .ulDesiredPresentNs = ulDesiredPresentNs,
        };

        if ( flTimestamp <= k_flRealTimestampTolerance )
        {
            output.ulRealFrame = ulFrame - 1;
        }
        else if ( flTimestamp < 1.0 - k_flRealTimestampTolerance )
        {
            output.oGenerated = uint32_t( plan.flTimestamps.size() );
            plan.flTimestamps.push_back( output.flTimestamp );
        }

        m_Queue.push_back( output );
    }

    void CFramePacer::PlanByPhase( IntervalPlan_t &plan, uint64_t ulFrame, double flStep, double flMeanIntervalNs, uint64_t ulRefreshNs )
    {
        // Content at timestamp t shows t real intervals after this frame's place on the
        // timeline, later by how late recent frames arrived after it and by half a refresh, so
        // no output's vblank comes before its real frame.
        const double flShowNs = m_flTimelineNs + GetArrivalLagNs() + double( ulRefreshNs ) / 2.0;

        // A carried phase can leave every output of the interval between real frames; past the
        // maximum, realign so the last output is the real frame.
        uint32_t uGenerated = 0;
        for ( double flPhase = m_flPhase + flStep; flPhase <= 1.0 + k_flRealTimestampTolerance; flPhase += flStep )
        {
            if ( flPhase > k_flRealTimestampTolerance && flPhase < 1.0 - k_flRealTimestampTolerance )
                uGenerated++;
        }
        if ( uGenerated > GetMaxGeneratedPerFrame() )
            m_flPhase = 1.0 - double( GetMaxGeneratedPerFrame() + 1 ) * flStep;

        while ( m_flPhase + flStep <= 1.0 + k_flRealTimestampTolerance )
        {
            m_flPhase += flStep;
            const uint64_t ulPresentNs = uint64_t( flShowNs + std::max( m_flPhase, 0.0 ) * flMeanIntervalNs );
            if ( ClaimVBlank( ulPresentNs, ulRefreshNs ) )
                QueueOutput( plan, ulFrame, m_flPhase, ulPresentNs );
        }
        m_flPhase -= 1.0;
    }

    IntervalPlan_t CFramePacer::OnRealFrame( uint64_t ulFrame, uint64_t ulTimeNs, uint64_t ulRefreshNs )
    {
        m_Intervals.AddFrame( ulTimeNs );

        IntervalPlan_t plan;
        std::optional<double> oflMeanIntervalNs = m_Intervals.GetMeanIntervalNs();
        if ( oflMeanIntervalNs )
            plan.flBaseFps = 1.0e9 / *oflMeanIntervalNs;

        double flStep = 1.0;
        if ( m_Settings.eMode == PacingMode::Adaptive )
        {
            plan.flTargetFps = GetTargetFps( ulRefreshNs );
            if ( m_Intervals.IsFull() && plan.flTargetFps > 0.0 )
            {
                const bool bGenerating = DecideGenerating( plan.flBaseFps, plan.flTargetFps );
                flStep = std::max( plan.flBaseFps / plan.flTargetFps, 1.0 / double( GetMaxGeneratedPerFrame() + 1 ) );
                plan.eState = bGenerating ? PacingState::Generating : PacingState::WithinTolerance;
            }
        }
        else if ( oflMeanIntervalNs )
        {
            plan.flTargetFps = plan.flBaseFps * m_Settings.flRatio;
            flStep = m_Settings.flRatio > 1.0 ? 1.0 / m_Settings.flRatio : 1.0;
            plan.eState = PacingState::Generating;
        }

        if ( plan.eState == PacingState::Generating && flStep >= 1.0 )
            plan.eState = PacingState::AtTarget;
        if ( plan.eState != PacingState::Measuring && plan.flBaseFps < k_flMinBaseFps )
            plan.eState = PacingState::BelowFloor;

        if ( plan.eState != PacingState::Generating )
        {
            m_bTimelineValid = false;
            ShowRealFrame( ulFrame, ulTimeNs );
            return plan;
        }

        // Generation (re)starts from this real frame: there is no interval to fill yet.
        if ( !m_bTimelineValid || ulFrame == 0 || !AdvanceTimeline( ulTimeNs, *oflMeanIntervalNs ) )
        {
            StartTimeline( ulFrame, ulTimeNs, ulRefreshNs );
            return plan;
        }

        PlanByPhase( plan, ulFrame, flStep, *oflMeanIntervalNs, ulRefreshNs );
        return plan;
    }

    void CFramePacer::DropOlderThan( uint64_t ulOldestRealFrame, uint64_t ulOldestInterval )
    {
        const size_t nBefore = m_Queue.size();
        std::erase_if( m_Queue, [&]( const PacedOutput_t &output )
        {
            return output.oGenerated ? output.ulInterval < ulOldestInterval : output.ulRealFrame < ulOldestRealFrame;
        } );
        m_ulDroppedOutputs += nBefore - m_Queue.size();
    }

    std::optional<PacedOutput_t> CFramePacer::OnVBlank( uint64_t ulVBlankNs, uint64_t ulRefreshNs,
        const std::function<bool( const PacedOutput_t & )> &bReady )
    {
        m_ulLastVBlankNs = ulVBlankNs;

        // Skip outputs more than a refresh late while a later one is waiting.
        while ( m_Queue.size() > 1 && m_Queue.front().ulDesiredPresentNs + ulRefreshNs < ulVBlankNs )
        {
            m_Queue.pop_front();
            m_ulDroppedOutputs++;
        }

        if ( m_Queue.empty() )
            return std::nullopt;

        const PacedOutput_t &head = m_Queue.front();
        if ( head.ulDesiredPresentNs > ulVBlankNs + ulRefreshNs / 2 || !bReady( head ) )
            return std::nullopt;

        PacedOutput_t output = head;
        m_Queue.pop_front();
        return output;
    }
}
