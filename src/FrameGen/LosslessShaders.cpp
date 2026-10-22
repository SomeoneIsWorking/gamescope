// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Barış Hamil

#include "LosslessShaders.h"

#include "PeResources.h"

#include <cstring>
#include <format>

namespace gamescope::FrameGen
{
    namespace
    {
        constexpr uint32_t k_uCopyResource = 254;
        constexpr uint32_t k_uBalancedFirstResource = 257;
        constexpr uint32_t k_uPerformanceFirstResource = 280;
        // The SPIR-V twin of DXBC resource N is N + 49 (FP16) or N + 98 (FP32).
        constexpr uint32_t k_uFp16ResourceOffset = 49;
        constexpr uint32_t k_uFp32ResourceOffset = 98;

        // cb: InputOffset, FirstIter, FirstIterS, AdvancedColorKind, HdrSupport,
        // ResolutionInvScale, Timestamp, then UiThreshold in the UI mask shader.
        constexpr std::array<uint32_t, 8> k_uUniformOffsets = { 0x00, 0x08, 0x0c, 0x10, 0x14, 0x18, 0x1c, 0x20 };

        struct ShaderRow_t
        {
            const char *pszName;
            uint32_t uLocalSize;
            uint32_t uUniformMembers;
            uint32_t uSamplers;
            // Balanced, then performance.
            uint32_t uInputs[ 2 ];
            uint32_t uOutputs[ 2 ];
            spv::ImageFormat eOutputFormat;
        };

        // Spec 2.3 and the src/dst columns of section 4.
        constexpr std::array<ShaderRow_t, k_uLosslessShaderCount> k_Rows =
        { {
            { "Copy",         8,  7, 0, { 1, 1 },   { 1, 1 }, spv::ImageFormatRgba32f },
            { "LumaPyramid",  32, 7, 1, { 1, 1 },   { 7, 7 }, spv::ImageFormatR8 },
            { "Compose",      16, 7, 2, { 5, 5 },   { 1, 1 }, spv::ImageFormatUnknown },
            { "FlowEntry",    8,  7, 2, { 9, 5 },   { 3, 3 }, spv::ImageFormatRgba8 },
            { "FlowEntryB",   8,  7, 2, { 10, 6 },  { 2, 1 }, spv::ImageFormatRgba8 },
            { "FlowAFilter1", 8,  0, 1, { 3, 3 },   { 4, 2 }, spv::ImageFormatRgba8 },
            { "FlowAFilter2", 8,  0, 1, { 4, 2 },   { 4, 2 }, spv::ImageFormatRgba8 },
            { "FlowAFilter3", 8,  0, 1, { 4, 2 },   { 4, 2 }, spv::ImageFormatRgba8 },
            { "FlowAResolve", 8,  7, 2, { 6, 4 },   { 1, 1 }, spv::ImageFormatRgba16f },
            { "FlowBFilter1", 8,  0, 1, { 3, 3 },   { 4, 2 }, spv::ImageFormatRgba8 },
            { "FlowBFilter2", 8,  0, 1, { 4, 2 },   { 4, 2 }, spv::ImageFormatRgba8 },
            { "FlowBFilter3", 8,  0, 1, { 4, 2 },   { 4, 2 }, spv::ImageFormatRgba8 },
            { "FlowBResolve", 8,  7, 2, { 6, 4 },   { 1, 1 }, spv::ImageFormatRgba16f },
            { "Feature1",     8,  0, 1, { 1, 1 },   { 2, 1 }, spv::ImageFormatRgba8 },
            { "Feature2",     8,  0, 1, { 2, 1 },   { 2, 1 }, spv::ImageFormatRgba8 },
            { "Feature3",     8,  0, 1, { 2, 1 },   { 4, 2 }, spv::ImageFormatRgba8 },
            { "Feature4",     8,  0, 1, { 4, 2 },   { 4, 2 }, spv::ImageFormatRgba8 },
            { "FlowCFilter1", 8,  0, 1, { 2, 1 },   { 2, 1 }, spv::ImageFormatRgba8 },
            { "FlowCFilter2", 8,  0, 1, { 2, 1 },   { 2, 1 }, spv::ImageFormatRgba8 },
            { "FlowCFilter3", 8,  0, 1, { 2, 1 },   { 2, 1 }, spv::ImageFormatRgba8 },
            { "FlowCResolve", 8,  7, 2, { 3, 2 },   { 1, 1 }, spv::ImageFormatRgba16f },
            { "Ui1",          8,  0, 1, { 12, 6 },  { 2, 2 }, spv::ImageFormatRgba8 },
            { "Ui2",          8,  0, 1, { 2, 2 },   { 2, 2 }, spv::ImageFormatRgba8 },
            { "Ui3",          8,  0, 1, { 2, 2 },   { 2, 2 }, spv::ImageFormatRgba8 },
            { "Ui4",          8,  0, 1, { 2, 2 },   { 2, 2 }, spv::ImageFormatRgba8 },
            { "UiMask",       32, 8, 1, { 2, 2 },   { 6, 6 }, spv::ImageFormatR8 },
        } };

        bool IsShared( LosslessShader eShader )
        {
            return eShader == LosslessShader::Copy || eShader == LosslessShader::LumaPyramid || eShader == LosslessShader::Compose;
        }

        std::string OutputName( LosslessShader eShader, uint32_t uIndex )
        {
            if ( eShader == LosslessShader::LumaPyramid )
                return std::format( "Output{}", uIndex );
            if ( eShader == LosslessShader::UiMask )
                return std::format( "OutputMip{}", uIndex );
            return std::format( "Output{}", uIndex + 1 );
        }

        std::map<uint32_t, SpirvDescriptor_t> ExpectedDescriptors( LosslessShader eShader, const LosslessShaderInterface_t &iface )
        {
            std::map<uint32_t, SpirvDescriptor_t> expected;
            if ( iface.uUniformMembers )
            {
                expected[ k_uLosslessUniformBinding ] = SpirvDescriptor_t{
                    .sName = "cb",
                    .eKind = SpirvDescriptorKind::UniformBuffer,
                    .uMemberOffsets = std::vector<uint32_t>( k_uUniformOffsets.begin(), k_uUniformOffsets.begin() + iface.uUniformMembers ),
                };
            }
            for ( uint32_t i = 0; i < iface.uSamplers; i++ )
                expected[ k_uLosslessSamplerBinding + i ] = SpirvDescriptor_t{ .sName = i ? "Sampler1" : "Sampler", .eKind = SpirvDescriptorKind::Sampler };
            for ( uint32_t i = 0; i < iface.uInputs; i++ )
                expected[ k_uLosslessInputBinding + i ] = SpirvDescriptor_t{ .sName = std::format( "Input{}", i + 1 ), .eKind = SpirvDescriptorKind::SampledImage };
            for ( uint32_t i = 0; i < iface.uOutputs; i++ )
            {
                expected[ k_uLosslessOutputBinding + i ] = SpirvDescriptor_t{
                    .sName = OutputName( eShader, i ),
                    .eKind = SpirvDescriptorKind::StorageImage,
                    .eFormat = iface.eOutputFormat,
                };
            }
            return expected;
        }

        std::string Describe( const SpirvDescriptor_t &descriptor )
        {
            std::string s = std::format( "'{}' set {} {}", descriptor.sName, descriptor.uSet, SpirvDescriptorKindName( descriptor.eKind ) );
            if ( descriptor.eKind == SpirvDescriptorKind::StorageImage )
                s += std::format( " format {}", uint32_t( descriptor.eFormat ) );
            for ( uint32_t uOffset : descriptor.uMemberOffsets )
                s += std::format( " +{}", uOffset );
            return s;
        }

        bool SameDescriptor( const SpirvDescriptor_t &a, const SpirvDescriptor_t &b )
        {
            return a.sName == b.sName && a.uSet == b.uSet && a.eKind == b.eKind && a.eFormat == b.eFormat && a.uMemberOffsets == b.uMemberOffsets;
        }

        // The first difference from the spec, or nothing.
        std::optional<std::string> CheckInterface( LosslessShader eShader, const LosslessShaderInterface_t &iface, const SpirvInterface_t &actual )
        {
            const std::array<uint32_t, 3> uExpectedSize = { iface.uLocalSize, iface.uLocalSize, 1 };
            if ( actual.uLocalSize != uExpectedSize )
                return std::format( "local size {}x{}x{}, expected {}x{}x1", actual.uLocalSize[ 0 ], actual.uLocalSize[ 1 ], actual.uLocalSize[ 2 ], iface.uLocalSize, iface.uLocalSize );
            if ( actual.bFloat16 != iface.bFloat16 )
                return std::format( "Float16 capability {}, expected {}", actual.bFloat16, iface.bFloat16 );

            const std::map<uint32_t, SpirvDescriptor_t> expected = ExpectedDescriptors( eShader, iface );
            for ( const auto &[ uBinding, descriptor ] : expected )
            {
                auto it = actual.descriptors.find( uBinding );
                if ( it == actual.descriptors.end() )
                    return std::format( "binding {} missing, expected {}", uBinding, Describe( descriptor ) );
                if ( !SameDescriptor( it->second, descriptor ) )
                    return std::format( "binding {} is {}, expected {}", uBinding, Describe( it->second ), Describe( descriptor ) );
            }
            for ( const auto &[ uBinding, descriptor ] : actual.descriptors )
            {
                if ( !expected.contains( uBinding ) )
                    return std::format( "unexpected binding {}: {}", uBinding, Describe( descriptor ) );
            }
            return std::nullopt;
        }
    }

    LosslessShaderInterface_t GetLosslessShaderInterface( LosslessShader eShader, LosslessSizeMode eMode, bool bFp16 )
    {
        const ShaderRow_t &row = k_Rows[ uint32_t( eShader ) ];
        const uint32_t uMode = uint32_t( eMode );
        return LosslessShaderInterface_t{
            .uLocalSize = row.uLocalSize,
            .uUniformMembers = row.uUniformMembers,
            .uSamplers = row.uSamplers,
            .uInputs = row.uInputs[ uMode ],
            .uOutputs = row.uOutputs[ uMode ],
            .eOutputFormat = row.eOutputFormat,
            .bFloat16 = bFp16 && !IsShared( eShader ),
        };
    }

    const char *LosslessShaderName( LosslessShader eShader )
    {
        return k_Rows[ uint32_t( eShader ) ].pszName;
    }

    uint32_t LosslessShaderResourceId( LosslessShader eShader, LosslessSizeMode eMode, bool bFp16 )
    {
        const uint32_t uIndex = uint32_t( eShader );
        const uint32_t uFirstFlow = uint32_t( LosslessShader::FlowEntry );
        const uint32_t uDxbc = uIndex < uFirstFlow
            ? k_uCopyResource + uIndex
            : ( eMode == LosslessSizeMode::Balanced ? k_uBalancedFirstResource : k_uPerformanceFirstResource ) + uIndex - uFirstFlow;
        return uDxbc + ( bFp16 ? k_uFp16ResourceOffset : k_uFp32ResourceOffset );
    }

    std::optional<LosslessShaderSet_t> LoadLosslessShaders( const std::string &sDllPath, LosslessSizeMode eMode, bool bFp16, std::string &sError )
    {
        std::optional<CPeResources> oPe = CPeResources::Open( sDllPath, sError );
        if ( !oPe )
            return std::nullopt;

        LosslessShaderSet_t set;
        set.sFileVersion = oPe->FileVersion().value_or( "without a version" );
        for ( uint32_t i = 0; i < k_uLosslessShaderCount; i++ )
        {
            const LosslessShader eShader = LosslessShader( i );
            const uint32_t uId = LosslessShaderResourceId( eShader, eMode, bFp16 );
            auto Refuse = [&]( const std::string &sReason )
            {
                sError = std::format( "Lossless.dll {} ({} bytes): resource {} ({}): {}", set.sFileVersion, oPe->FileSize(), uId, LosslessShaderName( eShader ), sReason );
                return std::nullopt;
            };

            const std::optional<std::span<const uint8_t>> oBytes = oPe->Find( k_uPeResourceTypeRcData, uId );
            if ( !oBytes )
                return Refuse( "missing" );
            if ( oBytes->size() % sizeof( uint32_t ) )
                return Refuse( std::format( "{} bytes is not whole SPIR-V words", oBytes->size() ) );

            std::vector<uint32_t> words( oBytes->size() / sizeof( uint32_t ) );
            memcpy( words.data(), oBytes->data(), oBytes->size() );
            std::string sReflectError;
            const std::optional<SpirvInterface_t> oInterface = ReflectSpirv( words, sReflectError );
            if ( !oInterface )
                return Refuse( sReflectError );
            if ( std::optional<std::string> oMismatch = CheckInterface( eShader, GetLosslessShaderInterface( eShader, eMode, bFp16 ), *oInterface ) )
                return Refuse( *oMismatch );

            set.modules[ i ] = std::move( words );
        }
        return set;
    }
}
