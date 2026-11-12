/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "LadybirdServiceBase.h"
#include <AK/LexicalPath.h>
#include <AK/OwnPtr.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/EventLoop.h>
#include <LibCore/LocalServer.h>
#include <LibCore/System.h>
#include <LibFileSystem/FileSystem.h>
#include <LibIPC/SingleServer.h>
#include <LibTLS/TLSv12.h>
#include <LibWebView/Utilities.h>
#include <RequestServer/CURL.h>
#include <RequestServer/ConnectionFromClient.h>

namespace RequestServer {

extern ByteString g_default_certificate_path;

}

ErrorOr<int> service_main(int ipc_socket)
{

    RequestServer::g_default_certificate_path = ByteString::formatted("{}/cacert.pem", WebView::s_ladybird_resource_root);

    auto& event_loop = Core::EventLoop::initialize_for_current_thread();

    auto socket = TRY(Core::LocalSocket::adopt_fd(ipc_socket));

    TRY(RequestServer::initialize_libcurl());

    // The Android port connects a RequestServer client directly, so this service speaks the data endpoint rather than
    // the control endpoint. The maps it registers itself in outlive the connection, which runs the event loop below.
    RequestServer::ConnectionFromClient::ConnectionMap connections;
    RequestServer::ConnectionFromClient::RequestTransferLeaseMap request_transfer_leases;
    Optional<HTTP::DiskCache&> disk_cache;
    [[maybe_unused]] auto client = RequestServer::ConnectionFromClient::construct(
        make<IPC::Transport>(move(socket)),
        RequestServer::IsPrivate::No,
        RequestServer::SiteBinding::Unrestricted,
        connections,
        request_transfer_leases,
        disk_cache);

    return event_loop.exec();
}
