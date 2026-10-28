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

namespace gamescope::FrameGen
{
    // Lossless Scaling 3.2.2's config.ini values (docs/issues/adaptive-lsfg-vk.md in openSU).
    // An output this close to a real frame shows that real frame instead of a generated one.
    inline constexpr float k_flRealTimestampTolerance = 0.05f;
    // Real-frame intervals averaged, and how far off the mean one restarts the average.
    inline constexpr size_t k_nFrameIntervalHistory = 15;
    inline constexpr double k_flFrameIntervalResetFactor = 6.0;
    // Below this base rate only real frames show.
    inline constexpr double k_flMinBaseFps = 10.0;

    // Share of each real frame's arrival error that moves the output timeline, so arrivals
    // quantized by the game's own vsync still give evenly spaced outputs.
    inline constexpr double k_flTimelineCorrection = 0.1;
    // Generation restarts only this share of the tolerance below where it stopped.
    inline constexpr double k_flToleranceHysteresis = 0.5;
    // Real frames the base rate must stay past a threshold before generation turns on or off.
    inline constexpr uint32_t k_uDecisionHoldFrames = k_nFrameIntervalHistory;
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
    // Outputs go on vblanks. The scene shows one real-frame interval late, on a smoothed
    // timeline of the real frames, later by the recent arrival lag and a refresh so the first
    // image after a real frame has a refresh of GPU time, then earlier by under a refresh so the real
    // frame itself lands on a vblank. That delay is kept while it stays within the refresh, so
    // arrival jitter moves nothing and content time steps evenly at any ratio. Each vblank of the interval earns 1 / (step x interval)
    // of an output, carried over between real frames, so the output rate follows any
    // fractional ratio: the step is 1 / ratio in fixed mode, and in adaptive mode base rate /
    // target, never below 1 / (max + 1). An output shows the scene at its own vblank's time,
    // at most max generated per real frame. Without generation each real frame shows at once.
    class CFramePacer
    {
    public:
        void Configure( const PacingSettings_t &settings );
        void Reset();

        // Most images generated for any one real frame, which sizes the destination images.
        uint32_t GetMaxGeneratedPerFrame() const;

        // Real frame ulFrame (counting up from 0 since the last reset): the game finished it at
        // ulReadyNs, which measures the base rate, and it reached frame generation at ulArrivalNs,
        // which places it on the output timeline. Queues its outputs and returns the images to
        // generate for the interval it ends.
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
        // Moves the timeline to real frame ulTimeNs; false when it restarts (a stall).
        bool AdvanceTimeline( uint64_t ulTimeNs, double flMeanIntervalNs );
        // How far behind the timeline recent real frames arrived, at most.
        double GetArrivalLagNs() const;
        void ShowRealFrame( uint64_t ulFrame, uint64_t ulTimeNs );
        void StartTimeline( uint64_t ulFrame, uint64_t ulTimeNs, uint64_t ulRefreshNs );
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
        size_t m_nArrivalLags = 0;
        size_t m_nNextArrivalLag = 0;
        // Adaptive: generating or passing real frames through, and for how many real frames
        // the base rate has asked for the other.
        std::optional<bool> m_obGenerating;
        uint32_t m_uDecisionFrames = 0;
        bool m_bTimelineValid = false;
        std::deque<PacedOutput_t> m_Queue;
        uint64_t m_ulDroppedOutputs = 0;
    };
}
