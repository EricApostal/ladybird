/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Bitmap.h>
#include <LibGfx/Size.h>
#include <LibIPC/File.h>
#include <LibIPC/Forward.h>

namespace Gfx {

// An image in an AHardwareBuffer. The buffer itself is a kernel object, so what travels over IPC is a socket holding
// its handle: the receiver reads the buffer out of the socket. Android's counterpart of LinuxDmaBufHandle, for the
// embedders that display page frames straight from the compositor's GPU buffers.
struct AndroidAhbHandle {
    BitmapFormat bitmap_format;
    AlphaType alpha_type;
    IntSize size;
    IPC::File socket_fd;
};

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder&, Gfx::AndroidAhbHandle const&);

template<>
ErrorOr<Gfx::AndroidAhbHandle> decode(Decoder&);

}