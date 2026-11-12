// 30 fps at fixed 2x on 60 Hz, at every arrival phase against the vblanks, with a lasting step of
// the arrivals alone or of the ready times too, with and without arrival jitter, and a single late
// frame. A step re-aligns once: real frames back on vblanks within k_uTimelineStepFrames of it, and
// content steps a refresh off only at the re-align (and at a ready step, the step itself). A
// single late frame moves only itself. Arrival jitter across a vblank's edge shows its frame a
// refresh late whatever the pacer plans, so jittered cases check the runs alone.
//
//   framegen_pacer_steps
#include <cstdint>
#include <cstdio>
#include <random>

#include "ReplayCheck.h"

namespace
{
    constexpr int k_nPhases = 16;
    constexpr int k_nRealFrames = 600;
    constexpr int k_nStepFrame = 300;
    // Generation starts once the rate history is full; counting starts well after.
    constexpr int k_nSettledFrame = 100;
    constexpr double k_flIntervalNs = 2.0 * double( framegen::test::k_ulRefreshNs );
    constexpr uint64_t k_ulStartNs = 1000000000;
    constexpr uint64_t k_ulTransportNs = 1300000;
    constexpr int64_t k_lStepNs = 7000000;

    enum class Kind
    {
        Arrivals,
        Ready,
        SingleLate,
    };

    struct Case
    {
        const char *pszName;
        Kind eKind;
        int64_t lStepNs;
        uint64_t ulJitterNs;
        long lMaxOffsetRun;
        // Negative: not checked.
        long lMaxJumps;
    };

    constexpr long k_lStepRun = long( framegen::k_uTimelineStepFrames );

    constexpr Case k_Cases[] = {
        { "arrivals +7 ms", Kind::Arrivals, k_lStepNs, 0, k_lStepRun, 1 },
        { "arrivals -7 ms", Kind::Arrivals, -k_lStepNs, 0, k_lStepRun, 1 },
        { "arrivals +7 ms, 1 ms jitter", Kind::Arrivals, k_lStepNs, 1000000, k_lStepRun, -1 },
        { "arrivals -7 ms, 1 ms jitter", Kind::Arrivals, -k_lStepNs, 1000000, k_lStepRun, -1 },
        { "ready +7 ms", Kind::Ready, k_lStepNs, 0, k_lStepRun, 2 },
        { "ready -7 ms", Kind::Ready, -k_lStepNs, 0, k_lStepRun, 2 },
        { "ready +7 ms, 1 ms jitter", Kind::Ready, k_lStepNs, 1000000, k_lStepRun, -1 },
        { "ready -7 ms, 1 ms jitter", Kind::Ready, -k_lStepNs, 1000000, k_lStepRun, -1 },
        // The late frame itself shows a refresh late, and so does the one after.
        { "one frame 7 ms late", Kind::SingleLate, k_lStepNs, 0, k_lStepRun, 2 },
        { "one frame 7 ms late, 1 ms jitter", Kind::SingleLate, k_lStepNs, 1000000, k_lStepRun, -1 },
    };

    struct Result
    {
        long lLongestOffsetRun = 0;
        long lMostJumps = 0;
    };

    Result Run( const Case &c, int nPhase )
    {
        framegen::test::CReplayCheck check;
        std::mt19937 rng{ uint32_t( nPhase + 1 ) };
        const double flPhaseNs = double( nPhase ) * double( framegen::test::k_ulRefreshNs ) / k_nPhases;
        uint64_t ulNextVBlankNs = k_ulStartNs;
        for ( int nFrame = 0; nFrame < k_nRealFrames; ++nFrame )
        {
            const bool bStepped = c.eKind == Kind::SingleLate ? nFrame == k_nStepFrame : nFrame >= k_nStepFrame;
            const int64_t lStepNs = bStepped ? c.lStepNs : 0;
            const double flReadyNs = double( k_ulStartNs ) + flPhaseNs + double( nFrame ) * k_flIntervalNs +
                ( c.eKind == Kind::Arrivals ? 0.0 : double( lStepNs ) );
            const double flJitterNs = c.ulJitterNs
                ? std::uniform_real_distribution<double>{ -double( c.ulJitterNs ), double( c.ulJitterNs ) }( rng )
                : 0.0;
            const double flArrivalNs = flReadyNs + double( k_ulTransportNs ) +
                ( c.eKind == Kind::Arrivals ? double( lStepNs ) : 0.0 ) + flJitterNs;
            while ( double( ulNextVBlankNs ) <= flArrivalNs )
            {
                check.VBlank( ulNextVBlankNs );
                ulNextVBlankNs += framegen::test::k_ulRefreshNs;
            }
            if ( nFrame == k_nSettledFrame )
                check.ClearCounts();
            check.Real( uint64_t( nFrame ), uint64_t( flReadyNs ), uint64_t( flArrivalNs ) );
        }
        return Result{ .lLongestOffsetRun = check.LongestOffsetRun(), .lMostJumps = check.Jumps() };
    }
}

int main()
{
    bool bPassed = true;
    for ( const Case &c : k_Cases )
    {
        Result worst;
        for ( int nPhase = 0; nPhase < k_nPhases; ++nPhase )
        {
            const Result result = Run( c, nPhase );
            worst.lLongestOffsetRun = std::max( worst.lLongestOffsetRun, result.lLongestOffsetRun );
            worst.lMostJumps = std::max( worst.lMostJumps, result.lMostJumps );
        }
        const bool bOk = worst.lLongestOffsetRun <= c.lMaxOffsetRun && ( c.lMaxJumps < 0 || worst.lMostJumps <= c.lMaxJumps );
        bPassed = bPassed && bOk;
        std::printf( "%s: over %d phases, longest run off the half step %ld (at most %ld), content steps a "
                     "refresh off %ld (at most %ld)%s\n",
                     c.pszName, k_nPhases, worst.lLongestOffsetRun, c.lMaxOffsetRun, worst.lMostJumps, c.lMaxJumps,
                     bOk ? "" : "  FAIL" );
    }
    return bPassed ? 0 : 1;
}
