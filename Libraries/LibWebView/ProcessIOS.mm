/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Platform.h>

#if defined(AK_OS_IOS)

#include <LibWebView/ProcessIOS.h>
#include <LibWebView/ProcessManager.h>
#include <LibWebView/Process.h>
#include <LibWebView/Utilities.h>
#include <LibIPC/TransportBootstrapIOS.h>
#include <LibMain/Main.h>
#include <AK/Atomic.h>
#include <AK/ByteString.h>
#include <thread>

// Each service's main.cpp defines `ErrorOr<int> ladybird_main(Main::Arguments)`. All five
// services now run inside this single iOS binary instead of as separate executables.
// The engine (which links these services) registers the function pointers via this struct.
namespace WebView {

static IOSServiceMains s_ios_service_mains;

void register_ios_service_mains(IOSServiceMains mains) {
    s_ios_service_mains = mains;
}

namespace {

// Real pids are always positive. These "services" don't have a real pid since they're just
// detached threads within the host process, so we use negative IDs instead of pids.
pid_t allocate_virtual_pid()
{
    static Atomic<int> s_next_id { 1 };
    return -(s_next_id.fetch_add(1));
}

void run_service_thread(ProcessType type, IPC::TransportBootstrapMachPorts ports, Vector<ByteString> argv_storage)
{
    // This must happen before anything else on this thread: the service's main() reads it back
    // out via bootstrap_transport_from_xpc() during its own startup.
    IPC::set_pending_transport_ports_for_current_thread(move(ports));

    Vector<StringView> string_views;
    string_views.ensure_capacity(argv_storage.size());
    for (auto const& argument : argv_storage)
        string_views.unchecked_append(argument.view());

    Vector<char*> argv;
    argv.ensure_capacity(argv_storage.size());
    for (auto& argument : argv_storage)
        argv.unchecked_append(const_cast<char*>(argument.characters()));

    Main::Arguments arguments {
        .argc = static_cast<int>(argv.size()),
        .argv = argv.data(),
        .strings = string_views.span(),
    };

    auto result = [&]() -> ErrorOr<int> {
        switch (type) {
        case ProcessType::WebContent:
            return s_ios_service_mains.webcontent(arguments);
        case ProcessType::RequestServer:
            return s_ios_service_mains.request_server(arguments);
        case ProcessType::ImageDecoder:
            return s_ios_service_mains.image_decoder(arguments);
        case ProcessType::Compositor:
            return s_ios_service_mains.compositor(arguments);
        case ProcessType::WebWorker:
            return s_ios_service_mains.webworker(arguments);
        case ProcessType::Browser:
            break;
        }
        VERIFY_NOT_REACHED();
    }();

    // Services normally never return (their main()s end in event_loop.exec(), which blocks
    // forever), so reaching this point at all means the service's event loop was torn down or
    // it failed to start.
    if (result.is_error())
        dbgln("[ProcessIOS] {} service thread exited with error: {}", process_name_from_type(type), result.error());
    else
        dbgln("[ProcessIOS] {} service thread exited with code {}", process_name_from_type(type), result.value());
}

}

ErrorOr<Core::Process> spawn_browser_engine_process(ProcessType type, IPC::TransportBootstrapMachPorts&& ports, Core::ProcessSpawnOptions const& options)
{
    // `options.arguments` is a reference into storage owned by the caller's stack frame (see
    // Core::ProcessSpawnOptions); it will not outlive this call, but the service thread we're
    // about to start runs for the lifetime of the app. Copy everything we need out of it now.
    Vector<ByteString> argv_storage;
    argv_storage.ensure_capacity(options.arguments.size() + 1);
    argv_storage.append(ByteString { process_name_from_type(type) });
    for (auto const& argument : options.arguments)
        argv_storage.append(argument);

    std::thread service_thread(run_service_thread, type, move(ports), move(argv_storage));
    service_thread.detach();

    return Core::Process::from_pid(allocate_virtual_pid());
}

}

#endif
