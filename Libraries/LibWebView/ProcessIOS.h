/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Platform.h>

#if defined(AK_OS_IOS)

#include <LibCore/Process.h>
#include <LibIPC/TransportBootstrapMach.h>
#include <LibWebView/ProcessType.h>

namespace WebView {

// Single-process iOS: there is no separate process to spawn, so this starts the requested
// service on its own background thread inside the current process instead. `options` is only
// used to recover argv (the caller's `Core::ProcessSpawnOptions::arguments` reference does not
// outlive this call, so its contents are copied before the service thread starts).
ErrorOr<Core::Process> spawn_browser_engine_process(ProcessType, IPC::TransportBootstrapMachPorts&&, Core::ProcessSpawnOptions const& options);

}

#endif
