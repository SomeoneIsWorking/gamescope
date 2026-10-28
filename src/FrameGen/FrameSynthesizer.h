// Makes the image between two real frames of a window. Implementations record their GPU work
// on Gamescope's queue, so anything submitted after Generate sees the finished image.

#pragma once

#include "rc.h"

#include <cstdint>
#include <optional>

class CVulkanTexture;

namespace gamescope::FrameGen
{
    class IFrameSynthesizer
    {
    public:
        virtual ~IFrameSynthesizer() = default;

        // A new window or size, or real frames left out: forgets every real frame seen before.
        virtual bool BeginSession( uint32_t uWidth, uint32_t uHeight ) = 0;

        // Called with the real frames of the session in order, consecutive since BeginSession,
        // before any Generate that uses them.
        virtual bool AddRealFrame( const gamescope::Rc<CVulkanTexture> &pFrame ) = 0;

        // Writes the scene at flTimestamp in (0, 1) between pPrevious (0) and pCurrent (1), the
        // last two frames given to AddRealFrame, into pOutput, all of the same size and format.
        // Returns the submission's sequence number.
        virtual std::optional<uint64_t> Generate( const gamescope::Rc<CVulkanTexture> &pPrevious,
            const gamescope::Rc<CVulkanTexture> &pCurrent, float flTimestamp, const gamescope::Rc<CVulkanTexture> &pOutput ) = 0;
    };
}
