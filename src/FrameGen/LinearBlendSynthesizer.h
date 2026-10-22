// A cross-fade of the two real frames. It moves nothing, but every in-between image shows its
// timestamp as the blend weight, which proves capture, pacing and presentation.

#pragma once

#include "FrameSynthesizer.h"

namespace gamescope::FrameGen
{
    class CLinearBlendSynthesizer final : public IFrameSynthesizer
    {
    public:
        std::optional<uint64_t> Generate( const gamescope::Rc<CVulkanTexture> &pPrevious,
            const gamescope::Rc<CVulkanTexture> &pCurrent, float flTimestamp, const gamescope::Rc<CVulkanTexture> &pOutput ) override;
    };
}
