/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <ImageDecoder/Sandbox.h>
#include <LibSandbox/Sandbox.h>

namespace ImageDecoder {

ErrorOr<void> apply_sandbox()
{
    TRY(Sandbox::configure_runtime());

#if defined(AK_OS_IOS)
    // Seatbelt (LibSandbox's AK_OS_MACOS-gated add_seatbelt_path_if_exists/apply_macos_sandbox)
    // doesn't exist on iOS; sandboxing there is done via App Sandbox entitlements instead.
    return {};
#else
    Vector<Sandbox::SeatbeltPath> paths;
    return Sandbox::apply_macos_sandbox(paths.span(), Sandbox::NetworkAccess::Denied);
#endif
}

}
