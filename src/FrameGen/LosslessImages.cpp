// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Barış Hamil

#include "LosslessImages.h"

#include <algorithm>
#include <cmath>

namespace gamescope::FrameGen
{
    namespace
    {
        // Spec section 3: 7 levels when the scaled height is over 512.
        constexpr float k_flSevenLevelHeight = 512.0f;
        // The flow scale grows until q(0) holds 32 texels each way, to a 128-texel frame.
        constexpr uint32_t k_uMinQuarterTexels = 32;
        constexpr float k_flMinFlowTexels = 128.0f;

        uint32_t HalfUp( uint32_t u )
        {
            return ( u + 1 ) / 2;
        }

        LosslessExtent_t AtLeastOne( LosslessExtent_t extent )
        {
            return { std::max( extent.uWidth, 1u ), std::max( extent.uHeight, 1u ) };
        }
    }

    LosslessExtent_t LosslessGeometry_t::Luma( uint32_t uLevel ) const
    {
        return AtLeastOne( { flow.uWidth >> uLevel, flow.uHeight >> uLevel } );
    }

    LosslessExtent_t LosslessGeometry_t::Half( uint32_t uLevel ) const
    {
        const LosslessExtent_t luma = Luma( uLevel );
        return { HalfUp( luma.uWidth ), HalfUp( luma.uHeight ) };
    }

    LosslessExtent_t LosslessGeometry_t::Quarter( uint32_t uLevel ) const
    {
        const LosslessExtent_t half = Half( uLevel );
        return { HalfUp( half.uWidth ), HalfUp( half.uHeight ) };
    }

    LosslessExtent_t LosslessGeometry_t::UiMask( uint32_t uLevel ) const
    {
        const LosslessExtent_t quarter = Quarter( 0 );
        return AtLeastOne( { quarter.uWidth >> uLevel, quarter.uHeight >> uLevel } );
    }

    LosslessGeometry_t ComputeLosslessGeometry( uint32_t uWidth, uint32_t uHeight, float flFlowScale, uint32_t uIterationDelta )
    {
        LosslessGeometry_t geometry;
        geometry.frame = { uWidth, uHeight };
        // L uses the flow scale before the clamp.
        geometry.uLevels = flFlowScale * float( uHeight ) > k_flSevenLevelHeight ? 7 : 6;
        geometry.uLevels2 = std::min( k_uLosslessBaseLevels2 + uIterationDelta, k_uLosslessMaxLevels2 );

        auto SetScale = [&]( float flScale )
        {
            geometry.flFlowScale = flScale;
            geometry.flow = { uint32_t( std::floor( flScale * float( uWidth ) ) ), uint32_t( std::floor( flScale * float( uHeight ) ) ) };
        };
        SetScale( flFlowScale );

        const LosslessExtent_t quarter = geometry.Quarter( 0 );
        if ( quarter.uWidth / k_uMinQuarterTexels == 0 || quarter.uHeight / k_uMinQuarterTexels == 0 )
        {
            const float flMin = std::max( k_flMinFlowTexels / float( uWidth ), k_flMinFlowTexels / float( uHeight ) );
            // Rounded up to whole percent.
            SetScale( std::ceil( flMin * 100.0f ) / 100.0f );
        }
        return geometry;
    }

    LosslessModeLayout_t GetLosslessModeLayout( LosslessSizeMode eMode )
    {
        if ( eMode == LosslessSizeMode::Performance )
            return { .uFeatureChannels = 2, .uHalfImages = 2, .uQuarterImages = 5, .uFilterOutput = 3 };
        return { .uFeatureChannels = 4, .uHalfImages = 4, .uQuarterImages = 8, .uFilterOutput = 4 };
    }

    std::unique_ptr<CLosslessImages> CLosslessImages::Create( const LosslessGeometry_t &geometry, const LosslessModeLayout_t &layout, bool bUiMask, std::string &sError )
    {
        std::unique_ptr<CLosslessImages> p{ new CLosslessImages() };
        p->m_Geometry = geometry;
        p->m_Layout = layout;

        auto Add = [&]( std::vector<std::unique_ptr<CComputeImage>> &images, LosslessExtent_t extent, VkFormat eFormat )
        {
            std::unique_ptr<CComputeImage> pImage = CComputeImage::Create( extent.uWidth, extent.uHeight, eFormat, sError );
            if ( !pImage )
                return false;
            images.push_back( std::move( pImage ) );
            return true;
        };

        const uint32_t uLevels = geometry.uLevels;
        // Copy's SPIR-V declares an Rgba32f output (spec 2.3).
        for ( uint32_t i = 0; i < k_uLosslessFrameSlots; i++ )
        {
            if ( !Add( p->m_FrameCopies, geometry.frame, VK_FORMAT_R32G32B32A32_SFLOAT ) )
                return nullptr;
        }
        for ( uint32_t i = 0; i < k_uLosslessLumaLevels; i++ )
        {
            if ( !Add( p->m_Luma, geometry.Luma( i ), VK_FORMAT_R8_UNORM ) )
                return nullptr;
        }
        for ( uint32_t k = 0; k < k_uLosslessFeatureSlots; k++ )
        {
            for ( uint32_t c = 0; c < layout.uFeatureChannels; c++ )
            {
                for ( uint32_t l = 0; l < uLevels; l++ )
                {
                    if ( !Add( p->m_Feature, geometry.Quarter( l ), VK_FORMAT_R8G8B8A8_UNORM ) )
                        return nullptr;
                }
            }
        }
        for ( uint32_t c = 0; c < layout.uHalfImages; c++ )
        {
            for ( uint32_t l = 0; l < uLevels; l++ )
            {
                if ( !Add( p->m_Half, geometry.Half( l ), VK_FORMAT_R8G8B8A8_UNORM ) )
                    return nullptr;
            }
        }
        for ( uint32_t c = 0; c < layout.uQuarterImages; c++ )
        {
            for ( uint32_t l = 0; l < uLevels; l++ )
            {
                if ( !Add( p->m_Quarter, geometry.Quarter( l ), VK_FORMAT_R8G8B8A8_UNORM ) )
                    return nullptr;
            }
        }
        for ( uint32_t l = 0; l < uLevels; l++ )
        {
            if ( !Add( p->m_FlowA, geometry.Quarter( l ), VK_FORMAT_R16G16B16A16_SFLOAT ) )
                return nullptr;
        }
        for ( uint32_t l = 0; l < geometry.uLevels2; l++ )
        {
            if ( !Add( p->m_FlowB, geometry.Quarter( l ), VK_FORMAT_R16G16B16A16_SFLOAT ) ||
                 !Add( p->m_FlowC, geometry.Quarter( l ), VK_FORMAT_R16G16B16A16_SFLOAT ) )
                return nullptr;
        }
        if ( bUiMask )
        {
            for ( uint32_t i = 0; i < k_uLosslessUiMaskLevels; i++ )
            {
                if ( !Add( p->m_UiMask, geometry.UiMask( i ), VK_FORMAT_R8_UNORM ) )
                    return nullptr;
            }
        }
        return p;
    }

    const CComputeImage &CLosslessImages::Feature( uint32_t uSlot, uint32_t uChannel, uint32_t uLevel ) const
    {
        return *m_Feature[ ( uSlot * m_Layout.uFeatureChannels + uChannel ) * m_Geometry.uLevels + uLevel ];
    }

    const CComputeImage &CLosslessImages::Half( uint32_t uIndex, uint32_t uLevel ) const
    {
        return *m_Half[ uIndex * m_Geometry.uLevels + uLevel ];
    }

    const CComputeImage &CLosslessImages::Quarter( uint32_t uIndex, uint32_t uLevel ) const
    {
        return *m_Quarter[ uIndex * m_Geometry.uLevels + uLevel ];
    }

    std::vector<VkImage> CLosslessImages::AllImages() const
    {
        std::vector<VkImage> images;
        for ( const auto *pList : { &m_FrameCopies, &m_Luma, &m_Feature, &m_Half, &m_Quarter, &m_FlowA, &m_FlowB, &m_FlowC, &m_UiMask } )
        {
            for ( const std::unique_ptr<CComputeImage> &pImage : *pList )
                images.push_back( pImage->Image() );
        }
        return images;
    }
}
