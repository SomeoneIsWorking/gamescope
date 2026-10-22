// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Barış Hamil

// A device-local 2D image with one mip and one layer, sampled and written by compute shaders
// in GENERAL layout, in any format (CVulkanTexture only takes DRM formats).

#pragma once

#include "rendervulkan.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace gamescope::FrameGen
{
    class CComputeImage
    {
    public:
        // Nothing, with the reason, when the image cannot be made.
        static std::unique_ptr<CComputeImage> Create( uint32_t uWidth, uint32_t uHeight, VkFormat eFormat, std::string &sError );
        ~CComputeImage();

        CComputeImage( const CComputeImage & ) = delete;
        CComputeImage &operator=( const CComputeImage & ) = delete;

        VkImage Image() const { return m_image; }
        VkImageView View() const { return m_view; }
        uint32_t Width() const { return m_uWidth; }
        uint32_t Height() const { return m_uHeight; }

    private:
        CComputeImage() = default;

        VkImage m_image = VK_NULL_HANDLE;
        VkDeviceMemory m_memory = VK_NULL_HANDLE;
        VkImageView m_view = VK_NULL_HANDLE;
        uint32_t m_uWidth = 0;
        uint32_t m_uHeight = 0;
    };
}
