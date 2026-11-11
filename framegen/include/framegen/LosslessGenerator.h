// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Barış Hamil

// Motion-interpolated frames from the LSFG 3 shaders in the player's own Lossless.dll, run on
// the host's device (openSU's docs/design/frame-generation-spec.md).

#pragma once

#include "framegen/Device.h"
#include "framegen/LosslessModules.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace framegen
{
    inline constexpr float k_flLosslessMinFlowScale = 0.25f;

    enum class LosslessSizeMode : uint8_t
    {
        Balanced = 0,
        Performance = 1,
    };

    struct LosslessSettings_t
    {
        // The player's Lossless.dll, read when the generator is made, unless pModules holds its
        // modules already.
        std::string sDllPath;
        std::shared_ptr<const LosslessModules_t> pModules;
        LosslessSizeMode eSizeMode = LosslessSizeMode::Balanced;
        // Share of the frame size motion is estimated at, 0.25 to 1.
        float flFlowScale = 1.0f;
        // Lossless Scaling's shipped config.ini default.
        bool bUiDetection = true;

        bool operator==( const LosslessSettings_t & ) const = default;
    };

    // An 8-bit RGBA image of the session's size the generator reads (a real frame) or writes
    // (a generated one) in GENERAL layout, through a view of a UNORM format that allows storage.
    struct GeneratorImage_t
    {
        VkImageView view = VK_NULL_HANDLE;
        uint32_t uWidth = 0;
        uint32_t uHeight = 0;
    };

    // Recorded into the generated frame's command buffer before and after the generator's own
    // work, for a host that tracks the output image's layout.
    struct OutputHooks_t
    {
        std::function<void( VkCommandBuffer )> before;
        std::function<void( VkCommandBuffer )> after;
    };

    // A submission the generator made: its sequence number, and whether every pass in it was
    // recorded (a failed one still goes out, so its images are in use until it is done).
    struct Submission_t
    {
        uint64_t ulSeqNo = 0;
        bool bComplete = false;
    };

    class CLosslessGenerator
    {
    public:
        // Nothing, with the reason, when the DLL, a shader or the device does not match the spec.
        static std::unique_ptr<CLosslessGenerator> Create( std::shared_ptr<const CDevice> pDevice, const LosslessSettings_t &settings, std::string &sError );
        virtual ~CLosslessGenerator() = default;

        // A new size, or real frames left out: forgets every real frame seen before.
        virtual bool BeginSession( uint32_t uWidth, uint32_t uHeight ) = 0;
        // The next real frame of the session; its submission, or nothing when none was made. The
        // image must stay alive until that submission has finished.
        virtual std::optional<Submission_t> AddRealFrame( const GeneratorImage_t &frame ) = 0;
        // Writes the scene at flTimestamp in (0, 1) between the last two real frames into output;
        // its submission, or nothing when none was made.
        virtual std::optional<Submission_t> Generate( float flTimestamp, const GeneratorImage_t &output, const OutputHooks_t &hooks = {} ) = 0;
        // Real frames added since BeginSession.
        virtual uint64_t RealFrames() const = 0;

    protected:
        CLosslessGenerator() = default;
        CLosslessGenerator( const CLosslessGenerator & ) = delete;
        CLosslessGenerator &operator=( const CLosslessGenerator & ) = delete;
    };
}
