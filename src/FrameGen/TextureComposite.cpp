#include "TextureComposite.h"

#include "steamcompmgr.hpp"

namespace gamescope::FrameGen
{
    void AddPassthroughLayer( FrameInfo_t &frameInfo, const gamescope::Rc<CVulkanTexture> &pTexture, float flOpacity )
    {
        FrameInfo_t::Layer_t *pLayer = frameInfo.layers.push();
        pLayer->tex = pTexture;
        pLayer->zpos = frameInfo.layers.count() - 1;
        pLayer->offset = { 0.0f, 0.0f };
        pLayer->scale = { 1.0f, 1.0f };
        pLayer->opacity = flOpacity;
        pLayer->filter = GamescopeUpscaleFilter::NEAREST;
        pLayer->blackBorder = false;
        pLayer->applyColorMgmt = false;
        pLayer->colorspace = GAMESCOPE_APP_TEXTURE_COLORSPACE_PASSTHRU;
    }

    std::optional<uint64_t> CompositeIntoTexture( FrameInfo_t &frameInfo, const gamescope::Rc<CVulkanTexture> &pTarget,
        std::unique_ptr<CVulkanCmdBuffer> pCmdBuffer )
    {
        frameInfo.applyOutputColorMgmt = false;
        frameInfo.outputEncodingEOTF = EOTF_Gamma22;

        // The composite covers the current output size.
        const uint32_t uOutputWidth = currentOutputWidth;
        const uint32_t uOutputHeight = currentOutputHeight;
        currentOutputWidth = pTarget->width();
        currentOutputHeight = pTarget->height();
        std::optional<uint64_t> oSequence = vulkan_composite( &frameInfo, nullptr, false, pTarget, false, std::move( pCmdBuffer ) );
        currentOutputWidth = uOutputWidth;
        currentOutputHeight = uOutputHeight;
        return oSequence;
    }
}
