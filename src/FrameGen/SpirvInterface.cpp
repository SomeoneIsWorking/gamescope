// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Barış Hamil

#include "SpirvInterface.h"

#include <format>
#include <unordered_map>

namespace gamescope::FrameGen
{
    const char *SpirvDescriptorKindName( SpirvDescriptorKind eKind )
    {
        switch ( eKind )
        {
            case SpirvDescriptorKind::UniformBuffer: return "uniform buffer";
            case SpirvDescriptorKind::Sampler:       return "sampler";
            case SpirvDescriptorKind::SampledImage:  return "sampled image";
            case SpirvDescriptorKind::StorageImage:  return "storage image";
        }
        return "?";
    }

    namespace
    {
        struct Image_t
        {
            uint32_t uSampled = 0;
            spv::ImageFormat eFormat = spv::ImageFormatUnknown;
        };

        struct Pointer_t
        {
            spv::StorageClass eStorage = spv::StorageClassMax;
            uint32_t uPointee = 0;
        };

        struct Variable_t
        {
            uint32_t uPointerType = 0;
            spv::StorageClass eStorage = spv::StorageClassMax;
        };

        // A literal string starting at word uFirst of an instruction ending before uEnd.
        std::string ReadString( std::span<const uint32_t> words, size_t uFirst, size_t uEnd )
        {
            std::string sValue;
            for ( size_t i = uFirst; i < uEnd; i++ )
            {
                for ( uint32_t uByte = 0; uByte < 4; uByte++ )
                {
                    const char c = char( ( words[ i ] >> ( uByte * 8 ) ) & 0xff );
                    if ( c == '\0' )
                        return sValue;
                    sValue += c;
                }
            }
            return sValue;
        }
    }

    std::optional<SpirvInterface_t> ReflectSpirv( std::span<const uint32_t> words, std::string &sError )
    {
        static constexpr size_t k_uHeaderWords = 5;
        if ( words.size() < k_uHeaderWords || words[ 0 ] != spv::MagicNumber )
        {
            sError = "not a SPIR-V module";
            return std::nullopt;
        }

        SpirvInterface_t result;
        uint32_t uComputeEntryPoints = 0;
        bool bLocalSize = false;
        std::unordered_map<uint32_t, std::string> names;
        std::unordered_map<uint32_t, uint32_t> bindings;
        std::unordered_map<uint32_t, uint32_t> sets;
        std::unordered_map<uint32_t, std::map<uint32_t, uint32_t>> memberOffsets;
        std::unordered_map<uint32_t, Image_t> images;
        std::unordered_map<uint32_t, uint32_t> structMembers;
        std::unordered_map<uint32_t, Pointer_t> pointers;
        std::unordered_map<uint32_t, Variable_t> variables;
        std::unordered_map<uint32_t, bool> samplers;

        for ( size_t i = k_uHeaderWords; i < words.size(); )
        {
            const uint32_t uWordCount = words[ i ] >> spv::WordCountShift;
            const spv::Op eOp = spv::Op( words[ i ] & spv::OpCodeMask );
            if ( uWordCount == 0 || i + uWordCount > words.size() )
            {
                sError = std::format( "malformed instruction at word {}", i );
                return std::nullopt;
            }
            const size_t uEnd = i + uWordCount;
            auto Word = [&]( uint32_t uIndex ) { return i + uIndex < uEnd ? words[ i + uIndex ] : 0u; };

            switch ( eOp )
            {
                case spv::OpCapability:
                    if ( Word( 1 ) == spv::CapabilityFloat16 )
                        result.bFloat16 = true;
                    break;
                case spv::OpEntryPoint:
                    if ( Word( 1 ) == spv::ExecutionModelGLCompute )
                        uComputeEntryPoints++;
                    break;
                case spv::OpExecutionMode:
                    if ( Word( 2 ) == spv::ExecutionModeLocalSize && uWordCount >= 6 )
                    {
                        result.uLocalSize = { Word( 3 ), Word( 4 ), Word( 5 ) };
                        bLocalSize = true;
                    }
                    break;
                case spv::OpName:
                    names[ Word( 1 ) ] = ReadString( words, i + 2, uEnd );
                    break;
                case spv::OpDecorate:
                    if ( Word( 2 ) == spv::DecorationBinding )
                        bindings[ Word( 1 ) ] = Word( 3 );
                    else if ( Word( 2 ) == spv::DecorationDescriptorSet )
                        sets[ Word( 1 ) ] = Word( 3 );
                    break;
                case spv::OpMemberDecorate:
                    if ( Word( 3 ) == spv::DecorationOffset )
                        memberOffsets[ Word( 1 ) ][ Word( 2 ) ] = Word( 4 );
                    break;
                case spv::OpTypeImage:
                    images[ Word( 1 ) ] = Image_t{ .uSampled = Word( 7 ), .eFormat = spv::ImageFormat( Word( 8 ) ) };
                    break;
                case spv::OpTypeSampler:
                    samplers[ Word( 1 ) ] = true;
                    break;
                case spv::OpTypeStruct:
                    structMembers[ Word( 1 ) ] = uWordCount - 2;
                    break;
                case spv::OpTypePointer:
                    pointers[ Word( 1 ) ] = Pointer_t{ .eStorage = spv::StorageClass( Word( 2 ) ), .uPointee = Word( 3 ) };
                    break;
                case spv::OpVariable:
                    variables[ Word( 2 ) ] = Variable_t{ .uPointerType = Word( 1 ), .eStorage = spv::StorageClass( Word( 3 ) ) };
                    break;
                default:
                    break;
            }
            i = uEnd;
        }

        if ( uComputeEntryPoints != 1 || !bLocalSize )
        {
            sError = std::format( "{} compute entry points, local size {}", uComputeEntryPoints, bLocalSize ? "set" : "missing" );
            return std::nullopt;
        }

        for ( const auto &[ uId, uBinding ] : bindings )
        {
            auto itVariable = variables.find( uId );
            auto itPointer = itVariable != variables.end() ? pointers.find( itVariable->second.uPointerType ) : pointers.end();
            if ( itPointer == pointers.end() )
            {
                sError = std::format( "binding {} is not a variable", uBinding );
                return std::nullopt;
            }

            SpirvDescriptor_t descriptor;
            descriptor.sName = names.contains( uId ) ? names[ uId ] : std::string{};
            descriptor.uSet = sets.contains( uId ) ? sets[ uId ] : 0;
            const uint32_t uPointee = itPointer->second.uPointee;
            if ( auto itImage = images.find( uPointee ); itImage != images.end() && itImage->second.uSampled == 1 )
            {
                descriptor.eKind = SpirvDescriptorKind::SampledImage;
            }
            else if ( itImage != images.end() && itImage->second.uSampled == 2 )
            {
                descriptor.eKind = SpirvDescriptorKind::StorageImage;
                descriptor.eFormat = itImage->second.eFormat;
            }
            else if ( samplers.contains( uPointee ) )
            {
                descriptor.eKind = SpirvDescriptorKind::Sampler;
            }
            else if ( structMembers.contains( uPointee ) && itVariable->second.eStorage == spv::StorageClassUniform )
            {
                descriptor.eKind = SpirvDescriptorKind::UniformBuffer;
                for ( uint32_t uMember = 0; uMember < structMembers[ uPointee ]; uMember++ )
                {
                    auto itOffset = memberOffsets[ uPointee ].find( uMember );
                    if ( itOffset == memberOffsets[ uPointee ].end() )
                    {
                        sError = std::format( "binding {} member {} has no offset", uBinding, uMember );
                        return std::nullopt;
                    }
                    descriptor.uMemberOffsets.push_back( itOffset->second );
                }
            }
            else
            {
                sError = std::format( "binding {} ('{}') has an unsupported type", uBinding, descriptor.sName );
                return std::nullopt;
            }

            if ( !result.descriptors.emplace( uBinding, std::move( descriptor ) ).second )
            {
                sError = std::format( "binding {} is used twice", uBinding );
                return std::nullopt;
            }
        }
        return result;
    }
}
