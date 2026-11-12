// Drives CFramePacer with real frames arriving at every phase against the vblanks, at several
// base rates, ratios and arrival jitters, for needed ready times from none to a refresh, and checks
// that no generated output is due sooner than that after its real frame arrived, and that a longer
// need delays the real frames by no more than itself and a quarter refresh. A need falling from a
// refresh steps the delay down at most once, to no later than where a run at the lower need from
// the start shows its real frames, within a millisecond.
//
//   framegen_pacer_first_output
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <vector>

#include "framegen/FramePacer.h"

namespace
{
    constexpr uint64_t k_ulRefreshNs = 16666667;
    constexpr int k_nPhases = 64;
    constexpr int k_nRealFrames = 240;
    constexpr uint64_t k_ulNeedsNs[] = { 0, 4000000, 8000000, 12000000, k_ulRefreshNs };

    struct Case
    {
        const char *pszName;
        framegen::PacingSettings_t settings;
        double flBaseFps;
        // Each real frame arrives up to this much after its even place.
        uint64_t ulJitterNs;
    };

    struct Result
    {
        uint64_t ulGenerated = 0;
        uint64_t ulEarly = 0;
        uint64_t ulShortestNs = UINT64_MAX;
        uint64_t ulReal = 0;
        double flRealDelayNs = 0.0;
        // Real-frame delay changes of over half a refresh after the need fell.
        uint64_t ulSteps = 0;
    };

    // The need each real frame is planned for: `ulBeforeNs` until frame `nFall`, then `ulAfterNs`;
    // real frames are timed from frame `nMeasureFrom`.
    struct Need
    {
        uint64_t ulBeforeNs;
        uint64_t ulAfterNs;
        int nFall;
        int nMeasureFrom;
    };

    void Check( const framegen::PacedOutput_t &shown, uint64_t ulVBlankNs, const std::vector<uint64_t> &arrivals,
        const Need &need, Result &result, std::optional<double> &oflLastDelayNs )
    {
        const uint64_t ulArrivalNs = arrivals[ shown.ulInterval ];
        const uint64_t ulDueNs = std::min( shown.ulDesiredPresentNs, ulVBlankNs );
        const uint64_t ulGivenNs = ulDueNs > ulArrivalNs ? ulDueNs - ulArrivalNs : 0;
        const uint64_t ulNeededNs = int( shown.ulInterval ) < need.nFall ? need.ulBeforeNs : need.ulAfterNs;
        if ( !shown.oGenerated )
        {
            const double flDelayNs = double( ulVBlankNs - ulArrivalNs );
            if ( int( shown.ulInterval ) > need.nFall && oflLastDelayNs &&
                 std::abs( flDelayNs - *oflLastDelayNs ) > double( k_ulRefreshNs ) / 2.0 )
                ++result.ulSteps;
            oflLastDelayNs = flDelayNs;
            if ( int( shown.ulInterval ) >= need.nMeasureFrom )
            {
                ++result.ulReal;
                result.flRealDelayNs += flDelayNs;
            }
            return;
        }
        ++result.ulGenerated;
        result.ulEarly += ulGivenNs < ulNeededNs ? 1 : 0;
        result.ulShortestNs = std::min( result.ulShortestNs, ulGivenNs );
    }

    Result Run( const Case &c, const Need &need, uint64_t ulPhaseNs )
    {
        framegen::CFramePacer pacer;
        pacer.Configure( c.settings );
        std::optional<double> oflLastDelayNs;
        const double flIntervalNs = 1.0e9 / c.flBaseFps;
        const uint64_t ulStartNs = 1000000000;
        const auto ready = []( const framegen::PacedOutput_t & ) { return true; };
        std::vector<uint64_t> arrivals;
        Result result;
        uint64_t ulNextVBlankNs = ulStartNs;
        const auto vblanksUntil = [&]( uint64_t ulUntilNs )
        {
            for ( ; ulNextVBlankNs <= ulUntilNs; ulNextVBlankNs += k_ulRefreshNs )
            {
                if ( const auto shown = pacer.OnVBlank( ulNextVBlankNs, k_ulRefreshNs, ready ) )
                    Check( *shown, ulNextVBlankNs, arrivals, need, result, oflLastDelayNs );
            }
        };
        // A fixed pseudo-random jitter, the same every run.
        uint32_t uSeed = 12345;
        for ( int nFrame = 0; nFrame < k_nRealFrames; ++nFrame )
        {
            uSeed = uSeed * 1664525u + 1013904223u;
            const uint64_t ulJitterNs = c.ulJitterNs ? uSeed % c.ulJitterNs : 0;
            const uint64_t ulArrivalNs = ulStartNs + ulPhaseNs + uint64_t( double( nFrame ) * flIntervalNs ) + ulJitterNs;
            vblanksUntil( ulArrivalNs );
            if ( nFrame > 2 )
                pacer.DropOlderThan( uint64_t( nFrame - 2 ), uint64_t( nFrame - 2 ) );
            arrivals.push_back( ulArrivalNs );
            pacer.SetReadyNeededNs( nFrame < need.nFall ? need.ulBeforeNs : need.ulAfterNs );
            pacer.OnRealFrame( uint64_t( nFrame ), ulArrivalNs, ulArrivalNs, k_ulRefreshNs );
        }
        vblanksUntil( arrivals.back() + 4 * k_ulRefreshNs );
        return result;
    }
}

int main()
{
    using framegen::PacingMode;
    const Case cases[] = {
        { "adaptive 50 fps to 60", { .eMode = PacingMode::Adaptive, .uMaxGenerated = 3 }, 50.0, 0 },
        { "adaptive 50 fps to 60, 3 ms jitter", { .eMode = PacingMode::Adaptive, .uMaxGenerated = 3 }, 50.0, 3000000 },
        { "adaptive 45 fps to 60", { .eMode = PacingMode::Adaptive, .uMaxGenerated = 3 }, 45.0, 0 },
        { "adaptive 30 fps to 60", { .eMode = PacingMode::Adaptive, .uMaxGenerated = 3 }, 30.0, 1000000 },
        { "adaptive 24 fps to 60", { .eMode = PacingMode::Adaptive, .uMaxGenerated = 3 }, 24.0, 0 },
        { "fixed 2x at 30 fps", { .eMode = PacingMode::Fixed, .flRatio = 2.0 }, 30.0, 0 },
        { "fixed 2x at 30 fps, 3 ms jitter", { .eMode = PacingMode::Fixed, .flRatio = 2.0 }, 30.0, 3000000 },
        { "fixed 3x at 20 fps", { .eMode = PacingMode::Fixed, .flRatio = 3.0 }, 20.0, 0 },
    };
    bool bPassed = true;
    for ( const Case &c : cases )
    {
        double flNoNeedDelayNs = 0.0;
        double flLastRealDelayNs = 0.0;
        for ( const uint64_t ulNeededNs : k_ulNeedsNs )
        {
            Result all;
            for ( int nPhase = 0; nPhase < k_nPhases; ++nPhase )
            {
                const Result one = Run( c, { ulNeededNs, ulNeededNs, 0, 0 }, uint64_t( nPhase ) * k_ulRefreshNs / k_nPhases );
                all.ulGenerated += one.ulGenerated;
                all.ulEarly += one.ulEarly;
                all.ulShortestNs = std::min( all.ulShortestNs, one.ulShortestNs );
                all.ulReal += one.ulReal;
                all.flRealDelayNs += one.flRealDelayNs;
            }
            const double flRealDelayNs = all.ulReal ? all.flRealDelayNs / double( all.ulReal ) : 0.0;
            if ( ulNeededNs == 0 )
                flNoNeedDelayNs = flRealDelayNs;
            // A longer need costs the real frames no more than itself, within a quarter refresh.
            const bool bNeedCost = flRealDelayNs + 1.0 >= flLastRealDelayNs &&
                flRealDelayNs <= flNoNeedDelayNs + double( ulNeededNs ) + double( k_ulRefreshNs ) / 4.0;
            flLastRealDelayNs = flRealDelayNs;
            const bool bCase = all.ulGenerated > 0 && all.ulEarly == 0 && all.ulReal > 0 && bNeedCost;
            std::printf( "%s, need %.1f ms: %llu generated outputs over %d phases, %llu due sooner after their "
                         "real frame, the shortest %.2f ms; real frames shown %.2f ms after arriving%s\n",
                         c.pszName, double( ulNeededNs ) / 1.0e6, (unsigned long long)all.ulGenerated, k_nPhases,
                         (unsigned long long)all.ulEarly, double( all.ulShortestNs ) / 1.0e6, flRealDelayNs / 1.0e6,
                         bCase ? "" : "  FAIL" );
            bPassed = bPassed && bCase;
        }
        constexpr uint64_t k_ulFallenNs = 7000000;
        constexpr int k_nFall = 80;
        constexpr int k_nSettled = 140;
        double flFallDelayNs = 0.0;
        double flFixedDelayNs = 0.0;
        uint64_t ulMostSteps = 0;
        for ( int nPhase = 0; nPhase < k_nPhases; ++nPhase )
        {
            const uint64_t ulPhaseNs = uint64_t( nPhase ) * k_ulRefreshNs / k_nPhases;
            const Result fall = Run( c, { k_ulRefreshNs, k_ulFallenNs, k_nFall, k_nSettled }, ulPhaseNs );
            const Result fixed = Run( c, { k_ulFallenNs, k_ulFallenNs, k_nFall, k_nSettled }, ulPhaseNs );
            flFallDelayNs += fall.flRealDelayNs / double( fall.ulReal ) / k_nPhases;
            flFixedDelayNs += fixed.flRealDelayNs / double( fixed.ulReal ) / k_nPhases;
            ulMostSteps = std::max( ulMostSteps, fall.ulSteps - std::min( fall.ulSteps, fixed.ulSteps ) );
        }
        const bool bFall = flFallDelayNs <= flFixedDelayNs + 1.0e6 && ulMostSteps <= 1;
        std::printf( "%s, need falling from a refresh to 7 ms: real frames shown %.2f ms after arriving once "
                     "settled (%.2f at 7 ms from the start), at most %llu more steps in a phase%s\n",
                     c.pszName, flFallDelayNs / 1.0e6, flFixedDelayNs / 1.0e6, (unsigned long long)ulMostSteps,
                     bFall ? "" : "  FAIL" );
        bPassed = bPassed && bFall;
    }
    return bPassed ? 0 : 1;
}
