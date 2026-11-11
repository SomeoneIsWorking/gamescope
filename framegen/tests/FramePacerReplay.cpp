// Replays a recorded run's real frames and vblanks through CFramePacer at fixed 2x and checks what
// a player would see: how long the plan stays off the half step (no real frame on a vblank), and
// how often the shown content time steps a refresh more or less than the vblanks do.
//
//   framegen_pacer_replay EVENTS MAX_OFFSET_RUN MAX_JUMPS
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>

#include "framegen/FramePacer.h"

namespace
{
    constexpr uint64_t k_ulRefreshNs = 16666667;
    // An in-between of a 2x plan this far from the half step means real frames miss vblanks.
    constexpr double k_flHalfStepTolerance = 0.03;
}

int main( int argc, char **argv )
{
    if ( argc != 4 )
    {
        std::fprintf( stderr, "usage: framegen_pacer_replay EVENTS MAX_OFFSET_RUN MAX_JUMPS\n" );
        return 2;
    }
    std::ifstream events{ argv[ 1 ] };
    if ( !events )
    {
        std::fprintf( stderr, "cannot read %s\n", argv[ 1 ] );
        return 2;
    }
    const long lMaxOffsetRun = std::stol( argv[ 2 ] );
    const long lMaxJumps = std::stol( argv[ 3 ] );

    framegen::CFramePacer pacer;
    pacer.Configure( framegen::PacingSettings_t{ .eMode = framegen::PacingMode::Fixed, .flRatio = 2.0 } );
    std::map<uint64_t, double> readyNs;
    long lOffsetRun = 0;
    long lLongestOffsetRun = 0;
    long lJumps = 0;
    std::optional<std::pair<double, uint64_t>> oLastShown;

    std::string line;
    while ( std::getline( events, line ) )
    {
        std::istringstream words{ line };
        char kind = 0;
        words >> kind;
        if ( kind == 'R' )
        {
            uint64_t ulFrame = 0;
            uint64_t ulReadyUs = 0;
            uint64_t ulArrivalUs = 0;
            words >> ulFrame >> ulReadyUs >> ulArrivalUs;
            // As a host drives it: a new session from frame 0, images older than two frames gone.
            if ( ulFrame == 0 )
            {
                pacer.Reset();
                readyNs.clear();
                oLastShown.reset();
            }
            else
            {
                const uint64_t ulOldest = ulFrame > 2 ? ulFrame - 2 : 0;
                pacer.DropOlderThan( ulOldest, ulOldest );
            }
            readyNs[ ulFrame ] = double( ulReadyUs ) * 1000.0;
            const framegen::IntervalPlan_t plan = pacer.OnRealFrame( ulFrame, ulReadyUs * 1000, ulArrivalUs * 1000, k_ulRefreshNs );
            if ( plan.flTimestamps.empty() )
                continue;
            const bool bOffset = std::abs( plan.flTimestamps.front() - 0.5f ) > k_flHalfStepTolerance;
            lOffsetRun = bOffset ? lOffsetRun + 1 : 0;
            lLongestOffsetRun = std::max( lLongestOffsetRun, lOffsetRun );
        }
        else if ( kind == 'V' )
        {
            uint64_t ulVBlankUs = 0;
            words >> ulVBlankUs;
            const uint64_t ulVBlankNs = ulVBlankUs * 1000;
            const std::optional<framegen::PacedOutput_t> shown =
                pacer.OnVBlank( ulVBlankNs, k_ulRefreshNs, []( const framegen::PacedOutput_t & ) { return true; } );
            if ( !shown )
                continue;
            const auto previous = readyNs.find( shown->ulInterval - 1 );
            const auto current = readyNs.find( shown->ulInterval );
            if ( current == readyNs.end() )
                continue;
            const double flContentNs = shown->oGenerated && previous != readyNs.end()
                ? previous->second + double( shown->flTimestamp ) * ( current->second - previous->second )
                : readyNs[ shown->ulRealFrame ];
            if ( oLastShown )
            {
                const double flStepErrorNs = ( flContentNs - oLastShown->first ) - double( ulVBlankNs - oLastShown->second );
                lJumps += std::abs( flStepErrorNs ) > double( k_ulRefreshNs ) / 2.0 ? 1 : 0;
            }
            oLastShown = std::pair{ flContentNs, ulVBlankNs };
        }
    }

    std::printf( "%s: longest run off the half step %ld real frames (at most %ld), "
                 "content steps a refresh off %ld (at most %ld)\n",
                 argv[ 1 ], lLongestOffsetRun, lMaxOffsetRun, lJumps, lMaxJumps );
    return lLongestOffsetRun <= lMaxOffsetRun && lJumps <= lMaxJumps ? 0 : 1;
}
