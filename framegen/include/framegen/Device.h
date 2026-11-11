// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Barış Hamil

// The Vulkan device a generator runs on, as its host gives it: the handles, the features the
// shaders need, the queue the work is submitted to, and where log lines go.

#pragma once

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace framegen
{
    enum class LogLevel : uint8_t
    {
        Debug,
        Info,
        Error,
    };

    // One formatted line, without a trailing newline.
    using LogSink_t = std::function<void( LogLevel eLevel, const std::string &sLine )>;

    // The host's queue. Every command buffer the generator records goes through it, in order.
    class IQueue
    {
    public:
        virtual ~IQueue() = default;

        // A primary command buffer in the recording state, or VK_NULL_HANDLE.
        virtual VkCommandBuffer Begin() = 0;
        // Ends and submits a buffer Begin gave; its sequence number, counting up.
        virtual uint64_t Submit( VkCommandBuffer cmd ) = 0;
        // The newest sequence number the GPU has finished.
        virtual uint64_t Completed() = 0;
        // Returns once ulSeqNo has finished.
        virtual void Wait( uint64_t ulSeqNo ) = 0;

        // GPU time of recorded work, for a host that measures it: the span's slot, or nothing.
        virtual std::optional<uint32_t> BeginTiming( VkCommandBuffer ) { return std::nullopt; }
        virtual void EndTiming( VkCommandBuffer, uint32_t, const std::string & ) {}
        // The submission that carries the span EndTiming closed last.
        virtual void TimingSubmitted( uint64_t ) {}
    };

    struct DeviceDesc_t
    {
        PFN_vkGetInstanceProcAddr pfnGetInstanceProcAddr = nullptr;
        VkInstance instance = VK_NULL_HANDLE;
        VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
        VkDevice device = VK_NULL_HANDLE;
        // Enabled on the device: shaderFloat16, shaderStorageImageWriteWithoutFormat,
        // shaderStorageImageExtendedFormats.
        bool bFloat16 = false;
        bool bStorageWriteWithoutFormat = false;
        bool bStorageExtendedFormats = false;
        IQueue *pQueue = nullptr;
        LogSink_t log;
    };

#define FRAMEGEN_VK_FUNCTIONS \
    FRAMEGEN_VK_FUNC( AllocateDescriptorSets ) \
    FRAMEGEN_VK_FUNC( AllocateMemory ) \
    FRAMEGEN_VK_FUNC( BindBufferMemory ) \
    FRAMEGEN_VK_FUNC( BindImageMemory ) \
    FRAMEGEN_VK_FUNC( CmdBindDescriptorSets ) \
    FRAMEGEN_VK_FUNC( CmdBindPipeline ) \
    FRAMEGEN_VK_FUNC( CmdClearColorImage ) \
    FRAMEGEN_VK_FUNC( CmdDispatch ) \
    FRAMEGEN_VK_FUNC( CmdPipelineBarrier ) \
    FRAMEGEN_VK_FUNC( CreateBuffer ) \
    FRAMEGEN_VK_FUNC( CreateComputePipelines ) \
    FRAMEGEN_VK_FUNC( CreateDescriptorPool ) \
    FRAMEGEN_VK_FUNC( CreateDescriptorSetLayout ) \
    FRAMEGEN_VK_FUNC( CreateImage ) \
    FRAMEGEN_VK_FUNC( CreateImageView ) \
    FRAMEGEN_VK_FUNC( CreatePipelineLayout ) \
    FRAMEGEN_VK_FUNC( CreateSampler ) \
    FRAMEGEN_VK_FUNC( CreateShaderModule ) \
    FRAMEGEN_VK_FUNC( DestroyBuffer ) \
    FRAMEGEN_VK_FUNC( DestroyDescriptorPool ) \
    FRAMEGEN_VK_FUNC( DestroyDescriptorSetLayout ) \
    FRAMEGEN_VK_FUNC( DestroyImage ) \
    FRAMEGEN_VK_FUNC( DestroyImageView ) \
    FRAMEGEN_VK_FUNC( DestroyPipeline ) \
    FRAMEGEN_VK_FUNC( DestroyPipelineLayout ) \
    FRAMEGEN_VK_FUNC( DestroySampler ) \
    FRAMEGEN_VK_FUNC( DestroyShaderModule ) \
    FRAMEGEN_VK_FUNC( FreeMemory ) \
    FRAMEGEN_VK_FUNC( GetBufferMemoryRequirements ) \
    FRAMEGEN_VK_FUNC( GetImageMemoryRequirements ) \
    FRAMEGEN_VK_FUNC( MapMemory ) \
    FRAMEGEN_VK_FUNC( ResetDescriptorPool ) \
    FRAMEGEN_VK_FUNC( UpdateDescriptorSets )

    struct VulkanFunctions_t
    {
#define FRAMEGEN_VK_FUNC( x ) PFN_vk##x x = nullptr;
        FRAMEGEN_VK_FUNCTIONS
#undef FRAMEGEN_VK_FUNC
    };

    // The generator's view of the host's device, shared by everything it makes.
    class CDevice
    {
    public:
        // Nothing, with the reason, when a function is missing.
        static std::shared_ptr<CDevice> Create( const DeviceDesc_t &desc, std::string &sError );

        const VulkanFunctions_t &vk() const { return m_Functions; }
        VkDevice Device() const { return m_Desc.device; }
        VkPhysicalDevice PhysicalDevice() const { return m_Desc.physicalDevice; }
        IQueue &Queue() const { return *m_Desc.pQueue; }
        bool SupportsFloat16() const { return m_Desc.bFloat16; }
        bool SupportsStorageWriteWithoutFormat() const { return m_Desc.bStorageWriteWithoutFormat; }
        bool SupportsStorageExtendedFormats() const { return m_Desc.bStorageExtendedFormats; }

        // The first memory type of requiredTypeBits with all of eProperties, or -1.
        int32_t FindMemoryType( VkMemoryPropertyFlags eProperties, uint32_t uRequiredTypeBits ) const;
        VkFormatFeatureFlags OptimalTilingFeatures( VkFormat eFormat ) const;
        const VkPhysicalDeviceProperties &Properties() const { return m_Properties; }

        void Log( LogLevel eLevel, const std::string &sLine ) const;

    private:
        CDevice() = default;

        DeviceDesc_t m_Desc;
        VulkanFunctions_t m_Functions;
        PFN_vkGetPhysicalDeviceFormatProperties m_pfnGetFormatProperties = nullptr;
        VkPhysicalDeviceProperties m_Properties = {};
        VkPhysicalDeviceMemoryProperties m_MemoryProperties = {};
    };
}
