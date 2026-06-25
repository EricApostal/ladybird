/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Bitmap.h>
#include <LibGfx/SharedImageBuffer.h>

#ifdef USE_VULKAN_DMABUF_IMAGES
#    include <libdrm/drm_fourcc.h>
#    include <sys/mman.h>
#endif

#ifdef USE_VULKAN_AHB_IMAGES
#    include <android/hardware_buffer.h>
#    include <cstdlib>
#    include <dlfcn.h>
#    include <sys/mman.h>
#    include <unistd.h>

struct native_handle;
using native_handle_t = native_handle;

extern "C" __attribute__((weak)) int AHardwareBuffer_createFromHandle(AHardwareBuffer_Desc const*, native_handle_t const*, int, AHardwareBuffer**);

#    ifndef AHARDWAREBUFFER_CREATE_FROM_HANDLE_METHOD_CLONE
#        define AHARDWAREBUFFER_CREATE_FROM_HANDLE_METHOD_CLONE 1
#    endif
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

#ifdef AK_OS_MACOS
static constexpr auto shared_image_buffer_format = BitmapFormat::BGRA8888;
static constexpr auto shared_image_buffer_alpha_type = AlphaType::Premultiplied;

static NonnullRefPtr<Bitmap> create_bitmap_from_iosurface(Core::IOSurfaceHandle const& iosurface_handle)
{
    auto size = IntSize(static_cast<int>(iosurface_handle.width()), static_cast<int>(iosurface_handle.height()));
    auto bitmap_handle = Core::IOSurfaceHandle::from_mach_port(iosurface_handle.create_mach_port());
    return MUST(Bitmap::create_wrapper(shared_image_buffer_format, shared_image_buffer_alpha_type, size, iosurface_handle.bytes_per_row(), iosurface_handle.data(), [handle = move(bitmap_handle)] { }));
}

SharedImageBuffer::SharedImageBuffer(Core::IOSurfaceHandle&& iosurface_handle, NonnullRefPtr<Bitmap> bitmap)
    : m_iosurface_handle(move(iosurface_handle))
    , m_bitmap(move(bitmap))
{
}
#else
#    ifdef AK_OS_ANDROID
static constexpr auto shared_image_buffer_format = BitmapFormat::RGBA8888;
#    else
static constexpr auto shared_image_buffer_format = BitmapFormat::BGRA8888;
#    endif
static constexpr auto shared_image_buffer_alpha_type = AlphaType::Premultiplied;
#    ifdef USE_VULKAN_DMABUF_IMAGES
static constexpr auto shared_image_buffer_drm_format = DRM_FORMAT_ARGB8888;

static NonnullRefPtr<Bitmap> create_bitmap_from_linux_dmabuf(LinuxDmaBufHandle const& dmabuf)
{
    VERIFY(dmabuf.bitmap_format == shared_image_buffer_format);
    VERIFY(dmabuf.alpha_type == shared_image_buffer_alpha_type);
    VERIFY(dmabuf.drm_format == shared_image_buffer_drm_format);
    VERIFY(dmabuf.modifier == DRM_FORMAT_MOD_LINEAR);
    auto data_size = Bitmap::size_in_bytes(dmabuf.pitch, dmabuf.size.height());
    auto* data = ::mmap(nullptr, data_size, PROT_READ, MAP_SHARED, dmabuf.file.fd(), 0);
    VERIFY(data != MAP_FAILED);
    return MUST(Bitmap::create_wrapper(dmabuf.bitmap_format, dmabuf.alpha_type, dmabuf.size, dmabuf.pitch, data, [data, data_size] {
        VERIFY(::munmap(data, data_size) == 0);
    }));
}
#    endif

#    ifdef USE_VULKAN_AHB_IMAGES
static AndroidAhbHandle clone_android_ahb_handle(AndroidAhbHandle const& ahb)
{
    int duplicated_fd = dup(ahb.socket_fd.fd());
    VERIFY(duplicated_fd >= 0);

    return AndroidAhbHandle {
        .bitmap_format = ahb.bitmap_format,
        .alpha_type = ahb.alpha_type,
        .size = ahb.size,
        .socket_fd = IPC::File::adopt_fd(duplicated_fd),
    };
}

static NonnullRefPtr<Bitmap> create_bitmap_from_android_ahb(AndroidAhbHandle const& ahb_handle, AHardwareBuffer*& out_ahb)
{
    VERIFY(ahb_handle.bitmap_format == shared_image_buffer_format);
    VERIFY(ahb_handle.alpha_type == shared_image_buffer_alpha_type);

    int result = AHardwareBuffer_recvHandleFromUnixSocket(ahb_handle.socket_fd.fd(), &out_ahb);
    if (result != 0) {
        dbgln("AHardwareBuffer_recvHandleFromUnixSocket failed with {}", result);
    }
    VERIFY(result == 0);

    AHardwareBuffer_Desc desc { };
    AHardwareBuffer_describe(out_ahb, &desc);
    
    void* data = nullptr;
    int lock_result = AHardwareBuffer_lock(out_ahb, AHARDWAREBUFFER_USAGE_CPU_READ_RARELY, -1, nullptr, &data);
    VERIFY(lock_result == 0);
    VERIFY(data != nullptr);

    return MUST(Bitmap::create_wrapper(ahb_handle.bitmap_format, ahb_handle.alpha_type, ahb_handle.size, desc.stride * sizeof(u32), data, nullptr));
}
#    endif

SharedImageBuffer::SharedImageBuffer(NonnullRefPtr<Bitmap> bitmap)
    : m_bitmap(move(bitmap))
{
}

#    ifdef USE_VULKAN_DMABUF_IMAGES
SharedImageBuffer::SharedImageBuffer(NonnullRefPtr<Bitmap> bitmap, LinuxDmaBufHandle&& dmabuf)
    : m_linux_dmabuf_handle(move(dmabuf))
    , m_bitmap(move(bitmap))
{
}
#    endif

#    ifdef USE_VULKAN_AHB_IMAGES
SharedImageBuffer::SharedImageBuffer(NonnullRefPtr<Bitmap> bitmap, AndroidAhbHandle&& ahb_handle, AHardwareBuffer* ahb)
    : m_android_ahb_handle(move(ahb_handle))
    , m_android_ahb(ahb)
    , m_bitmap(move(bitmap))
{
}
#    endif
#endif

SharedImageBuffer SharedImageBuffer::create(IntSize size)
{
#ifdef AK_OS_MACOS
    auto iosurface_handle = Core::IOSurfaceHandle::create(size.width(), size.height());
    auto bitmap = create_bitmap_from_iosurface(iosurface_handle);
    return SharedImageBuffer(move(iosurface_handle), move(bitmap));
#else
    return SharedImageBuffer(MUST(Bitmap::create_shareable(shared_image_buffer_format, shared_image_buffer_alpha_type, size)));
#endif
}

SharedImageBuffer SharedImageBuffer::import_from_shared_image(SharedImage shared_image)
{
#ifdef AK_OS_MACOS
    auto iosurface_handle = Core::IOSurfaceHandle::from_mach_port(shared_image.m_port);
    auto bitmap = create_bitmap_from_iosurface(iosurface_handle);
    return SharedImageBuffer(move(iosurface_handle), move(bitmap));
#else
    return shared_image.m_data.visit(
        [](ShareableBitmap& shareable_bitmap) -> SharedImageBuffer {
            return SharedImageBuffer(*shareable_bitmap.bitmap());
        },
        [](LinuxDmaBufHandle& dmabuf) -> SharedImageBuffer {
#    ifdef USE_VULKAN_DMABUF_IMAGES
            auto bitmap = create_bitmap_from_linux_dmabuf(dmabuf);
            return SharedImageBuffer(move(bitmap), move(dmabuf));
#    else
            (void)dmabuf;
            VERIFY_NOT_REACHED();
#    endif
        }
#    ifdef USE_VULKAN_AHB_IMAGES
        ,
        [](AndroidAhbHandle& ahb_handle) -> SharedImageBuffer {
            AHardwareBuffer* ahb = nullptr;
            auto bitmap = create_bitmap_from_android_ahb(ahb_handle, ahb);
            return SharedImageBuffer(move(bitmap), move(ahb_handle), ahb);
        }
#    endif
    );
#endif
}

SharedImageBuffer::SharedImageBuffer(SharedImageBuffer&& other)
    : m_bitmap(move(other.m_bitmap))
{
#ifdef AK_OS_MACOS
    m_iosurface_handle = move(other.m_iosurface_handle);
#endif
#ifdef USE_VULKAN_DMABUF_IMAGES
    m_linux_dmabuf_handle = move(other.m_linux_dmabuf_handle);
#endif
#ifdef USE_VULKAN_AHB_IMAGES
    m_android_ahb_handle = move(other.m_android_ahb_handle);
    m_android_ahb = exchange(other.m_android_ahb, nullptr);
#endif
}

SharedImageBuffer& SharedImageBuffer::operator=(SharedImageBuffer&& other)
{
    if (this != &other) {
        m_bitmap = move(other.m_bitmap);
#ifdef AK_OS_MACOS
        m_iosurface_handle = move(other.m_iosurface_handle);
#endif
#ifdef USE_VULKAN_DMABUF_IMAGES
        m_linux_dmabuf_handle = move(other.m_linux_dmabuf_handle);
#endif
#ifdef USE_VULKAN_AHB_IMAGES
        m_android_ahb_handle = move(other.m_android_ahb_handle);
        if (m_android_ahb) {
            AHardwareBuffer_unlock(m_android_ahb, nullptr);
            AHardwareBuffer_release(m_android_ahb);
        }
        m_android_ahb = exchange(other.m_android_ahb, nullptr);
#endif
    }
    return *this;
}

SharedImageBuffer::~SharedImageBuffer()
{
#ifdef USE_VULKAN_AHB_IMAGES
    if (m_android_ahb) {
        AHardwareBuffer_unlock(m_android_ahb, nullptr);
        AHardwareBuffer_release(m_android_ahb);
    }
#endif
}

SharedImage SharedImageBuffer::export_shared_image() const
{
#ifdef AK_OS_MACOS
    return SharedImage { m_iosurface_handle.create_mach_port() };
#else
#    ifdef USE_VULKAN_AHB_IMAGES
    if (m_android_ahb_handle.has_value())
        return SharedImage { clone_android_ahb_handle(m_android_ahb_handle.value()) };
#    endif
    return SharedImage { ShareableBitmap { m_bitmap, ShareableBitmap::ConstructWithKnownGoodBitmap } };
#endif
}

}
