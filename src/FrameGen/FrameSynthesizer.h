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

        // Writes the scene at flTimestamp in (0, 1) between pPrevious (0) and pCurrent (1) into
        // pOutput, all of the same size and format. Returns the submission's sequence number.
        virtual std::optional<uint64_t> Generate( const gamescope::Rc<CVulkanTexture> &pPrevious,
            const gamescope::Rc<CVulkanTexture> &pCurrent, float flTimestamp, const gamescope::Rc<CVulkanTexture> &pOutput ) = 0;
    };
}
