// A cross-fade of the two real frames, chosen explicitly (GAMESCOPE_FRAMEGEN_GENERATOR 1). It moves
// nothing, but every in-between image shows its timestamp as the blend weight, which tests capture,
// pacing and presentation apart from the Lossless generator.

#pragma once

#include "FrameSynthesizer.h"

namespace gamescope::FrameGen
{
    class CLinearBlendSynthesizer final : public IFrameSynthesizer
    {
    public:
        bool BeginSession( uint32_t, uint32_t ) override { return true; }
        bool AddRealFrame( const gamescope::Rc<CVulkanTexture> & ) override { return true; }
        std::optional<uint64_t> Generate( const gamescope::Rc<CVulkanTexture> &pPrevious,
            const gamescope::Rc<CVulkanTexture> &pCurrent, float flTimestamp, const gamescope::Rc<CVulkanTexture> &pOutput ) override;
    };
}
