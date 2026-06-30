/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2023, Lucas Chollet <lucas.chollet@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <ImageDecoder/ConnectionFromClient.h>
#include <ImageDecoder/Sandbox.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Process.h>
#if defined(AK_OS_MACOS) || defined(AK_OS_IOS)
#    include <LibIPC/Transport.h>
#    include <LibIPC/TransportBootstrapMach.h>
#endif
#if defined(AK_OS_IOS)
#    include <LibIPC/TransportBootstrapIOS.h>
#elif !defined(AK_OS_WINDOWS)
#    include <LibIPC/SingleServer.h>
#endif
#include <LibMain/Main.h>

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    AK::set_rich_debug_enabled(true);

    Core::ArgsParser args_parser;
    StringView mach_server_name;
    bool wait_for_debugger = false;
    bool disable_sandbox = false;

    args_parser.add_option(mach_server_name, "Mach server name", "mach-server-name", 0, "mach_server_name");
    args_parser.add_option(wait_for_debugger, "Wait for debugger", "wait-for-debugger");
    args_parser.add_option(disable_sandbox, "Disable process sandboxing", "disable-sandbox");
    args_parser.parse(arguments);

    if (wait_for_debugger)
        Core::Process::wait_for_debugger_and_break();

    if (!disable_sandbox)
        TRY(ImageDecoder::apply_sandbox());

    auto& event_loop = Core::EventLoop::initialize_for_current_thread();

#if defined(AK_OS_IOS)
    auto transport_ports = TRY(IPC::bootstrap_transport_from_xpc());
    auto client = ImageDecoder::ConnectionFromClient::construct(
        make<IPC::Transport>(move(transport_ports.receive_right), move(transport_ports.send_right)));
#elif defined(AK_OS_MACOS)
    auto browser_port = TRY(Core::MachPort::look_up_from_bootstrap_server(ByteString { mach_server_name }));
    auto transport_ports = TRY(IPC::bootstrap_transport_from_server_port(browser_port));
    auto client = ImageDecoder::ConnectionFromClient::construct(
        make<IPC::Transport>(move(transport_ports.receive_right), move(transport_ports.send_right)));
#else
    auto client = TRY(IPC::take_over_accepted_client_from_system_server<ImageDecoder::ConnectionFromClient>(mach_server_name));
#endif

    return event_loop.exec();
}
