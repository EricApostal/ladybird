/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Noncopyable.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Forward.h>
#include <LibGfx/ShareableBitmap.h>
#include <LibIPC/File.h>
#include <LibIPC/Forward.h>

#if defined(AK_OS_MACOS) || defined(AK_OS_IOS) || defined(AK_OS_IOS)
#    include <LibCore/MachPort.h>
#endif

namespace Gfx {

class SharedImageBuffer;

#if !defined(AK_OS_MACOS) && !defined(AK_OS_IOS) && !defined(AK_OS_IOS)
struct LinuxDmaBufHandle {
    BitmapFormat bitmap_format;
    AlphaType alpha_type;
    IntSize size;
    u32 drm_format;
    size_t pitch;
    u64 modifier;
    IPC::File file;
};
#endif

#ifdef USE_VULKAN_AHB_IMAGES
struct AndroidAhbHandle {
    BitmapFormat bitmap_format;
    AlphaType alpha_type;
    IntSize size;
    IPC::File socket_fd;
};
#endif

#if defined(USE_VULKAN_DMABUF_IMAGES) || defined(USE_VULKAN_AHB_IMAGES)
struct VulkanImage;
SharedImage duplicate_shared_image(VulkanImage const&);
#    ifdef USE_VULKAN_DMABUF_IMAGES
LinuxDmaBufHandle duplicate_linux_dmabuf_handle(VulkanImage const&);
#    endif
#    ifdef USE_VULKAN_AHB_IMAGES
AndroidAhbHandle duplicate_android_ahb_handle(VulkanImage const&);
#    endif
#endif

class SharedImage {
    AK_MAKE_NONCOPYABLE(SharedImage);

public:
    SharedImage(SharedImage&&) = default;
    SharedImage& operator=(SharedImage&&) = default;
    ~SharedImage() = default;

#if defined(AK_OS_MACOS) || defined(AK_OS_IOS) || defined(AK_OS_IOS)
    explicit SharedImage(Core::MachPort&&);
#else
    explicit SharedImage(ShareableBitmap);
    explicit SharedImage(LinuxDmaBufHandle&&);
#    ifdef USE_VULKAN_AHB_IMAGES
    explicit SharedImage(AndroidAhbHandle&&);
#    endif
#endif

private:
#if defined(AK_OS_MACOS) || defined(AK_OS_IOS) || defined(AK_OS_IOS)
    Core::MachPort m_port;
#else
    Variant<ShareableBitmap, LinuxDmaBufHandle
#    ifdef USE_VULKAN_AHB_IMAGES
        ,
        AndroidAhbHandle
#    endif
        >
        m_data;
#endif

    friend class SharedImageBuffer;

    template<typename U>
    friend ErrorOr<void> IPC::encode(IPC::Encoder&, U const&);

    template<typename U>
    friend ErrorOr<U> IPC::decode(IPC::Decoder&);
};

}

namespace IPC {

#if !defined(AK_OS_MACOS) && !defined(AK_OS_IOS) && !defined(AK_OS_IOS)
template<>
ErrorOr<void> encode(Encoder&, Gfx::LinuxDmaBufHandle const&);

template<>
ErrorOr<Gfx::LinuxDmaBufHandle> decode(Decoder&);

#    ifdef USE_VULKAN_AHB_IMAGES
template<>
ErrorOr<void> encode(Encoder&, Gfx::AndroidAhbHandle const&);

template<>
ErrorOr<Gfx::AndroidAhbHandle> decode(Decoder&);
#    endif
#endif

template<>
ErrorOr<void> encode(Encoder&, Gfx::SharedImage const&);

template<>
ErrorOr<Gfx::SharedImage> decode(Decoder&);

}
