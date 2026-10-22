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
    // An output this close to a real frame shows that real frame instead of a generated one.
    inline constexpr float k_flRealTimestampTolerance = 0.05f;
    // Real-frame intervals averaged, and how far off the mean one restarts the average.
    inline constexpr size_t k_nFrameIntervalHistory = 15;
    inline constexpr double k_flFrameIntervalResetFactor = 6.0;
    // Share of each real frame's arrival error that moves the output timeline, so arrivals
    // quantized by the game's own vsync still give evenly spaced outputs.
    inline constexpr double k_flTimelineCorrection = 0.1;

    struct PacingSettings_t
    {
        // Output frames per real frame. Values at or below 1 generate nothing.
        double flRatio = 1.0;

        bool operator==( const PacingSettings_t & ) const = default;
    };

    // Mean of the recent intervals between real frames. An interval far off the mean
    // (a stall, a load) restarts the history.
    class CFrameIntervalEstimator
    {
    public:
        void Reset();
        void AddFrame( uint64_t ulTimeNs );
        std::optional<double> GetMeanIntervalNs() const;

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

    struct IntervalPlan_t
    {
        // Positions in (0, 1) between the previous and this real frame to generate.
        std::vector<float> flTimestamps;
    };

    // The one place that decides how many images are generated per real frame, at which
    // timestamps, and when each image is shown.
    //
    // A phase accumulator advances by one output step per output and carries over between
    // real frames, so the count per real frame follows any fractional ratio. Outputs are shown
    // one real-frame interval after the real frame that completes them, at their phase within
    // that interval on a smoothed timeline of the real frames, and at most one per vblank.
    class CFramePacer
    {
    public:
        void Configure( const PacingSettings_t &settings );
        void Reset();

        // Most images generated for any one real frame, which sizes the destination images.
        uint32_t GetMaxGeneratedPerFrame() const;

        // Real frame ulFrame (counting up from 0 since the last reset) finished at ulTimeNs.
        // Queues its outputs and returns the images to generate for the interval it ends.
        IntervalPlan_t OnRealFrame( uint64_t ulFrame, uint64_t ulTimeNs );

        // Drops queued outputs that need images older than the given real frame and interval.
        void DropOlderThan( uint64_t ulOldestRealFrame, uint64_t ulOldestInterval );

        // The output to show from the vblank at ulVBlankNs, or nothing to keep the current one.
        // bReady says whether a generated output's image is finished.
        std::optional<PacedOutput_t> OnVBlank( uint64_t ulVBlankNs, uint64_t ulRefreshNs,
            const std::function<bool( const PacedOutput_t & )> &bReady );

        uint64_t GetDroppedOutputs() const { return m_ulDroppedOutputs; }
        const PacedOutput_t *PeekNext() const { return m_Queue.empty() ? nullptr : &m_Queue.front(); }

    private:
        double GetStep() const;
        double AdvanceTimeline( uint64_t ulTimeNs, double flMeanIntervalNs );

        PacingSettings_t m_Settings;
        CFrameIntervalEstimator m_Intervals;
        double m_flPhase = 0.0;
        // Where the output timeline puts the latest real frame.
        double m_flTimelineNs = 0.0;
        std::deque<PacedOutput_t> m_Queue;
        uint64_t m_ulDroppedOutputs = 0;
    };
}
