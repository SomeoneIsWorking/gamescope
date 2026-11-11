// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Barış Hamil

// The Lossless generator: per real frame the copy, luma pyramid, feature pyramid and UI mask, per
// generated frame three flow fields coarse to fine and the compose (openSU's
// docs/design/frame-generation-spec.md, section 4).

#pragma once

#include "framegen/Device.h"
#include "framegen/LosslessGenerator.h"

#include "LosslessImages.h"
#include "LosslessPipelines.h"
#include "LosslessShaders.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace framegen
{
    inline constexpr float k_flLosslessDefaultUiThreshold = 0.5f;

    class CLosslessGeneratorImpl final : public CLosslessGenerator
    {
    public:
        static std::unique_ptr<CLosslessGeneratorImpl> Create( std::shared_ptr<const CDevice> pDevice, const LosslessSettings_t &settings, std::string &sError );
        ~CLosslessGeneratorImpl() override;

        bool BeginSession( uint32_t uWidth, uint32_t uHeight ) override;
        std::optional<Submission_t> AddRealFrame( const GeneratorImage_t &frame ) override;
        std::optional<Submission_t> Generate( float flTimestamp, const GeneratorImage_t &output, const OutputHooks_t &hooks ) override;
        uint64_t RealFrames() const override { return m_ulRealFrames; }

    private:
        // Submissions in flight before a slot's descriptors and constants are reused.
        static constexpr uint32_t k_uSlots = 16;
        // Most dispatches in one submission: 5 L + 10 L2 + 1 for a generated frame.
        static constexpr uint32_t k_uMaxPassesPerSlot = 5 * k_uLosslessMaxLevels + 10 * k_uLosslessMaxLevels2 + 1;

        // The cb block of every shader, one per level (spec 2.3).
        struct LevelConstants_t
        {
            uint32_t uInputOffset[ 2 ];
            uint32_t uFirstIter;
            uint32_t uFirstIterS;
            uint32_t uAdvancedColorKind;
            uint32_t uHdrSupport;
            float flResolutionInvScale;
            float flTimestamp;
            float flUiThreshold;
            uint32_t uPadding[ 3 ];
        };
        static_assert( sizeof( LevelConstants_t ) == 48 );

        struct Slot_t
        {
            VkDescriptorPool pool = VK_NULL_HANDLE;
            uint64_t ulSeqNo = 0;
        };

        struct Pass_t
        {
            LosslessShader eShader;
            // Null views read as zero (robustness2 null descriptors), like D3D11's null SRVs.
            std::vector<VkImageView> inputs;
            std::vector<VkImageView> outputs;
            LosslessSampler eSampler0 = LosslessSampler::BorderBlack;
            uint32_t uLevel = 0;
            uint32_t uGroupsX = 1;
            uint32_t uGroupsY = 1;
        };

        struct Recording_t
        {
            VkCommandBuffer cmd = VK_NULL_HANDLE;
            uint32_t uSlot = 0;
            std::optional<uint32_t> ouTimingSlot;
            // A pass could not be recorded; the submission still goes out to keep the ring in order.
            bool bFailed = false;
        };

        explicit CLosslessGeneratorImpl( std::shared_ptr<const CDevice> pDevice );

        void Log( LogLevel eLevel, const std::string &sLine ) const { m_pDevice->Log( eLevel, sLine ); }
        void WaitIdle();
        std::optional<Recording_t> BeginRecording( float flTimestamp );
        void Dispatch( Recording_t &recording, const Pass_t &pass );
        uint64_t Submit( Recording_t &recording, const std::string &sTimingKey );
        bool ClearImages();

        void RecordIngest( Recording_t &recording, VkImageView frame );
        void RecordUiMask( Recording_t &recording );
        void RecordGenerate( Recording_t &recording, VkImageView output );
        // Feature images of one real frame's slot at one level, all channels in use.
        void AddFeatures( std::vector<VkImageView> &views, uint32_t uSlot, uint32_t uLevel ) const;
        // Quarter images [uFirst, uFirst + uCount) at one level.
        std::vector<VkImageView> Quarters( uint32_t uFirst, uint32_t uCount, uint32_t uLevel ) const;

        std::shared_ptr<const CDevice> m_pDevice;
        LosslessSettings_t m_Settings;
        bool m_bFp16 = false;
        LosslessModeLayout_t m_Layout;
        std::unique_ptr<CLosslessPipelines> m_pPipelines;
        std::unique_ptr<CLosslessImages> m_pImages;

        VkBuffer m_uniformBuffer = VK_NULL_HANDLE;
        VkDeviceMemory m_uniformMemory = VK_NULL_HANDLE;
        uint8_t *m_pUniformData = nullptr;
        uint32_t m_uUniformStride = 0;

        std::array<Slot_t, k_uSlots> m_Slots;
        uint32_t m_uNextSlot = 0;
        uint64_t m_ulLastSeqNo = 0;

        // s, k of the spec and the real frames of this session.
        uint32_t m_uFrameSlot = 0;
        uint32_t m_uFeatureSlot = 0;
        uint64_t m_ulRealFrames = 0;

        std::string m_sIngestKey;
        std::string m_sGenerateKey;
    };
}
