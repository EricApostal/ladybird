/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Platform.h>

#if defined(AK_OS_IOS)

#include <LibIPC/TransportBootstrapIOS.h>

namespace IPC {

// Single-process iOS: each "service" runs on its own thread inside the host app's process
// rather than as a separate process reached over a real XPC connection (BrowserEngineKit's
// extension points require entitlements that are not obtainable outside Apple's approval
// program). The thread that spawns a service thread (see WebView::spawn_browser_engine_process
// in LibWebView/ProcessIOS.mm) creates the mach ports for that service and, immediately before
// starting the thread, stashes them here on the new thread's behalf via
// set_pending_transport_ports_for_current_thread(). The service thread's first action is
// always to call bootstrap_transport_from_xpc(), which just reads the slot back out.
//
// This is thread_local (rather than e.g. a map keyed by thread id) because exactly one service
// thread reads exactly one set of ports, once, immediately after they're published, and no two
// service threads ever share a thread_local instance.
namespace {
thread_local Optional<TransportBootstrapMachPorts> s_pending_transport_ports;
}

void set_pending_transport_ports_for_current_thread(TransportBootstrapMachPorts&& ports)
{
    s_pending_transport_ports = move(ports);
}

ErrorOr<TransportBootstrapMachPorts> bootstrap_transport_from_xpc()
{
    if (!s_pending_transport_ports.has_value())
        return Error::from_string_literal("No transport ports were published for this service thread before it started");

    return s_pending_transport_ports.release_value();
}

}

#endif
