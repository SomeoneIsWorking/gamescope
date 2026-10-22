// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Barış Hamil

// Reads the resources of a PE (Windows DLL) file in place: the bytes of a resource by type
// and integer id, and the FileVersion of its version resource.

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace gamescope::FrameGen
{
    inline constexpr uint32_t k_uPeResourceTypeVersion = 16;
    inline constexpr uint32_t k_uPeResourceTypeRcData = 10;

    class CPeResources
    {
    public:
        // Reads the whole file; nothing when it is not a PE image with a resource directory.
        static std::optional<CPeResources> Open( const std::string &sPath, std::string &sError );

        // The first language of resource (uType, uId), or nothing when absent.
        std::optional<std::span<const uint8_t>> Find( uint32_t uType, uint32_t uId ) const;

        // "a.b.c.d" from the fixed file info of version resource 1.
        std::optional<std::string> FileVersion() const;

        size_t FileSize() const { return m_Bytes.size(); }

    private:
        struct Section_t
        {
            uint32_t uVirtualAddress = 0;
            uint32_t uVirtualSize = 0;
            uint32_t uRawOffset = 0;
            uint32_t uRawSize = 0;
        };

        std::optional<uint32_t> ReadU16( size_t uOffset ) const;
        std::optional<uint32_t> ReadU32( size_t uOffset ) const;
        std::optional<size_t> RvaToOffset( uint32_t uRva, uint32_t uSize ) const;
        // The entry with integer id uId (or the first when nullopt) of the directory at uDirectory.
        std::optional<uint32_t> FindEntry( uint32_t uDirectory, std::optional<uint32_t> ouId ) const;

        std::vector<uint8_t> m_Bytes;
        std::vector<Section_t> m_Sections;
        // File offset and size of the resource directory.
        size_t m_uResourceOffset = 0;
        uint32_t m_uResourceSize = 0;
    };
}
