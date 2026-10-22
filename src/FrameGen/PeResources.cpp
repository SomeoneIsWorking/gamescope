// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Barış Hamil

#include "PeResources.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <fstream>
#include <iterator>

namespace gamescope::FrameGen
{
    // Microsoft PE/COFF specification: field offsets used below.
    static constexpr size_t k_uDosNewHeaderOffset = 0x3c;
    static constexpr size_t k_uCoffHeaderSize = 20;
    static constexpr size_t k_uSectionHeaderSize = 40;
    static constexpr uint32_t k_uPe32Magic = 0x10b;
    static constexpr uint32_t k_uPe32PlusMagic = 0x20b;
    static constexpr uint32_t k_uResourceDirectoryIndex = 2;
    static constexpr uint32_t k_uSubdirectoryBit = 0x80000000u;
    static constexpr uint32_t k_uFixedFileInfoSignature = 0xfeef04bdu;

    std::optional<uint32_t> CPeResources::ReadU16( size_t uOffset ) const
    {
        if ( uOffset + 2 > m_Bytes.size() )
            return std::nullopt;
        return uint32_t( m_Bytes[ uOffset ] ) | uint32_t( m_Bytes[ uOffset + 1 ] ) << 8;
    }

    std::optional<uint32_t> CPeResources::ReadU32( size_t uOffset ) const
    {
        if ( uOffset + 4 > m_Bytes.size() )
            return std::nullopt;
        uint32_t uValue;
        memcpy( &uValue, &m_Bytes[ uOffset ], sizeof( uValue ) );
        return uValue;
    }

    std::optional<size_t> CPeResources::RvaToOffset( uint32_t uRva, uint32_t uSize ) const
    {
        for ( const Section_t &section : m_Sections )
        {
            const uint64_t ulEnd = uint64_t( section.uVirtualAddress ) + std::max( section.uVirtualSize, section.uRawSize );
            if ( uRva < section.uVirtualAddress || uint64_t( uRva ) + uSize > ulEnd )
                continue;
            const uint32_t uDelta = uRva - section.uVirtualAddress;
            if ( uint64_t( uDelta ) + uSize > section.uRawSize || uint64_t( section.uRawOffset ) + uDelta + uSize > m_Bytes.size() )
                return std::nullopt;
            return size_t( section.uRawOffset ) + uDelta;
        }
        return std::nullopt;
    }

    std::optional<CPeResources> CPeResources::Open( const std::string &sPath, std::string &sError )
    {
        std::ifstream file( sPath, std::ios::binary );
        if ( !file )
        {
            sError = std::format( "cannot open '{}'", sPath );
            return std::nullopt;
        }

        CPeResources pe;
        pe.m_Bytes.assign( std::istreambuf_iterator<char>( file ), std::istreambuf_iterator<char>() );
        if ( pe.m_Bytes.size() < 2 || pe.m_Bytes[ 0 ] != 'M' || pe.m_Bytes[ 1 ] != 'Z' )
        {
            sError = std::format( "'{}' is not a PE file", sPath );
            return std::nullopt;
        }

        const std::optional<uint32_t> ouNewHeader = pe.ReadU32( k_uDosNewHeaderOffset );
        if ( !ouNewHeader || pe.ReadU32( *ouNewHeader ) != 0x00004550u )
        {
            sError = std::format( "'{}' has no PE header", sPath );
            return std::nullopt;
        }

        const size_t uCoff = *ouNewHeader + 4;
        const std::optional<uint32_t> ouSectionCount = pe.ReadU16( uCoff + 2 );
        const std::optional<uint32_t> ouOptionalSize = pe.ReadU16( uCoff + 16 );
        const size_t uOptional = uCoff + k_uCoffHeaderSize;
        const std::optional<uint32_t> ouMagic = pe.ReadU16( uOptional );
        if ( !ouSectionCount || !ouOptionalSize || !ouMagic || ( *ouMagic != k_uPe32Magic && *ouMagic != k_uPe32PlusMagic ) )
        {
            sError = std::format( "'{}' has an unknown optional header", sPath );
            return std::nullopt;
        }

        const size_t uDirectoryCountOffset = uOptional + ( *ouMagic == k_uPe32PlusMagic ? 108 : 92 );
        const size_t uDirectories = uDirectoryCountOffset + 4;
        const std::optional<uint32_t> ouDirectoryCount = pe.ReadU32( uDirectoryCountOffset );
        const std::optional<uint32_t> ouResourceRva = pe.ReadU32( uDirectories + size_t( k_uResourceDirectoryIndex ) * 8 );
        const std::optional<uint32_t> ouResourceSize = pe.ReadU32( uDirectories + size_t( k_uResourceDirectoryIndex ) * 8 + 4 );
        if ( !ouDirectoryCount || *ouDirectoryCount <= k_uResourceDirectoryIndex || !ouResourceRva || !ouResourceSize || !*ouResourceRva )
        {
            sError = std::format( "'{}' has no resources", sPath );
            return std::nullopt;
        }

        const size_t uSections = uOptional + *ouOptionalSize;
        for ( uint32_t i = 0; i < *ouSectionCount; i++ )
        {
            const size_t uHeader = uSections + size_t( i ) * k_uSectionHeaderSize;
            const std::optional<uint32_t> ouVirtualSize = pe.ReadU32( uHeader + 8 );
            const std::optional<uint32_t> ouVirtualAddress = pe.ReadU32( uHeader + 12 );
            const std::optional<uint32_t> ouRawSize = pe.ReadU32( uHeader + 16 );
            const std::optional<uint32_t> ouRawOffset = pe.ReadU32( uHeader + 20 );
            if ( !ouVirtualSize || !ouVirtualAddress || !ouRawSize || !ouRawOffset )
            {
                sError = std::format( "'{}' has a truncated section table", sPath );
                return std::nullopt;
            }
            pe.m_Sections.push_back( { *ouVirtualAddress, *ouVirtualSize, *ouRawOffset, *ouRawSize } );
        }

        const std::optional<size_t> ouResourceOffset = pe.RvaToOffset( *ouResourceRva, *ouResourceSize );
        if ( !ouResourceOffset )
        {
            sError = std::format( "'{}' has its resources outside every section", sPath );
            return std::nullopt;
        }
        pe.m_uResourceOffset = *ouResourceOffset;
        pe.m_uResourceSize = *ouResourceSize;
        return pe;
    }

    std::optional<uint32_t> CPeResources::FindEntry( uint32_t uDirectory, std::optional<uint32_t> ouId ) const
    {
        const size_t uBase = m_uResourceOffset + uDirectory;
        const std::optional<uint32_t> ouNamed = ReadU16( uBase + 12 );
        const std::optional<uint32_t> ouIds = ReadU16( uBase + 14 );
        if ( uDirectory >= m_uResourceSize || !ouNamed || !ouIds )
            return std::nullopt;

        // Named entries come first; resources looked up here have integer ids.
        const uint32_t uFirst = ouId ? *ouNamed : 0;
        for ( uint32_t i = uFirst; i < *ouNamed + *ouIds; i++ )
        {
            const size_t uEntry = uBase + 16 + size_t( i ) * 8;
            const std::optional<uint32_t> ouName = ReadU32( uEntry );
            const std::optional<uint32_t> ouData = ReadU32( uEntry + 4 );
            if ( !ouName || !ouData )
                return std::nullopt;
            if ( !ouId || *ouName == *ouId )
                return ouData;
        }
        return std::nullopt;
    }

    std::optional<std::span<const uint8_t>> CPeResources::Find( uint32_t uType, uint32_t uId ) const
    {
        const std::optional<uint32_t> ouType = FindEntry( 0, uType );
        if ( !ouType || !( *ouType & k_uSubdirectoryBit ) )
            return std::nullopt;
        const std::optional<uint32_t> ouName = FindEntry( *ouType & ~k_uSubdirectoryBit, uId );
        if ( !ouName || !( *ouName & k_uSubdirectoryBit ) )
            return std::nullopt;
        const std::optional<uint32_t> ouLanguage = FindEntry( *ouName & ~k_uSubdirectoryBit, std::nullopt );
        if ( !ouLanguage || ( *ouLanguage & k_uSubdirectoryBit ) || *ouLanguage >= m_uResourceSize )
            return std::nullopt;

        const std::optional<uint32_t> ouDataRva = ReadU32( m_uResourceOffset + *ouLanguage );
        const std::optional<uint32_t> ouDataSize = ReadU32( m_uResourceOffset + *ouLanguage + 4 );
        if ( !ouDataRva || !ouDataSize )
            return std::nullopt;
        const std::optional<size_t> ouOffset = RvaToOffset( *ouDataRva, *ouDataSize );
        if ( !ouOffset )
            return std::nullopt;
        return std::span<const uint8_t>{ m_Bytes.data() + *ouOffset, *ouDataSize };
    }

    std::optional<std::string> CPeResources::FileVersion() const
    {
        const std::optional<std::span<const uint8_t>> oVersion = Find( k_uPeResourceTypeVersion, 1 );
        if ( !oVersion )
            return std::nullopt;

        // VS_FIXEDFILEINFO: signature, struct version, FileVersionMS, FileVersionLS.
        for ( size_t i = 0; i + 16 <= oVersion->size(); i += 4 )
        {
            uint32_t uWords[ 4 ];
            memcpy( uWords, oVersion->data() + i, sizeof( uWords ) );
            if ( uWords[ 0 ] != k_uFixedFileInfoSignature )
                continue;
            return std::format( "{}.{}.{}.{}", uWords[ 2 ] >> 16, uWords[ 2 ] & 0xffff, uWords[ 3 ] >> 16, uWords[ 3 ] & 0xffff );
        }
        return std::nullopt;
    }
}
