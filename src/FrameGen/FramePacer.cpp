#include "FramePacer.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <utility>

namespace gamescope::FrameGen
{
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
        m_Queue.clear();
    }

    double CFramePacer::GetStep() const
    {
        return m_Settings.flRatio > 1.0 ? 1.0 / m_Settings.flRatio : 1.0;
    }

    uint32_t CFramePacer::GetMaxGeneratedPerFrame() const
    {
        return uint32_t( std::ceil( 1.0 / GetStep() ) );
    }

    double CFramePacer::AdvanceTimeline( uint64_t ulTimeNs, double flMeanIntervalNs )
    {
        const double flPredictedNs = m_flTimelineNs + flMeanIntervalNs;
        const double flErrorNs = double( ulTimeNs ) - flPredictedNs;
        // A frame more than an interval off the prediction (a stall) restarts the timeline.
        m_flTimelineNs = std::abs( flErrorNs ) > flMeanIntervalNs ? double( ulTimeNs ) : flPredictedNs + flErrorNs * k_flTimelineCorrection;
        return m_flTimelineNs;
    }

    IntervalPlan_t CFramePacer::OnRealFrame( uint64_t ulFrame, uint64_t ulTimeNs )
    {
        m_Intervals.AddFrame( ulTimeNs );

        IntervalPlan_t plan;
        std::optional<double> oflMeanIntervalNs = m_Intervals.GetMeanIntervalNs();
        if ( ulFrame == 0 || !oflMeanIntervalNs )
        {
            m_flPhase = 0.0;
            m_flTimelineNs = double( ulTimeNs );
            m_Queue.push_back( PacedOutput_t{ .ulInterval = ulFrame, .ulRealFrame = ulFrame, .ulDesiredPresentNs = ulTimeNs } );
            return plan;
        }

        const double flTimelineNs = AdvanceTimeline( ulTimeNs, *oflMeanIntervalNs );
        const double flStep = GetStep();
        const uint32_t uMaxGenerated = GetMaxGeneratedPerFrame();
        while ( m_flPhase + flStep <= 1.0 + k_flRealTimestampTolerance )
        {
            m_flPhase += flStep;

            PacedOutput_t output =
            {
                .ulInterval = ulFrame,
                .ulRealFrame = ulFrame,
                .flTimestamp = float( m_flPhase ),
                .ulDesiredPresentNs = uint64_t( flTimelineNs + std::max( m_flPhase, 0.0 ) * *oflMeanIntervalNs ),
            };

            if ( m_flPhase <= k_flRealTimestampTolerance )
            {
                output.ulRealFrame = ulFrame - 1;
            }
            else if ( m_flPhase < 1.0 - k_flRealTimestampTolerance )
            {
                if ( plan.flTimestamps.size() >= uMaxGenerated )
                    continue;

                output.oGenerated = uint32_t( plan.flTimestamps.size() );
                plan.flTimestamps.push_back( output.flTimestamp );
            }

            m_Queue.push_back( output );
        }
        m_flPhase -= 1.0;

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
