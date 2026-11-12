// Replays a recorded run's real frames and vblanks through CFramePacer at fixed 2x (ReplayCheck.h)
// and checks how long the plan stays off the half step and how often content steps a refresh off.
//
//   framegen_pacer_replay EVENTS MAX_OFFSET_RUN MAX_JUMPS
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "ReplayCheck.h"

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

    framegen::test::CReplayCheck check;
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
            check.Real( ulFrame, ulReadyUs * 1000, ulArrivalUs * 1000 );
        }
        else if ( kind == 'V' )
        {
            uint64_t ulVBlankUs = 0;
            words >> ulVBlankUs;
            check.VBlank( ulVBlankUs * 1000 );
        }
    }

    std::printf( "%s: longest run off the half step %ld real frames (at most %ld), "
                 "content steps a refresh off %ld (at most %ld)\n",
                 argv[ 1 ], check.LongestOffsetRun(), lMaxOffsetRun, check.Jumps(), lMaxJumps );
    return check.LongestOffsetRun() <= lMaxOffsetRun && check.Jumps() <= lMaxJumps ? 0 : 1;
}
