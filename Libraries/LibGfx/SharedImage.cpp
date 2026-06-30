/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/SharedImage.h>
#include <LibIPC/Attachment.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#if defined(USE_VULKAN_DMABUF_IMAGES) || defined(USE_VULKAN_AHB_IMAGES)
#    include <LibGfx/VulkanImage.h>
#endif

#ifdef USE_VULKAN_AHB_IMAGES
#    include <android/hardware_buffer.h>
#    include <sys/socket.h>
#    include <unistd.h>

struct native_handle;
using native_handle_t = native_handle;

extern "C" __attribute__((weak)) native_handle_t const* AHardwareBuffer_getNativeHandle(AHardwareBuffer const*);
#endif

#if defined(AK_OS_MACOS) || defined(AK_OS_IOS) || defined(AK_OS_IOS)
static Core::MachPort copy_send_right(Core::MachPort const& port)
{
    auto result = mach_port_mod_refs(mach_task_self(), port.port(), MACH_PORT_RIGHT_SEND, +1);
    VERIFY(result == KERN_SUCCESS);
    return Core::MachPort::adopt_right(port.port(), Core::MachPort::PortRight::Send);
}
#endif

namespace Gfx {

#ifdef USE_VULKAN_AHB_IMAGES
struct NativeHandleLayout {
    int version;
    int num_fds;
    int num_ints;
    int data[0];
};
#endif

#if defined(USE_VULKAN_DMABUF_IMAGES) || defined(USE_VULKAN_AHB_IMAGES)
#    ifdef AK_OS_ANDROID
static constexpr auto shared_image_bitmap_format = BitmapFormat::RGBA8888;
#    else
static constexpr auto shared_image_bitmap_format = BitmapFormat::BGRA8888;
#    endif
static constexpr auto shared_image_alpha_type = AlphaType::Premultiplied;
#endif

#if defined(AK_OS_MACOS) || defined(AK_OS_IOS) || defined(AK_OS_IOS)
SharedImage::SharedImage(Core::MachPort&& port)
    : m_port(move(port))
{
}
#else
SharedImage::SharedImage(ShareableBitmap shareable_bitmap)
    : m_data(move(shareable_bitmap))
{
}

SharedImage::SharedImage(LinuxDmaBufHandle&& dmabuf)
    : m_data(move(dmabuf))
{
}

#    ifdef USE_VULKAN_AHB_IMAGES
SharedImage::SharedImage(AndroidAhbHandle&& ahb)
    : m_data(move(ahb))
{
}
#    endif

SharedImage duplicate_shared_image(VulkanImage const& vulkan_image)
{
#    ifdef USE_VULKAN_AHB_IMAGES
    return SharedImage { duplicate_android_ahb_handle(vulkan_image) };
#    else
    return SharedImage { duplicate_linux_dmabuf_handle(vulkan_image) };
#    endif
}

#    ifdef USE_VULKAN_DMABUF_IMAGES
LinuxDmaBufHandle duplicate_linux_dmabuf_handle(VulkanImage const& vulkan_image)
{
    VERIFY(vulkan_image.info.format == VK_FORMAT_B8G8R8A8_UNORM || vulkan_image.info.format == VK_FORMAT_R8G8B8A8_UNORM);
    auto fd = vulkan_image.get_dma_buf_fd();
    VERIFY(fd >= 0);
    return LinuxDmaBufHandle {
        .bitmap_format = shared_image_bitmap_format,
        .alpha_type = shared_image_alpha_type,
        .size = IntSize(static_cast<int>(vulkan_image.info.extent.width), static_cast<int>(vulkan_image.info.extent.height)),
        .drm_format = vk_format_to_drm_format(vulkan_image.info.format),
        .pitch = static_cast<size_t>(vulkan_image.info.row_pitch),
        .modifier = vulkan_image.info.modifier,
        .file = IPC::File::adopt_fd(fd),
    };
}
#    endif

#    ifdef USE_VULKAN_AHB_IMAGES
AndroidAhbHandle duplicate_android_ahb_handle(VulkanImage const& vulkan_image)
{
    auto* ahb = vulkan_image.get_ahardware_buffer();
    VERIFY(ahb != nullptr);

    int sockets[2];
    VERIFY(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    
    // Send the AHB handle through sockets[0].
    int result = AHardwareBuffer_sendHandleToUnixSocket(ahb, sockets[0]);
    VERIFY(result == 0);
    
    // Close sockets[0], keeping sockets[1] alive to send via IPC.
    close(sockets[0]);

    return AndroidAhbHandle {
        .bitmap_format = shared_image_bitmap_format,
        .alpha_type = shared_image_alpha_type,
        .size = IntSize(static_cast<int>(vulkan_image.info.extent.width), static_cast<int>(vulkan_image.info.extent.height)),
        .socket_fd = IPC::File::adopt_fd(sockets[1]),
    };
}
#    endif
#endif

}

namespace IPC {

#if !defined(AK_OS_MACOS) && !defined(AK_OS_IOS) && !defined(AK_OS_IOS)
enum class SharedImageBackingType : u8 {
    ShareableBitmap,
    LinuxDmaBuf,
#    ifdef USE_VULKAN_AHB_IMAGES
    AndroidAhb,
#    endif
};

template<>
ErrorOr<void> encode(Encoder& encoder, Gfx::LinuxDmaBufHandle const& dmabuf)
{
    TRY(encoder.encode(dmabuf.bitmap_format));
    TRY(encoder.encode(dmabuf.alpha_type));
    TRY(encoder.encode(dmabuf.size));
    TRY(encoder.encode(dmabuf.drm_format));
    TRY(encoder.encode(dmabuf.pitch));
    TRY(encoder.encode(dmabuf.modifier));
    TRY(encoder.encode(TRY(IPC::File::clone_fd(dmabuf.file.fd()))));
    return { };
}

template<>
ErrorOr<Gfx::LinuxDmaBufHandle> decode(Decoder& decoder)
{
    return Gfx::LinuxDmaBufHandle {
        .bitmap_format = TRY(decoder.decode<Gfx::BitmapFormat>()),
        .alpha_type = TRY(decoder.decode<Gfx::AlphaType>()),
        .size = TRY(decoder.decode<Gfx::IntSize>()),
        .drm_format = TRY(decoder.decode<u32>()),
        .pitch = TRY(decoder.decode<size_t>()),
        .modifier = TRY(decoder.decode<u64>()),
        .file = TRY(decoder.decode<IPC::File>()),
    };
}

#    ifdef USE_VULKAN_AHB_IMAGES
template<>
ErrorOr<void> encode(Encoder& encoder, Gfx::AndroidAhbHandle const& ahb)
{
    TRY(encoder.encode(ahb.bitmap_format));
    TRY(encoder.encode(ahb.alpha_type));
    TRY(encoder.encode(ahb.size));
    TRY(encoder.encode(TRY(IPC::File::clone_fd(ahb.socket_fd.fd()))));
    return { };
}

template<>
ErrorOr<Gfx::AndroidAhbHandle> decode(Decoder& decoder)
{
    auto bitmap_format = TRY(decoder.decode<Gfx::BitmapFormat>());
    auto alpha_type = TRY(decoder.decode<Gfx::AlphaType>());
    auto size = TRY(decoder.decode<Gfx::IntSize>());
    auto socket_fd = TRY(decoder.decode<IPC::File>());

    return Gfx::AndroidAhbHandle {
        .bitmap_format = bitmap_format,
        .alpha_type = alpha_type,
        .size = size,
        .socket_fd = move(socket_fd),
    };
}
#    endif
#endif

template<>
ErrorOr<void> encode(Encoder& encoder, Gfx::SharedImage const& shared_image)
{
#if defined(AK_OS_MACOS) || defined(AK_OS_IOS) || defined(AK_OS_IOS)
    TRY(encoder.append_attachment(Attachment::from_mach_port(copy_send_right(shared_image.m_port), Core::MachPort::MessageRight::MoveSend)));
#else
    return shared_image.m_data.visit(
        [&](Gfx::ShareableBitmap const& shareable_bitmap) -> ErrorOr<void> {
            TRY(encoder.encode(SharedImageBackingType::ShareableBitmap));
            TRY(encoder.encode(shareable_bitmap));
            return { };
        },
        [&](Gfx::LinuxDmaBufHandle const& dmabuf) -> ErrorOr<void> {
            TRY(encoder.encode(SharedImageBackingType::LinuxDmaBuf));
            TRY(encoder.encode(dmabuf));
            return { };
#    ifdef USE_VULKAN_AHB_IMAGES
        },
        [&](Gfx::AndroidAhbHandle const& ahb) -> ErrorOr<void> {
            TRY(encoder.encode(SharedImageBackingType::AndroidAhb));
            TRY(encoder.encode(ahb));
            return { };
#    endif
        });
#endif
    return { };
}

template<>
ErrorOr<Gfx::SharedImage> decode(Decoder& decoder)
{
#if defined(AK_OS_MACOS) || defined(AK_OS_IOS) || defined(AK_OS_IOS)
    auto attachment = decoder.attachments().dequeue();
    VERIFY(attachment.message_right() == Core::MachPort::MessageRight::MoveSend);
    return Gfx::SharedImage { attachment.release_mach_port() };
#else
    switch (TRY(decoder.decode<SharedImageBackingType>())) {
    case SharedImageBackingType::ShareableBitmap:
        return Gfx::SharedImage { TRY(decoder.decode<Gfx::ShareableBitmap>()) };
    case SharedImageBackingType::LinuxDmaBuf:
        return Gfx::SharedImage { TRY(decoder.decode<Gfx::LinuxDmaBufHandle>()) };
#    ifdef USE_VULKAN_AHB_IMAGES
    case SharedImageBackingType::AndroidAhb:
        return Gfx::SharedImage { TRY(decoder.decode<Gfx::AndroidAhbHandle>()) };
#    endif
    default:
        VERIFY_NOT_REACHED();
    }
#endif
}

}
