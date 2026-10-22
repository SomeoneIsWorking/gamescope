// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Barış Hamil

// The sizes and intermediate images of the Lossless generator for one frame size, flow scale
// and size mode (spec sections 3 and 5).

#pragma once

#include "ComputeImage.h"
#include "LosslessShaders.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace gamescope::FrameGen
{
    inline constexpr uint32_t k_uLosslessLumaLevels = 7;
    inline constexpr uint32_t k_uLosslessUiMaskLevels = 6;
    inline constexpr uint32_t k_uLosslessMaxLevels = 7;
    // Second and third flow fields: 3 levels plus the iteration delta, at most 6.
    inline constexpr uint32_t k_uLosslessBaseLevels2 = 3;
    inline constexpr uint32_t k_uLosslessMaxLevels2 = 6;
    inline constexpr uint32_t k_uLosslessFrameSlots = 2;
    inline constexpr uint32_t k_uLosslessFeatureSlots = 3;

    struct LosslessExtent_t
    {
        uint32_t uWidth = 0;
        uint32_t uHeight = 0;
    };

    struct LosslessGeometry_t
    {
        LosslessExtent_t frame;
        // After the small-frame clamp.
        float flFlowScale = 1.0f;
        LosslessExtent_t flow;
        // Levels of the first flow (L) and of the second and third (L2).
        uint32_t uLevels = 0;
        uint32_t uLevels2 = 0;

        // a(l), h(l) and q(l) of the spec.
        LosslessExtent_t Luma( uint32_t uLevel ) const;
        LosslessExtent_t Half( uint32_t uLevel ) const;
        LosslessExtent_t Quarter( uint32_t uLevel ) const;
        LosslessExtent_t UiMask( uint32_t uLevel ) const;
    };

    LosslessGeometry_t ComputeLosslessGeometry( uint32_t uWidth, uint32_t uHeight, float flFlowScale, uint32_t uIterationDelta );

    // Image counts per level that differ between the size modes.
    struct LosslessModeLayout_t
    {
        uint32_t uFeatureChannels = 0;
        uint32_t uHalfImages = 0;
        uint32_t uQuarterImages = 0;
        // First Quarter image the second filter stage of a flow writes.
        uint32_t uFilterOutput = 0;
    };

    LosslessModeLayout_t GetLosslessModeLayout( LosslessSizeMode eMode );

    class CLosslessImages
    {
    public:
        static std::unique_ptr<CLosslessImages> Create( const LosslessGeometry_t &geometry, const LosslessModeLayout_t &layout, bool bUiMask, std::string &sError );

        const LosslessGeometry_t &Geometry() const { return m_Geometry; }
        const CComputeImage &FrameCopy( uint32_t uSlot ) const { return *m_FrameCopies[ uSlot ]; }
        const CComputeImage &Luma( uint32_t uLevel ) const { return *m_Luma[ uLevel ]; }
        const CComputeImage &Feature( uint32_t uSlot, uint32_t uChannel, uint32_t uLevel ) const;
        const CComputeImage &Half( uint32_t uIndex, uint32_t uLevel ) const;
        const CComputeImage &Quarter( uint32_t uIndex, uint32_t uLevel ) const;
        const CComputeImage &FlowA( uint32_t uLevel ) const { return *m_FlowA[ uLevel ]; }
        const CComputeImage &FlowB( uint32_t uLevel ) const { return *m_FlowB[ uLevel ]; }
        const CComputeImage &FlowC( uint32_t uLevel ) const { return *m_FlowC[ uLevel ]; }
        bool HasUiMask() const { return !m_UiMask.empty(); }
        const CComputeImage &UiMask( uint32_t uLevel ) const { return *m_UiMask[ uLevel ]; }

        // Every image, for the clear before first use.
        std::vector<VkImage> AllImages() const;

    private:
        CLosslessImages() = default;

        LosslessGeometry_t m_Geometry;
        LosslessModeLayout_t m_Layout;
        std::vector<std::unique_ptr<CComputeImage>> m_FrameCopies;
        std::vector<std::unique_ptr<CComputeImage>> m_Luma;
        // [slot][channel][level], [index][level].
        std::vector<std::unique_ptr<CComputeImage>> m_Feature;
        std::vector<std::unique_ptr<CComputeImage>> m_Half;
        std::vector<std::unique_ptr<CComputeImage>> m_Quarter;
        std::vector<std::unique_ptr<CComputeImage>> m_FlowA;
        std::vector<std::unique_ptr<CComputeImage>> m_FlowB;
        std::vector<std::unique_ptr<CComputeImage>> m_FlowC;
        std::vector<std::unique_ptr<CComputeImage>> m_UiMask;
    };
}
