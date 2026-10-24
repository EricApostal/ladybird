/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/SharedImage.h>
#include <LibIPC/Attachment.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#ifdef USE_VULKAN_DMABUF_IMAGES
#    include <LibGfx/VulkanImage.h>
#endif
#ifdef USE_VULKAN_AHB_IMAGES
#    include <LibGfx/VulkanImage.h>
#    include <sys/socket.h>
#    include <unistd.h>
#endif
#ifdef USE_DIRECTX
#    include <LibGfx/D3DSharedTexture.h>
#endif

namespace Gfx {

#ifdef AK_OS_MACOS
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

SharedImage::SharedImage(WindowsD3DHandle&& d3d_handle)
    : m_data(move(d3d_handle))
{
}

#    ifdef USE_VULKAN_AHB_IMAGES
SharedImage::SharedImage(AndroidAhbHandle&& ahb_handle)
    : m_data(move(ahb_handle))
{
}
#    endif

#    ifdef USE_VULKAN_DMABUF_IMAGES
SharedImage duplicate_shared_image(VulkanImage const& vulkan_image)
{
    return SharedImage { duplicate_linux_dmabuf_handle(vulkan_image) };
}

LinuxDmaBufHandle duplicate_linux_dmabuf_handle(VulkanImage const& vulkan_image)
{
    VERIFY(vulkan_image.info.format == VK_FORMAT_B8G8R8A8_UNORM);
    auto fd = vulkan_image.get_dma_buf_fd();
    VERIFY(fd >= 0);
    LinuxDmaBufHandle dma_buf;
    dma_buf.size = IntSize(static_cast<int>(vulkan_image.info.extent.width), static_cast<int>(vulkan_image.info.extent.height));
    dma_buf.drm_format = vk_format_to_drm_format(vulkan_image.info.format);
    dma_buf.modifier = vulkan_image.info.modifier;
    dma_buf.objects.append(IPC::File::adopt_fd(fd));
    dma_buf.planes.append({ .object_index = 0, .offset = 0, .pitch = static_cast<u32>(vulkan_image.info.row_pitch) });
    return dma_buf;
}
#    endif

#    ifdef USE_VULKAN_AHB_IMAGES
SharedImage duplicate_shared_image(VulkanImage const& vulkan_image)
{
    return SharedImage { duplicate_android_ahb_handle(vulkan_image) };
}

AndroidAhbHandle duplicate_android_ahb_handle(VulkanImage const& vulkan_image)
{
    auto* ahb = vulkan_image.get_ahardware_buffer();
    VERIFY(ahb != nullptr);

    int sockets[2];
    VERIFY(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);

    // Hand the buffer's handle through the socket; the receiving process reads it back out with
    // AHardwareBuffer_recvHandleFromUnixSocket. The buffer stays alive because the sender keeps its own reference.
    VERIFY(AHardwareBuffer_sendHandleToUnixSocket(ahb, sockets[0]) == 0);
    close(sockets[0]);

    auto const& info = vulkan_image.info;
    return AndroidAhbHandle {
        .bitmap_format = BitmapFormat::RGBA8888,
        .alpha_type = AlphaType::Premultiplied,
        .size = IntSize(static_cast<int>(info.extent.width), static_cast<int>(info.extent.height)),
        .socket_fd = IPC::File::adopt_fd(sockets[1]),
    };
}
#    endif

#    ifdef USE_DIRECTX
SharedImage duplicate_shared_image(D3DSharedTexture const& texture)
{
    return SharedImage { duplicate_windows_d3d_handle(texture) };
}

WindowsD3DHandle duplicate_windows_d3d_handle(D3DSharedTexture const& texture)
{
    return WindowsD3DHandle {
        .size = texture.size(),
        .file = MUST(texture.clone_handle()),
    };
}
#    endif
#endif

}

namespace IPC {

#ifndef AK_OS_MACOS
enum class SharedImageBackingType : u8 {
    ShareableBitmap,
    LinuxDmaBuf,
#    ifdef USE_VULKAN_AHB_IMAGES
    AndroidAhb,
#    endif
    WindowsD3DTexture,
};

#    ifdef USE_VULKAN_AHB_IMAGES
template<>
ErrorOr<void> encode(Encoder& encoder, Gfx::AndroidAhbHandle const& ahb_handle)
{
    TRY(encoder.encode(ahb_handle.bitmap_format));
    TRY(encoder.encode(ahb_handle.alpha_type));
    TRY(encoder.encode(ahb_handle.size));
    TRY(encoder.encode(TRY(IPC::File::clone_fd(ahb_handle.socket_fd.fd()))));
    return {};
}

template<>
ErrorOr<Gfx::AndroidAhbHandle> decode(Decoder& decoder)
{
    return Gfx::AndroidAhbHandle {
        .bitmap_format = TRY(decoder.decode<Gfx::BitmapFormat>()),
        .alpha_type = TRY(decoder.decode<Gfx::AlphaType>()),
        .size = TRY(decoder.decode<Gfx::IntSize>()),
        .socket_fd = TRY(decoder.decode<IPC::File>()),
    };
}
#    endif

template<>
ErrorOr<void> encode(Encoder& encoder, Gfx::WindowsD3DHandle const& d3d_handle)
{
    TRY(encoder.encode(d3d_handle.size));
    TRY(encoder.encode(TRY(IPC::File::clone_fd(d3d_handle.file.fd()))));
    return {};
}

template<>
ErrorOr<Gfx::WindowsD3DHandle> decode(Decoder& decoder)
{
    return Gfx::WindowsD3DHandle {
        .size = TRY(decoder.decode<Gfx::IntSize>()),
        .file = TRY(decoder.decode<IPC::File>()),
    };
}
#endif

template<>
ErrorOr<void> encode(Encoder& encoder, Gfx::SharedImage const& shared_image)
{
#ifdef AK_OS_MACOS
    TRY(encoder.append_attachment(Attachment::from_mach_port(TRY(shared_image.m_port.copy_send_right()), Core::MachPort::MessageRight::MoveSend)));
#else
    return shared_image.m_data.visit(
        [&](Gfx::ShareableBitmap const& shareable_bitmap) -> ErrorOr<void> {
            TRY(encoder.encode(SharedImageBackingType::ShareableBitmap));
            TRY(encoder.encode(shareable_bitmap));
            return {};
        },
        [&](Gfx::LinuxDmaBufHandle const& dmabuf) -> ErrorOr<void> {
            TRY(encoder.encode(SharedImageBackingType::LinuxDmaBuf));
            TRY(encoder.encode(dmabuf));
            return {};
        },
#    ifdef USE_VULKAN_AHB_IMAGES
        [&](Gfx::AndroidAhbHandle const& ahb_handle) -> ErrorOr<void> {
            TRY(encoder.encode(SharedImageBackingType::AndroidAhb));
            TRY(encoder.encode(ahb_handle));
            return {};
        },
#    endif
        [&](Gfx::WindowsD3DHandle const& d3d_handle) -> ErrorOr<void> {
            TRY(encoder.encode(SharedImageBackingType::WindowsD3DTexture));
            TRY(encoder.encode(d3d_handle));
            return {};
        });
#endif
    return {};
}

template<>
ErrorOr<Gfx::SharedImage> decode(Decoder& decoder)
{
#ifdef AK_OS_MACOS
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
    case SharedImageBackingType::WindowsD3DTexture:
        return Gfx::SharedImage { TRY(decoder.decode<Gfx::WindowsD3DHandle>()) };
    default:
        VERIFY_NOT_REACHED();
    }
#endif
}

}
