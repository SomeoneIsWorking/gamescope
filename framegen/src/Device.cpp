// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Barış Hamil

#include "framegen/Device.h"

namespace framegen
{
    std::shared_ptr<CDevice> CDevice::Create( const DeviceDesc_t &desc, std::string &sError )
    {
        if ( !desc.pfnGetInstanceProcAddr || desc.instance == VK_NULL_HANDLE || desc.physicalDevice == VK_NULL_HANDLE ||
             desc.device == VK_NULL_HANDLE || !desc.pQueue )
        {
            sError = "the device is incomplete";
            return nullptr;
        }

        std::shared_ptr<CDevice> p{ new CDevice() };
        p->m_Desc = desc;

        auto pfnGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr) desc.pfnGetInstanceProcAddr( desc.instance, "vkGetDeviceProcAddr" );
        auto pfnGetProperties = (PFN_vkGetPhysicalDeviceProperties) desc.pfnGetInstanceProcAddr( desc.instance, "vkGetPhysicalDeviceProperties" );
        auto pfnGetMemoryProperties = (PFN_vkGetPhysicalDeviceMemoryProperties) desc.pfnGetInstanceProcAddr( desc.instance, "vkGetPhysicalDeviceMemoryProperties" );
        p->m_pfnGetFormatProperties = (PFN_vkGetPhysicalDeviceFormatProperties) desc.pfnGetInstanceProcAddr( desc.instance, "vkGetPhysicalDeviceFormatProperties" );
        if ( !pfnGetDeviceProcAddr || !pfnGetProperties || !pfnGetMemoryProperties || !p->m_pfnGetFormatProperties )
        {
            sError = "the instance lacks a core function";
            return nullptr;
        }

#define FRAMEGEN_VK_FUNC( x ) \
        p->m_Functions.x = (PFN_vk##x) pfnGetDeviceProcAddr( desc.device, "vk" #x ); \
        if ( !p->m_Functions.x ) \
        { \
            sError = "the device lacks vk" #x; \
            return nullptr; \
        }
        FRAMEGEN_VK_FUNCTIONS
#undef FRAMEGEN_VK_FUNC

        pfnGetProperties( desc.physicalDevice, &p->m_Properties );
        pfnGetMemoryProperties( desc.physicalDevice, &p->m_MemoryProperties );
        return p;
    }

    int32_t CDevice::FindMemoryType( VkMemoryPropertyFlags eProperties, uint32_t uRequiredTypeBits ) const
    {
        for ( uint32_t i = 0; i < m_MemoryProperties.memoryTypeCount; i++ )
        {
            if ( ( ( 1u << i ) & uRequiredTypeBits ) == 0 )
                continue;
            if ( ( eProperties & m_MemoryProperties.memoryTypes[ i ].propertyFlags ) != eProperties )
                continue;
            return int32_t( i );
        }
        return -1;
    }

    VkFormatFeatureFlags CDevice::OptimalTilingFeatures( VkFormat eFormat ) const
    {
        VkFormatProperties props;
        m_pfnGetFormatProperties( m_Desc.physicalDevice, eFormat, &props );
        return props.optimalTilingFeatures;
    }

    void CDevice::Log( LogLevel eLevel, const std::string &sLine ) const
    {
        if ( m_Desc.log )
            m_Desc.log( eLevel, sLine );
    }
}
