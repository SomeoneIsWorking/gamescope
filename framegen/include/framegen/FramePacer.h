// Frame generation pacing: which in-between images to generate for each real frame,
// and which image to show at each vblank.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <vector>

namespace framegen
{
    // Lossless Scaling 3.2.2's config.ini values (docs/issues/adaptive-lsfg-vk.md in openSU).
    // An output this close to a real frame shows that real frame instead of a generated one.
    inline constexpr float k_flRealTimestampTolerance = 0.05f;
    // Real-frame intervals averaged, and how far off the mean one restarts the average.
    inline constexpr size_t k_nFrameIntervalHistory = 15;
    inline constexpr double k_flFrameIntervalResetFactor = 6.0;
    // Below this base rate only real frames show.
    inline constexpr double k_flMinBaseFps = 10.0;

    // Share of each real frame's ready-time error that moves the output timeline, so ready times
    // quantized by the game's own vsync still give evenly spaced outputs.
    inline constexpr double k_flTimelineCorrection = 0.1;
    // Real frames in a row whose ready times miss the timeline on one side by more than the
    // real-frame tolerance before the timeline takes the step at once.
    inline constexpr uint32_t k_uTimelineStepFrames = 3;
    // Generation restarts only this share of the tolerance below where it stopped.
    inline constexpr double k_flToleranceHysteresis = 0.5;
    // Real frames the base rate must stay past a threshold before generation turns on or off.
    inline constexpr uint32_t k_uDecisionHoldFrames = k_nFrameIntervalHistory;
    // Real frames the aligned delay must hold still before it replaces a kept delay that still
    // fits: one interval leaving the history can move it past the tolerance in one step.
    inline constexpr uint32_t k_uAlignedSettleFrames = k_nFrameIntervalHistory;
    // A kept delay a fallen ready need no longer asks for gives way to the one it allows when that
    // shows the scene at least this share of a refresh sooner: one visible step, worth it.
    inline constexpr double k_flNeedFallStepRefreshes = 0.5;
    // That step lands at least this share of a refresh above the least delay the need allows, so
    // arrival jitter does not push it out again.
    inline constexpr double k_flNeedFallMarginRefreshes = 0.25;
    inline constexpr uint32_t k_uMaxGeneratedLimit = 15;

    enum class PacingMode : uint32_t
    {
        // A fixed number of outputs per real frame.
        Fixed = 0,
        // Outputs at the target rate, whatever the real frame rate.
        Adaptive = 1,
    };

    struct PacingSettings_t
    {
        PacingMode eMode = PacingMode::Fixed;
        // Fixed: output frames per real frame. Values at or below 1 generate nothing.
        double flRatio = 1.0;
        // Adaptive: output rate; 0 is the display's refresh. Never above the refresh.
        double flTargetFps = 0.0;
        // Adaptive: most generated images per real frame.
        uint32_t uMaxGenerated = 3;
        // Adaptive: a base rate this close to the target shows real frames only.
        double flToleranceFps = 0.0;

        bool operator==( const PacingSettings_t & ) const = default;
    };

    // Mean of the recent intervals between real frames. An interval far off the mean
    // (a stall, a burst) restarts the history without itself, since it measures no rate.
    class CFrameIntervalEstimator
    {
    public:
        void Reset();
        void AddFrame( uint64_t ulTimeNs );
        std::optional<double> GetMeanIntervalNs() const;
        // The interval added nIntervalsAgo frames ago (1 the latest) held a step of the clock, not
        // a frame of the rate: it takes the mean of the others.
        void ForgetStep( size_t nIntervalsAgo );
        // The history holds k_nFrameIntervalHistory intervals since the last restart.
        bool IsFull() const { return m_nIntervalCount == m_flIntervals.size(); }

    private:
        std::optional<uint64_t> m_oulLastFrameNs;
        std::array<double, k_nFrameIntervalHistory> m_flIntervals{};
        size_t m_nIntervalCount = 0;
        size_t m_nNextInterval = 0;
    };

    struct PacedOutput_t
    {
        // The real frame whose interval this output belongs to.
        uint64_t ulInterval = 0;
        // Shows real frame ulRealFrame, or the generated image oGenerated of ulInterval.
        uint64_t ulRealFrame = 0;
        std::optional<uint32_t> oGenerated;
        float flTimestamp = 1.0f;
        uint64_t ulDesiredPresentNs = 0;
    };

    // Why real frames are shown as they come, or that outputs are being generated.
    enum class PacingState : uint32_t
    {
        // Fewer intervals measured than the history holds.
        Measuring,
        BelowFloor,
        // The real frame rate reaches the target, or the ratio is at most 1.
        AtTarget,
        WithinTolerance,
        Generating,
    };

    const char *PacingStateName( PacingState eState );

    struct IntervalPlan_t
    {
        // Positions in (0, 1) between the previous and this real frame to generate.
        std::vector<float> flTimestamps;
        PacingState eState = PacingState::Measuring;
        // Measured real frame rate and the output rate aimed for; 0 while unknown.
        double flBaseFps = 0.0;
        double flTargetFps = 0.0;
    };

    // The one place that decides how many images are generated per real frame, at which
    // timestamps, and when each image is shown.
    //
    // Outputs go on vblanks. Content time is the real frames' ready times; arrival times only say
    // how late the plan must be. The scene shows one real-frame interval late on a timeline of the
    // ready times, later by the recent arrival lag behind it, the ready time the generator needs and
    // a refresh, then earlier by under a refresh so the real frame itself lands on a vblank: the
    // first image after a real frame is due at least that ready time after it arrives.
    //
    // A ready time within the real-frame tolerance of the timeline's prediction is jitter and moves
    // it by k_flTimelineCorrection of its error. Ready times past the tolerance on one side for
    // k_uTimelineStepFrames frames in a row are a lasting step: the timeline takes it at once, the
    // interval it happened in leaves the rate history, and the plan re-aligns, so a step costs one
    // re-align and real frames are back on vblanks within k_uTimelineStepFrames frames, not a ramp.
    // A lasting step of the arrivals alone moves only the lag. One late frame is not a step; more
    // than an interval off (a stall) restarts the timeline.
    //
    // The delay is kept while it stays within the refresh, so arrival jitter moves nothing and
    // content time steps evenly at any ratio, until the delay that lands real frames on vblanks has
    // held still for k_uAlignedSettleFrames, or until a fallen ready need allows one at least
    // k_flNeedFallStepRefreshes of a refresh sooner. Each vblank of the interval earns
    // 1 / (step x interval) of an output, carried over between real frames, so the output rate
    // follows any fractional ratio: the step is 1 / ratio in fixed mode, and in adaptive mode base
    // rate / target, never below 1 / (max + 1). An output shows the scene at its own vblank's time,
    // at most max generated per real frame. Without generation each real frame shows at once.
    class CFramePacer
    {
    public:
        void Configure( const PacingSettings_t &settings );
        void Reset();
        // How long after a real frame arrives the generator has its first image ready, which the
        // first generated output after it is planned at least that late for; 0 by default.
        void SetReadyNeededNs( uint64_t ulReadyNeededNs ) { m_ulReadyNeededNs = ulReadyNeededNs; }

        // Most images generated for any one real frame, which sizes the destination images.
        uint32_t GetMaxGeneratedPerFrame() const;

        // Real frame ulFrame (counting up from 0 since the last reset): the game finished it at
        // ulReadyNs, its content time, which measures the base rate and places it on the timeline,
        // and it reached frame generation at ulArrivalNs, which sets how late the plan must be.
        // Queues its outputs and returns the images to generate for the interval it ends.
        IntervalPlan_t OnRealFrame( uint64_t ulFrame, uint64_t ulReadyNs, uint64_t ulArrivalNs, uint64_t ulRefreshNs );

        // Drops queued outputs that need images older than the given real frame and interval.
        void DropOlderThan( uint64_t ulOldestRealFrame, uint64_t ulOldestInterval );

        // The output to show from the vblank at ulVBlankNs, or nothing to keep the current one.
        // bReady says whether a generated output's image is finished.
        std::optional<PacedOutput_t> OnVBlank( uint64_t ulVBlankNs, uint64_t ulRefreshNs,
            const std::function<bool( const PacedOutput_t & )> &bReady );

        uint64_t GetDroppedOutputs() const { return m_ulDroppedOutputs; }
        const PacedOutput_t *PeekNext() const { return m_Queue.empty() ? nullptr : &m_Queue.front(); }

    private:
        double GetTargetFps( uint64_t ulRefreshNs ) const;
        bool DecideGenerating( double flBaseFps, double flTargetFps );
        // Moves the timeline to a real frame ready at ulReadyNs and keeps how far behind it the
        // frame arrived; false when it restarts (a stall).
        bool AdvanceTimeline( uint64_t ulReadyNs, uint64_t ulArrivalNs, double flMeanIntervalNs );
        void AddArrivalLag( uint64_t ulArrivalNs );
        // How far behind the timeline recent real frames arrived, at most.
        double GetArrivalLagNs() const;
        void ShowRealFrame( uint64_t ulFrame, uint64_t ulTimeNs );
        void StartTimeline( uint64_t ulFrame, uint64_t ulReadyNs, uint64_t ulArrivalNs, uint64_t ulRefreshNs );
        // The vblank grid's last point at or before flTimeNs; flTimeNs itself before any vblank.
        double VBlankAtOrBefore( double flTimeNs, uint64_t ulRefreshNs ) const;
        // Queues the outputs of the vblanks up to real frame ulFrame's own.
        void PlanOnVBlanks( IntervalPlan_t &plan, uint64_t ulFrame, double flStep, double flMeanIntervalNs, uint64_t ulRefreshNs );
        // Queues an output for real frame ulFrame's interval at timestamp flTimestamp, snapped to
        // a real frame when close to one.
        void QueueOutput( IntervalPlan_t &plan, uint64_t ulFrame, double flTimestamp, uint64_t ulDesiredPresentNs );

        PacingSettings_t m_Settings;
        CFrameIntervalEstimator m_Intervals;
        // Outputs earned by the vblanks so far, carried between real frames.
        double m_flCredit = 0.0;
        // Where the output timeline puts the latest real frame, and how far recent real
        // frames arrived after it.
        double m_flTimelineNs = 0.0;
        std::array<double, k_nFrameIntervalHistory> m_flArrivalLagsNs{};
        // The latest vblank, the last that showed an output, and the one the last queued output shows from.
        uint64_t m_ulLastVBlankNs = 0;
        uint64_t m_ulLastShownVBlankNs = 0;
        std::optional<double> m_oflLastClaimedVBlankNs;
        // From the timeline to where the current interval starts on screen.
        std::optional<double> m_oflDelayNs;
        // The last aligned delay, and for how many real frames it has held within the tolerance.
        std::optional<double> m_oflLastAlignedNs;
        uint32_t m_uAlignedSteadyFrames = 0;
        size_t m_nArrivalLags = 0;
        size_t m_nNextArrivalLag = 0;
        // Real frames in a row past the tolerance, and on which side of the timeline.
        uint32_t m_uStepFrames = 0;
        int m_nStepSide = 0;
        // Adaptive: generating or passing real frames through, and for how many real frames
        // the base rate has asked for the other.
        std::optional<bool> m_obGenerating;
        uint32_t m_uDecisionFrames = 0;
        bool m_bTimelineValid = false;
        std::deque<PacedOutput_t> m_Queue;
        uint64_t m_ulReadyNeededNs = 0;
        // The need the last interval was planned for, and whether one that fell has yet to step
        // the delay down.
        std::optional<uint64_t> m_oulPlannedNeedNs;
        bool m_bNeedFallPending = false;
        uint64_t m_ulDroppedOutputs = 0;
    };
}
