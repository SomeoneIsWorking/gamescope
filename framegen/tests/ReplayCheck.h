// Drives CFramePacer at fixed 2x with real frames and vblanks as a host would, and measures what a
// player sees: how long the plan stays off the half step (no real frame on a vblank) later than
// k_uTimelineStepFrames after a step of the ready times, and how often the shown content time steps
// half a refresh or more off the vblanks' step between vblanks seen on the grid.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <array>
#include <map>
#include <optional>
#include <utility>

#include "framegen/FramePacer.h"

namespace framegen::test
{
    inline constexpr uint64_t k_ulRefreshNs = 16666667;
    // An in-between of a 2x plan this far from the half step means real frames miss vblanks.
    inline constexpr double k_flHalfStepTolerance = 0.03;

    class CReplayCheck
    {
    public:
        CReplayCheck()
        {
            m_Pacer.Configure( PacingSettings_t{ .eMode = PacingMode::Fixed, .flRatio = 2.0 } );
        }

        // Real frame ulFrame, ready at ulReadyNs and arriving at ulArrivalNs; frame 0 starts a session.
        void Real( uint64_t ulFrame, uint64_t ulReadyNs, uint64_t ulArrivalNs )
        {
            // As a host drives it: a new session from frame 0, images older than two frames gone.
            if ( ulFrame == 0 )
            {
                m_Pacer.Reset();
                m_ReadyNs.clear();
                m_oLastShown.reset();
            }
            else
            {
                const uint64_t ulOldest = ulFrame > 2 ? ulFrame - 2 : 0;
                m_Pacer.DropOlderThan( ulOldest, ulOldest );
            }
            NoteStep( ulFrame, ulReadyNs );
            m_ReadyNs[ ulFrame ] = double( ulReadyNs );
            const IntervalPlan_t plan = m_Pacer.OnRealFrame( ulFrame, ulReadyNs, ulArrivalNs, k_ulRefreshNs );
            if ( plan.flTimestamps.empty() )
                return;
            const bool bAfterStep = m_oulLastStep && ulFrame <= *m_oulLastStep + k_uTimelineStepFrames;
            const double flHalfStepError = std::abs( double( plan.flTimestamps.front() ) - 0.5 );
            const bool bOffset = !bAfterStep && flHalfStepError > k_flHalfStepTolerance;
            if ( !bAfterStep )
                m_flWorstHalfStepError = std::max( m_flWorstHalfStepError, flHalfStepError );
            m_lOffsetRun = bOffset ? m_lOffsetRun + 1 : 0;
            m_lLongestOffsetRun = std::max( m_lLongestOffsetRun, m_lOffsetRun );
        }

        void VBlank( uint64_t ulVBlankNs )
        {
            const std::optional<PacedOutput_t> shown =
                m_Pacer.OnVBlank( ulVBlankNs, k_ulRefreshNs, []( const PacedOutput_t & ) { return true; } );
            if ( !shown )
                return;
            const auto previous = m_ReadyNs.find( shown->ulInterval - 1 );
            const auto current = m_ReadyNs.find( shown->ulInterval );
            if ( current == m_ReadyNs.end() )
                return;
            const double flContentNs = shown->oGenerated && previous != m_ReadyNs.end()
                ? previous->second + double( shown->flTimestamp ) * ( current->second - previous->second )
                : m_ReadyNs[ shown->ulRealFrame ];
            if ( m_oLastShown )
            {
                const double flVBlankStepNs = double( ulVBlankNs - m_oLastShown->second );
                const double flStepErrorNs = ( flContentNs - m_oLastShown->first ) - flVBlankStepNs;
                // Between vblanks seen off the grid (a late host present) the vblank step itself is wrong.
                const double flOffGridNs = std::abs( flVBlankStepNs - std::round( flVBlankStepNs / double( k_ulRefreshNs ) ) * double( k_ulRefreshNs ) );
                const bool bOnGrid = flOffGridNs <= double( k_flRealTimestampTolerance ) * double( k_ulRefreshNs );
                m_lJumps += bOnGrid && std::abs( flStepErrorNs ) > double( k_ulRefreshNs ) / 2.0 ? 1 : 0;
            }
            m_oLastShown = std::pair{ flContentNs, ulVBlankNs };
        }

        // Counts from here on, once generation has settled.
        void ClearCounts()
        {
            m_lLongestOffsetRun = 0;
            m_lJumps = 0;
            m_flWorstHalfStepError = 0.0;
        }

        long LongestOffsetRun() const { return m_lLongestOffsetRun; }
        long Jumps() const { return m_lJumps; }
        // The first in-between's largest distance from the half step, past k_uTimelineStepFrames of a step.
        double WorstHalfStepError() const { return m_flWorstHalfStepError; }

    private:
        // A ready interval off the median of the recent ones by more than the real-frame tolerance
        // is a step of the ready times at ulFrame.
        void NoteStep( uint64_t ulFrame, uint64_t ulReadyNs )
        {
            const auto previous = m_ReadyNs.find( ulFrame - 1 );
            if ( ulFrame == 0 || previous == m_ReadyNs.end() )
            {
                m_nRecent = 0;
                m_nNextRecent = 0;
                return;
            }
            const double flIntervalNs = double( ulReadyNs ) - previous->second;
            if ( m_nRecent == m_flRecentNs.size() )
            {
                std::array<double, k_nFrameIntervalHistory> sorted = m_flRecentNs;
                std::nth_element( sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end() );
                const double flMedianNs = sorted[ sorted.size() / 2 ];
                if ( std::abs( flIntervalNs - flMedianNs ) > double( k_flRealTimestampTolerance ) * flMedianNs )
                    m_oulLastStep = ulFrame;
            }
            m_flRecentNs[ m_nNextRecent ] = flIntervalNs;
            m_nNextRecent = ( m_nNextRecent + 1 ) % m_flRecentNs.size();
            m_nRecent = std::min( m_nRecent + 1, m_flRecentNs.size() );
        }

        CFramePacer m_Pacer;
        std::array<double, k_nFrameIntervalHistory> m_flRecentNs{};
        size_t m_nRecent = 0;
        size_t m_nNextRecent = 0;
        std::optional<uint64_t> m_oulLastStep;
        std::map<uint64_t, double> m_ReadyNs;
        long m_lOffsetRun = 0;
        long m_lLongestOffsetRun = 0;
        long m_lJumps = 0;
        double m_flWorstHalfStepError = 0.0;
        std::optional<std::pair<double, uint64_t>> m_oLastShown;
    };
}
