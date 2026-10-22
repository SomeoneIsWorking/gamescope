// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Barış Hamil

#include "ComputeImage.h"

#include <format>

namespace gamescope::FrameGen
{
    std::unique_ptr<CComputeImage> CComputeImage::Create( uint32_t uWidth, uint32_t uHeight, VkFormat eFormat, std::string &sError )
    {
        std::unique_ptr<CComputeImage> pImage{ new CComputeImage() };
        pImage->m_uWidth = uWidth;
        pImage->m_uHeight = uHeight;

        const VkImageCreateInfo imageInfo =
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = eFormat,
            .extent = { uWidth, uHeight, 1 },
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            // Transfer destination for the clear before first use.
            .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };
        VkResult res = g_device.vk.CreateImage( g_device.device(), &imageInfo, nullptr, &pImage->m_image );
        if ( res != VK_SUCCESS )
        {
            sError = std::format( "vkCreateImage {}x{} format {} failed: {}", uWidth, uHeight, int( eFormat ), int( res ) );
            return nullptr;
        }

        VkMemoryRequirements requirements;
        g_device.vk.GetImageMemoryRequirements( g_device.device(), pImage->m_image, &requirements );
        const int32_t nMemoryType = g_device.findMemoryType( VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, requirements.memoryTypeBits );
        if ( nMemoryType < 0 )
        {
            sError = "no device-local memory type for an image";
            return nullptr;
        }

        const VkMemoryAllocateInfo allocateInfo =
        {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = requirements.size,
            .memoryTypeIndex = uint32_t( nMemoryType ),
        };
        res = g_device.vk.AllocateMemory( g_device.device(), &allocateInfo, nullptr, &pImage->m_memory );
        if ( res != VK_SUCCESS )
        {
            sError = std::format( "vkAllocateMemory of {} bytes failed: {}", requirements.size, int( res ) );
            return nullptr;
        }
        res = g_device.vk.BindImageMemory( g_device.device(), pImage->m_image, pImage->m_memory, 0 );
        if ( res != VK_SUCCESS )
        {
            sError = std::format( "vkBindImageMemory failed: {}", int( res ) );
            return nullptr;
        }

        const VkImageViewCreateInfo viewInfo =
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = pImage->m_image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = eFormat,
            .subresourceRange = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .levelCount = 1, .layerCount = 1 },
        };
        res = g_device.vk.CreateImageView( g_device.device(), &viewInfo, nullptr, &pImage->m_view );
        if ( res != VK_SUCCESS )
        {
            sError = std::format( "vkCreateImageView failed: {}", int( res ) );
            return nullptr;
        }
        return pImage;
    }

    CComputeImage::~CComputeImage()
    {
        if ( m_view != VK_NULL_HANDLE )
            g_device.vk.DestroyImageView( g_device.device(), m_view, nullptr );
        if ( m_image != VK_NULL_HANDLE )
            g_device.vk.DestroyImage( g_device.device(), m_image, nullptr );
        if ( m_memory != VK_NULL_HANDLE )
            g_device.vk.FreeMemory( g_device.device(), m_memory, nullptr );
    }
}
