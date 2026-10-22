// Composites unscaled layers into a texture of their own size, values copied without colour
// management. Frame generation's copies and blends go through Gamescope's own compositor.

#pragma once

#include "rendervulkan.hpp"

#include <cstdint>
#include <memory>
#include <optional>

namespace gamescope::FrameGen
{
    // Adds pTexture as an unscaled layer whose values pass through unchanged.
    void AddPassthroughLayer( FrameInfo_t &frameInfo, const gamescope::Rc<CVulkanTexture> &pTexture, float flOpacity );

    // Returns the submission's sequence number, or nothing on failure.
    std::optional<uint64_t> CompositeIntoTexture( FrameInfo_t &frameInfo, const gamescope::Rc<CVulkanTexture> &pTarget,
        std::unique_ptr<CVulkanCmdBuffer> pCmdBuffer );
}
