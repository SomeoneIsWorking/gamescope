// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Barış Hamil

// The framegen library's Lossless generator on Gamescope's device: its queue is Gamescope's, its
// log the "lossless" scope, and its frames Gamescope's textures, kept alive until the GPU is done
// with them.

#pragma once

#include "FrameSynthesizer.h"

#include "framegen/Device.h"
#include "framegen/LosslessGenerator.h"

#include "GpuTimestamps.h"
#include "log.hpp"
#include "rendervulkan.hpp"

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace gamescope::FrameGen
{
    using framegen::LosslessSettings_t;
    using framegen::LosslessSizeMode;

    // Gamescope's command buffers and sequence numbers behind the library's queue.
    class CGamescopeQueue final : public framegen::IQueue
    {
    public:
        explicit CGamescopeQueue( LogScope &log );

        VkCommandBuffer Begin() override;
        uint64_t Submit( VkCommandBuffer cmd ) override;
        uint64_t Completed() override;
        // Never Gamescope's newest submission while it records: wait() would rewind its upload ring.
        void Wait( uint64_t ulSeqNo ) override;

        std::optional<uint32_t> BeginTiming( VkCommandBuffer cmd ) override;
        void EndTiming( VkCommandBuffer cmd, uint32_t uSlot, const std::string &sKey ) override;
        void TimingSubmitted( uint64_t ulSeqNo ) override;

        // The Gamescope buffer behind a raw one Begin gave and Submit has not taken yet.
        CVulkanCmdBuffer *Recording( VkCommandBuffer cmd ) const;

    private:
        std::map<VkCommandBuffer, std::unique_ptr<CVulkanCmdBuffer>> m_Recording;
        CGpuTimestamps m_Timestamps;
    };

    class CLosslessSynthesizer final : public IFrameSynthesizer
    {
    public:
        // Nothing, with the reason, when the DLL, a shader or the device does not match the spec.
        static std::unique_ptr<CLosslessSynthesizer> Create( const LosslessSettings_t &settings, std::string &sError );
        ~CLosslessSynthesizer() override;

        bool BeginSession( uint32_t uWidth, uint32_t uHeight ) override;
        bool AddRealFrame( const gamescope::Rc<CVulkanTexture> &pFrame ) override;
        std::optional<uint64_t> Generate( const gamescope::Rc<CVulkanTexture> &pPrevious,
            const gamescope::Rc<CVulkanTexture> &pCurrent, float flTimestamp, const gamescope::Rc<CVulkanTexture> &pOutput ) override;

    private:
        struct InFlight_t
        {
            uint64_t ulSeqNo = 0;
            gamescope::Rc<CVulkanTexture> pTexture;
        };

        CLosslessSynthesizer() = default;

        // Keeps pTexture until submission ulSeqNo is done, and lets go of finished ones.
        void Hold( uint64_t ulSeqNo, const gamescope::Rc<CVulkanTexture> &pTexture );

        std::unique_ptr<CGamescopeQueue> m_pQueue;
        std::unique_ptr<framegen::CLosslessGenerator> m_pGenerator;
        std::deque<InFlight_t> m_InFlight;
        const CVulkanTexture *m_pLastFrame = nullptr;
    };
}
