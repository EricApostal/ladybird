/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Platform.h>

#if defined(AK_OS_IOS)

#include <LibIPC/TransportBootstrapMach.h>

namespace IPC {

// Single-process iOS: instead of receiving these ports over a real XPC connection from a
// separate process, the "service" runs on its own thread inside the host app's process. The
// thread that spawns that service thread calls set_pending_transport_ports_for_current_thread()
// with the ports it created *before* starting the thread; the new thread's first action is to
// call bootstrap_transport_from_xpc(), which simply reads back the ports handed to it.
//
// This keeps the call sites in SingleServer.h and Services/WebContent/main.cpp completely
// unchanged: as far as they're concerned, they're still "waiting for the host process to send
// the IPC mach ports over XPC".
void set_pending_transport_ports_for_current_thread(TransportBootstrapMachPorts&&);

ErrorOr<TransportBootstrapMachPorts> bootstrap_transport_from_xpc();

}

#endif
