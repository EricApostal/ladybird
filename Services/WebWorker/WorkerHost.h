/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefCounted.h>
#include <AK/Vector.h>
#include <LibURL/URL.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/Bindings/Worker.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/Scripting/SerializedEnvironmentSettingsObject.h>
#include <LibWeb/HTML/Scripting/WorkerEnvironmentSettingsObject.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/HTML/WorkerGlobalScope.h>

namespace WebWorker {

class WorkerHost : public RefCounted<WorkerHost> {
public:
    explicit WorkerHost(URL::URL url, Web::Bindings::WorkerType type, String name);
    ~WorkerHost();

    void run(GC::Ref<Web::Page>, Web::HTML::TransferDataEncoder message_port_data, Web::HTML::SerializedEnvironmentSettingsObject const&, Web::Bindings::RequestCredentials, bool is_shared);
    void connect_shared_worker(Web::HTML::TransferDataEncoder message_port_data, Web::HTML::SerializedEnvironmentSettingsObject);

    // https://w3c.github.io/ServiceWorker/#run-service-worker-algorithm
    // NOTE: Unlike the spec, this performs its own script fetch rather than reusing the script resource
    //       already fetched by the Update algorithm - see the AD-HOC comment on this function's definition.
    void run_service_worker(GC::Ref<Web::Page>, URL::URL script_url, Web::Bindings::WorkerType, Web::HTML::SerializedEnvironmentSettingsObject const&);

    // Dispatches an ExtendableEvent (install/activate) at the running ServiceWorkerGlobalScope. Returns whether
    // the event completed without an uncaught exception.
    // FIXME: Does not implement waitUntil()'s lifetime-extension semantics; "completed" just means the event's
    //        listeners ran to completion synchronously.
    bool dispatch_extendable_event(FlyString const& event_name);

private:
    struct PendingSharedWorkerConnection {
        Web::HTML::TransferDataEncoder message_port_data;
        Web::HTML::SerializedEnvironmentSettingsObject outside_settings;
    };

    enum class ShouldAppendOwner {
        No,
        Yes,
    };

    void connect_shared_worker_impl(Web::HTML::TransferDataEncoder message_port_data, Web::HTML::SerializedEnvironmentSettingsObject const&, ShouldAppendOwner);
    void flush_pending_shared_worker_connections();

    GC::Root<Web::HTML::WorkerDebugConsoleClient> m_console;
    GC::Root<Web::HTML::WorkerGlobalScope> m_worker_global_scope;
    GC::Root<Web::HTML::WorkerEnvironmentSettingsObject> m_inside_settings;

    URL::URL m_url;
    Web::Bindings::WorkerType m_type;
    String m_name;
    bool m_is_shared { false };
    bool m_accepting_shared_worker_connections { false };
    // WorkerHost is only touched on the WebWorker main thread.
    Vector<PendingSharedWorkerConnection> m_pending_shared_worker_connections;
};

}
