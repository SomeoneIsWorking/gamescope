// Drives CFramePacer at fixed 2x with real frames and vblanks as a host would, and measures what a
// player sees: how long the plan stays off the half step (no real frame on a vblank), and how often
// the shown content time steps half a refresh or more off the vblanks' step.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
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
            m_ReadyNs[ ulFrame ] = double( ulReadyNs );
            const IntervalPlan_t plan = m_Pacer.OnRealFrame( ulFrame, ulReadyNs, ulArrivalNs, k_ulRefreshNs );
            if ( plan.flTimestamps.empty() )
                return;
            const bool bOffset = std::abs( plan.flTimestamps.front() - 0.5f ) > k_flHalfStepTolerance;
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
                const double flStepErrorNs = ( flContentNs - m_oLastShown->first ) - double( ulVBlankNs - m_oLastShown->second );
                m_lJumps += std::abs( flStepErrorNs ) > double( k_ulRefreshNs ) / 2.0 ? 1 : 0;
            }
            m_oLastShown = std::pair{ flContentNs, ulVBlankNs };
        }

        long LongestOffsetRun() const { return m_lLongestOffsetRun; }
        long Jumps() const { return m_lJumps; }

    private:
        CFramePacer m_Pacer;
        std::map<uint64_t, double> m_ReadyNs;
        long m_lOffsetRun = 0;
        long m_lLongestOffsetRun = 0;
        long m_lJumps = 0;
        std::optional<std::pair<double, uint64_t>> m_oLastShown;
    };
}
