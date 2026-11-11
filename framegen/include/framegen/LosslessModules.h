// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Barış Hamil

// The LSFG 3 SPIR-V modules of a Lossless.dll (RT_RCDATA resources 303 to 400: both size modes, FP16
// and FP32), read in place from the DLL, or as a host that read them handed them on.

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace framegen
{
    inline constexpr uint32_t k_uLosslessFirstModule = 303;
    inline constexpr uint32_t k_uLosslessLastModule = 400;

    struct LosslessModules_t
    {
        // The DLL's FileVersion, or "without a version".
        std::string sFileVersion;
        uint64_t ulDllSize = 0;
        // Resource id to its bytes; an id the DLL lacks is absent.
        std::map<uint32_t, std::vector<uint8_t>> resources;
    };

    // Nothing, with the reason, when the file is not a PE with resources.
    std::optional<LosslessModules_t> ReadLosslessModules( const std::string &sDllPath, std::string &sError );
}
