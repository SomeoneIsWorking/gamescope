#include "LinearBlendSynthesizer.h"

#include "TextureComposite.h"

namespace gamescope::FrameGen
{
    std::optional<uint64_t> CLinearBlendSynthesizer::Generate( const gamescope::Rc<CVulkanTexture> &pPrevious,
        const gamescope::Rc<CVulkanTexture> &pCurrent, float flTimestamp, const gamescope::Rc<CVulkanTexture> &pOutput )
    {
        // Premultiplied blending of an opaque frame gives previous * (1 - t) + current * t.
        FrameInfo_t frameInfo = {};
        AddPassthroughLayer( frameInfo, pPrevious, 1.0f );
        AddPassthroughLayer( frameInfo, pCurrent, flTimestamp );
        return CompositeIntoTexture( frameInfo, pOutput, g_device.commandBuffer() );
    }
}
