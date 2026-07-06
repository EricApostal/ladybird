/*
 * Copyright (c) 2026, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <LibJS/Heap/Cell.h>
#include <LibURL/URL.h>
#include <LibWeb/Bindings/Worker.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/WorkerAgentTypes.h>
#include <LibWeb/StorageAPI/StorageKey.h>

namespace Web::ServiceWorker {

// A thin proxy that launches (or reuses) the WebWorker process hosting a service worker's global scope, and
// routes command/completion IPC to and from it. Deliberately not modeled on HTML::WorkerAgentParent: that
// class assumes a JS-visible worker object exists to receive DOM `error` events, but a service worker's
// "owner" is a Job/Registration, not an EventTarget - so this class exposes plain completion callbacks
// instead of dispatching events itself.
class WEB_API ServiceWorkerAgent : public JS::Cell {
    GC_CELL(ServiceWorkerAgent, JS::Cell);
    GC_DECLARE_ALLOCATOR(ServiceWorkerAgent);

public:
    static constexpr bool OVERRIDES_FINALIZE = true;

    static GC::Ref<ServiceWorkerAgent> create(GC::Ref<HTML::EnvironmentSettingsObject> client, URL::URL script_url, URL::URL scope_url, Bindings::WorkerType, StorageAPI::StorageKey);

    static void did_finish_loading_worker_script(HTML::WorkerAgentOwnerToken);
    static void did_fail_loading_worker_script(HTML::WorkerAgentOwnerToken);
    static void did_dispatch_extendable_event(HTML::WorkerAgentOwnerToken, String event_name, bool completed_without_error);

    void on_script_loaded(Function<void()> callback) { m_on_script_loaded = move(callback); }
    void on_script_load_failed(Function<void()> callback) { m_on_script_load_failed = move(callback); }

    // Dispatches an ExtendableEvent (install/activate) into the running service worker's global scope and
    // invokes `on_complete` once the WebWorker process reports back.
    // FIXME: Only one dispatch is tracked at a time (a second call before the first completes overwrites
    //        m_on_extendable_event_dispatched and loses the first callback). Note that Job.cpp's Install
    //        callback DOES call Activate - and thus this - synchronously on success, i.e. from inside the
    //        invocation of the previous callback; did_dispatch_extendable_event() specifically moves the
    //        member out before invoking it so that this same-callback reentrant reassignment is safe. A
    //        genuinely *overlapping* second dispatch (unrelated to that chaining) would still lose a callback.
    void dispatch_extendable_event(FlyString const& event_name, Function<void(bool completed_without_error)> on_complete);

protected:
    ServiceWorkerAgent(GC::Ref<HTML::EnvironmentSettingsObject> client, URL::URL script_url, URL::URL scope_url, Bindings::WorkerType, StorageAPI::StorageKey);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;
    virtual void finalize() override;

private:
    GC::Ref<HTML::EnvironmentSettingsObject> m_client;
    URL::URL m_script_url;
    URL::URL m_scope_url;
    Bindings::WorkerType m_worker_type { Bindings::WorkerType::Classic };
    StorageAPI::StorageKey m_storage_key;

    HTML::WorkerAgentId m_agent_id { 0 };
    HTML::WorkerAgentOwnerToken m_owner_token { 0 };

    Function<void()> m_on_script_loaded;
    Function<void()> m_on_script_load_failed;
    Function<void(bool)> m_on_extendable_event_dispatched;
};

}
