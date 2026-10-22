#include "GpuTimestamps.h"

#include "steamcompmgr.hpp"

#include <algorithm>
#include <vector>

namespace gamescope
{
    CGpuTimestamps::CGpuTimestamps( LogScope &log )
        : m_Log{ log }
    {
    }

    CGpuTimestamps::~CGpuTimestamps()
    {
        if ( m_pool != VK_NULL_HANDLE )
            g_device.vk.DestroyQueryPool( g_device.device(), m_pool, nullptr );
    }

    bool CGpuTimestamps::Init()
    {
        if ( m_pool != VK_NULL_HANDLE || m_bUnsupported )
            return m_pool != VK_NULL_HANDLE;

        uint32_t uFamilyCount = 0;
        g_device.vk.GetPhysicalDeviceQueueFamilyProperties( g_device.physDev(), &uFamilyCount, nullptr );
        std::vector<VkQueueFamilyProperties> families( uFamilyCount );
        g_device.vk.GetPhysicalDeviceQueueFamilyProperties( g_device.physDev(), &uFamilyCount, families.data() );
        if ( g_device.queueFamily() >= uFamilyCount || families[ g_device.queueFamily() ].timestampValidBits == 0 )
        {
            m_Log.errorf( "queue family %u has no timestamp support", g_device.queueFamily() );
            m_bUnsupported = true;
            return false;
        }

        const uint32_t uValidBits = families[ g_device.queueFamily() ].timestampValidBits;
        m_ulValidMask = uValidBits >= 64 ? ~0ull : ( ( 1ull << uValidBits ) - 1 );

        VkPhysicalDeviceProperties props;
        g_device.vk.GetPhysicalDeviceProperties( g_device.physDev(), &props );
        m_flPeriodNs = props.limits.timestampPeriod;

        VkQueryPoolCreateInfo createInfo =
        {
            .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .queryType = VK_QUERY_TYPE_TIMESTAMP,
            .queryCount = k_uSlots * k_uQueriesPerSlot,
        };
        VkResult res = g_device.vk.CreateQueryPool( g_device.device(), &createInfo, nullptr, &m_pool );
        if ( res != VK_SUCCESS )
        {
            m_Log.errorf( "vkCreateQueryPool failed: %d", res );
            m_bUnsupported = true;
            return false;
        }
        return true;
    }

    void CGpuTimestamps::Drain()
    {
        while ( !m_Pending.empty() )
        {
            const Pending_t &p = m_Pending.front();
            if ( p.ulSeqNo == 0 || p.ulSeqNo > g_device.completedSeqNo() )
                break;
            // Value then availability per query, 64-bit, begin/mid/end.
            const bool bSplit = !p.sKeyPre.empty();
            const uint32_t uCount = bSplit ? 3 : 2;
            const size_t uEnd = bSplit ? 2 : 1;
            uint64_t ulResults[ k_uQueriesPerSlot * 2 ] = {};
            VkResult res = g_device.vk.GetQueryPoolResults( g_device.device(), m_pool, p.uSlot * k_uQueriesPerSlot, uCount, sizeof( ulResults ), ulResults,
                sizeof( uint64_t ) * 2, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT );
            if ( res != VK_SUCCESS || !ulResults[ 1 ] || !ulResults[ uEnd * 2 + 1 ] )
                break;

            auto Add = [&]( const std::string &sKey, uint64_t ulFrom, uint64_t ulTo )
            {
                Accum_t &a = m_Accum[ sKey ];
                const double flNs = double( ( ulTo - ulFrom ) & m_ulValidMask ) * m_flPeriodNs;
                a.flTotalNs += flNs;
                a.flMaxNs = std::max( a.flMaxNs, flNs );
                a.uCount++;
            };
            Add( p.sKey, ulResults[ 0 ], ulResults[ uEnd * 2 ] );
            if ( bSplit )
            {
                Add( p.sKeyPre, ulResults[ 0 ], ulResults[ 2 ] );
                Add( p.sKeyPost, ulResults[ 2 ], ulResults[ 4 ] );
            }
            m_Pending.pop_front();
        }

        const uint64_t ulNow = get_time_in_nanos();
        if ( ulNow - m_ulLastReportNs < k_ulReportPeriodNs )
            return;
        m_ulLastReportNs = ulNow;
        for ( auto &[ sKey, a ] : m_Accum )
        {
            if ( a.uCount )
                m_Log.debugf( "%s %.0f us avg, %.0f us max over %u", sKey.c_str(), a.flTotalNs / a.uCount / 1000.0, a.flMaxNs / 1000.0, a.uCount );
            a = {};
        }
    }

    std::optional<uint32_t> CGpuTimestamps::Begin( CVulkanCmdBuffer *pCmdBuffer )
    {
        if ( !m_Log.Enabled( LOG_DEBUG ) || !Init() )
            return std::nullopt;

        Drain();
        if ( m_Pending.size() >= k_uSlots )
            return std::nullopt;

        const uint32_t uSlot = m_uNextSlot;
        m_uNextSlot = ( m_uNextSlot + 1 ) % k_uSlots;
        m_bMidWritten[ uSlot ] = false;
        g_device.vk.CmdResetQueryPool( pCmdBuffer->rawBuffer(), m_pool, uSlot * k_uQueriesPerSlot, k_uQueriesPerSlot );
        // Bottom of pipe so the first timestamp lands after earlier submissions drain.
        g_device.vk.CmdWriteTimestamp( pCmdBuffer->rawBuffer(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_pool, uSlot * k_uQueriesPerSlot );
        return uSlot;
    }

    void CGpuTimestamps::Mid( CVulkanCmdBuffer *pCmdBuffer, uint32_t uSlot )
    {
        g_device.vk.CmdWriteTimestamp( pCmdBuffer->rawBuffer(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_pool, uSlot * k_uQueriesPerSlot + 1 );
        m_bMidWritten[ uSlot ] = true;
    }

    void CGpuTimestamps::End( CVulkanCmdBuffer *pCmdBuffer, uint32_t uSlot, const std::string &sKey, const std::string &sKeyPre, const std::string &sKeyPost )
    {
        const bool bMid = m_bMidWritten[ uSlot ];
        g_device.vk.CmdWriteTimestamp( pCmdBuffer->rawBuffer(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, m_pool, uSlot * k_uQueriesPerSlot + ( bMid ? 2 : 1 ) );
        Pending_t pending{ .uSlot = uSlot, .sKey = sKey };
        if ( bMid )
        {
            pending.sKeyPre = sKeyPre;
            pending.sKeyPost = sKeyPost;
        }
        m_Pending.push_back( std::move( pending ) );
    }

    void CGpuTimestamps::Submitted( uint64_t ulSeqNo )
    {
        m_Pending.back().ulSeqNo = ulSeqNo;
    }
}
