// GPU timestamps around recorded work, read back once its submission is done and averaged per
// key every two seconds on the owner's log, so measuring never stalls the queue.

#pragma once

#include "log.hpp"
#include "rendervulkan.hpp"

#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>

namespace gamescope
{
    class CGpuTimestamps
    {
    public:
        // Measures only while the log is at debug verbosity.
        explicit CGpuTimestamps( LogScope &log );
        ~CGpuTimestamps();

        CGpuTimestamps( const CGpuTimestamps & ) = delete;
        CGpuTimestamps &operator=( const CGpuTimestamps & ) = delete;

        // The slot that Mid and End take, or nothing when not measuring.
        std::optional<uint32_t> Begin( CVulkanCmdBuffer *pCmdBuffer );
        // Splits the span into a pre and a post part.
        void Mid( CVulkanCmdBuffer *pCmdBuffer, uint32_t uSlot );
        void End( CVulkanCmdBuffer *pCmdBuffer, uint32_t uSlot, const std::string &sKey, const std::string &sKeyPre = {}, const std::string &sKeyPost = {} );
        // The submission that carries the span End closed last.
        void Submitted( uint64_t ulSeqNo );

    private:
        static constexpr uint32_t k_uSlots = 64;
        static constexpr uint32_t k_uQueriesPerSlot = 3;
        static constexpr uint64_t k_ulReportPeriodNs = 2'000'000'000ull;

        struct Pending_t
        {
            uint32_t uSlot = 0;
            std::string sKey;
            // Set when Mid split the span.
            std::string sKeyPre;
            std::string sKeyPost;
            // A reused slot reports its previous use as available until the reset executes,
            // so results are read only once the submission is done.
            uint64_t ulSeqNo = 0;
        };

        struct Accum_t
        {
            double flTotalNs = 0.0;
            double flMaxNs = 0.0;
            uint32_t uCount = 0;
        };

        bool Init();
        void Drain();

        LogScope &m_Log;
        VkQueryPool m_pool = VK_NULL_HANDLE;
        float m_flPeriodNs = 0.0f;
        uint64_t m_ulValidMask = ~0ull;
        bool m_bUnsupported = false;
        uint32_t m_uNextSlot = 0;
        std::deque<Pending_t> m_Pending;
        bool m_bMidWritten[ k_uSlots ] = {};
        std::map<std::string, Accum_t> m_Accum;
        uint64_t m_ulLastReportNs = 0;
    };
}
