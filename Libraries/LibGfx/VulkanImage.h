/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if defined(USE_VULKAN_DMABUF_IMAGES) || defined(USE_VULKAN_AHB_IMAGES)

#    include <AK/Assertions.h>
#    include <AK/NonnullRefPtr.h>
#    include <AK/RefCounted.h>
#    include <AK/Span.h>
#    include <LibGfx/VulkanContext.h>
#    ifdef USE_VULKAN_DMABUF_IMAGES
#        include <libdrm/drm_fourcc.h>
#    endif
#    ifdef AK_OS_ANDROID
#        include <android/hardware_buffer.h>
#    endif

namespace Gfx {

struct VulkanImage : public RefCounted<VulkanImage> {
    VkImage image { VK_NULL_HANDLE };
    VkDeviceMemory memory { VK_NULL_HANDLE };
    struct {
        VkFormat format;
        VkExtent3D extent;
        VkImageTiling tiling;
        VkImageUsageFlags usage;
        VkSharingMode sharing_mode;
        VkImageLayout layout;
        VkDeviceSize row_pitch; // for tiled images this is some implementation-specific value
#    ifdef USE_VULKAN_DMABUF_IMAGES
        uint64_t modifier { DRM_FORMAT_MOD_INVALID };
#    endif
    } info;
    VulkanContext const& context;

#    ifdef USE_VULKAN_DMABUF_IMAGES
    int get_dma_buf_fd() const;
#    endif
#    ifdef USE_VULKAN_AHB_IMAGES
    AHardwareBuffer* get_ahardware_buffer() const;
#    endif
    void transition_layout(VkImageLayout old_layout, VkImageLayout new_layout);
    VulkanImage(VulkanContext const& context)
        : context(context)
    {
    }
    ~VulkanImage();

#    ifdef USE_VULKAN_AHB_IMAGES
    mutable AHardwareBuffer* m_android_hardware_buffer { nullptr };
#    endif
};

#    ifdef USE_VULKAN_DMABUF_IMAGES
static inline uint32_t vk_format_to_drm_format(VkFormat format)
{
    switch (format) {
    case VK_FORMAT_B8G8R8A8_UNORM:
        return DRM_FORMAT_ARGB8888;
    // add more as needed
    default:
        VERIFY_NOT_REACHED();
        return DRM_FORMAT_INVALID;
    }
}
#    endif

ErrorOr<NonnullRefPtr<VulkanImage>> create_shared_vulkan_image(VulkanContext const& context, uint32_t width, uint32_t height, VkFormat format, ReadonlySpan<uint64_t> modifiers);

}

#endif
