// Frame generation for the focused window: keeps copies of its last two committed frames,
// has a synthesizer make the in-between images, and picks the image its base layer shows at
// each vblank. Driven by the GAMESCOPE_FRAMEGEN* root properties.

#pragma once

#include "FramePacer.h"
#include "FrameSynthesizer.h"

#include "gamescope_shared.h"
#include "rc.h"

#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class CVulkanTexture;
struct commit_t;
struct steamcompmgr_win_t;

namespace gamescope::FrameGen
{
    // Real frames, and intervals of generated images, kept at once: an output may still be
    // queued when the second real frame after its own arrives.
    inline constexpr uint64_t k_ulBufferedFrames = 3;

    struct Settings_t
    {
        bool bEnabled = false;
        // The player's Lossless.dll, for a synthesizer that runs its shaders.
        std::string sDllPath;
        PacingSettings_t pacing;

        bool operator==( const Settings_t & ) const = default;
    };

    struct DisplayImage_t
    {
        gamescope::Rc<CVulkanTexture> pTexture;
        GamescopeAppTextureColorspace eColorspace = GAMESCOPE_APP_TEXTURE_COLORSPACE_SRGB;
    };

    class CFrameGenerator
    {
    public:
        static CFrameGenerator &Get();

        void SetSettings( const Settings_t &settings );

        // Called after done commits are handled. Copies a new frame of the focused window
        // and generates the images between it and the one before.
        void Update( steamcompmgr_win_t *pWindow, const gamescope::Rc<commit_t> &pCommit, uint64_t ulNowNs, uint64_t ulRefreshNs );

        // Picks the image shown from this vblank. True when it changed and needs a repaint.
        bool OnVBlank( uint64_t ulNowNs, uint64_t ulRefreshNs );

        // The image to paint for the window's own content, while frame generation owns it.
        std::optional<DisplayImage_t> GetDisplayImage( const steamcompmgr_win_t *pWindow ) const;

        // Changes whenever the shown image does.
        uint64_t GetDisplaySerial() const { return m_ulDisplaySerial; }

    private:
        CFrameGenerator();

        struct Interval_t
        {
            uint64_t ulFrame = 0;
            // The submission that writes each generated image.
            std::vector<uint64_t> ulSequences;
        };

        bool BeginSession( steamcompmgr_win_t *pWindow, uint32_t uWidth, uint32_t uHeight );
        void EndSession();
        bool Capture( const gamescope::Rc<commit_t> &pCommit, uint64_t ulNowNs, uint64_t ulRefreshNs );
        bool IsReady( const PacedOutput_t &output ) const;
        gamescope::Rc<CVulkanTexture> GetTexture( const PacedOutput_t &output ) const;
        void LogStats( uint64_t ulNowNs );

        Settings_t m_Settings;
        std::unique_ptr<IFrameSynthesizer> m_pSynthesizer;

        // The session: one window at one size.
        const steamcompmgr_win_t *m_pWindow = nullptr;
        uint64_t m_ulWindowSeq = 0;
        uint32_t m_uWidth = 0;
        uint32_t m_uHeight = 0;
        uint32_t m_uMaxGenerated = 0;
        std::array<gamescope::Rc<CVulkanTexture>, k_ulBufferedFrames> m_pSources;
        std::vector<gamescope::Rc<CVulkanTexture>> m_pDestinations;
        uint64_t m_ulRealFrames = 0;
        uint64_t m_ulLastCommitId = 0;
        GamescopeAppTextureColorspace m_eColorspace = GAMESCOPE_APP_TEXTURE_COLORSPACE_SRGB;
        std::deque<Interval_t> m_Intervals;
        CFramePacer m_Pacer;
        std::optional<PacingState> m_oePacingState;

        std::optional<PacedOutput_t> m_oDisplay;
        uint64_t m_ulDisplaySerial = 0;

        uint64_t m_ulStatsStartNs = 0;
        uint64_t m_ulStatsReal = 0;
        uint64_t m_ulStatsShown = 0;
        uint64_t m_ulStatsShownGenerated = 0;
        uint64_t m_ulStatsDroppedBase = 0;
    };
}
