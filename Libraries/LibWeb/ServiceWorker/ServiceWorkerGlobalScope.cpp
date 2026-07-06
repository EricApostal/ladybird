/*
 * Copyright (c) 2024-2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/ServiceWorkerExposedInterfaces.h>
#include <LibWeb/CookieStore/CookieStore.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/ServiceWorker/Clients.h>
#include <LibWeb/ServiceWorker/EventNames.h>
#include <LibWeb/ServiceWorker/ServiceWorkerGlobalScope.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::ServiceWorker {

GC_DEFINE_ALLOCATOR(ServiceWorkerGlobalScope);

ServiceWorkerGlobalScope::~ServiceWorkerGlobalScope() = default;

ServiceWorkerGlobalScope::ServiceWorkerGlobalScope(JS::Realm& realm, GC::Ref<Web::Page> page)
    : HTML::WorkerGlobalScope(realm, page)
{
    // AD-HOC: This was never set, matching DedicatedWorkerGlobalScope/SharedWorkerGlobalScope - see their
    //         constructors for the same flag.
    m_legacy_platform_object_flags = LegacyPlatformObjectFlags { .has_global_interface_extended_attribute = true };
}

// AD-HOC: This override did not previously exist at all. Since nothing ever constructed a
//         ServiceWorkerGlobalScope before Service Worker execution was implemented, this global object's
//         prototype/exposed-interfaces were never wired up - any property access (including `self`) would
//         fail. This mirrors DedicatedWorkerGlobalScope::initialize_web_interfaces_impl() exactly.
void ServiceWorkerGlobalScope::initialize_web_interfaces_impl()
{
    auto& realm = this->realm();
    Bindings::add_service_worker_exposed_interfaces(*this);

    ServiceWorkerGlobalScopeGlobalMixin::initialize(realm, *this);

    Base::initialize_web_interfaces_impl();
}

void ServiceWorkerGlobalScope::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);

    visitor.visit(m_cookie_store);
    visitor.visit(m_clients);
}

// https://w3c.github.io/ServiceWorker/#dom-serviceworkerglobalscope-clients
GC::Ref<Clients> ServiceWorkerGlobalScope::clients()
{
    auto& realm = this->realm();

    if (!m_clients)
        m_clients = realm.create<Clients>(realm);
    return *m_clients;
}

// https://w3c.github.io/ServiceWorker/#dom-serviceworkerglobalscope-skipwaiting
// FIXME: This does not actually set a "skip waiting" flag consulted by Try Activate (Job.cpp's simplified
//        Activate always runs immediately regardless, since it only ever handles a registration with no
//        prior active worker) - it only satisfies the JS-visible contract of resolving a promise. Flutter's
//        flutter_service_worker.js calls this unconditionally as the first statement of its install handler,
//        so leaving this unimplemented aborted the entire handler before it could reach caches.open(...).
GC::Ref<WebIDL::Promise> ServiceWorkerGlobalScope::skip_waiting()
{
    auto& realm = this->realm();
    return WebIDL::create_resolved_promise(realm, JS::js_undefined());
}

// https://w3c.github.io/ServiceWorker/#dom-serviceworkerglobalscope-oninstall
void ServiceWorkerGlobalScope::set_oninstall(GC::Ptr<WebIDL::CallbackType> value)
{
    set_event_handler_attribute(EventNames::install, value);
}

// https://w3c.github.io/ServiceWorker/#dom-serviceworkerglobalscope-oninstall
GC::Ptr<WebIDL::CallbackType> ServiceWorkerGlobalScope::oninstall()
{
    return event_handler_attribute(EventNames::install);
}

// https://w3c.github.io/ServiceWorker/#dom-serviceworkerglobalscope-onactivate
void ServiceWorkerGlobalScope::set_onactivate(GC::Ptr<WebIDL::CallbackType> value)
{
    set_event_handler_attribute(EventNames::activate, value);
}

// https://w3c.github.io/ServiceWorker/#dom-serviceworkerglobalscope-onactivate
GC::Ptr<WebIDL::CallbackType> ServiceWorkerGlobalScope::onactivate()
{
    return event_handler_attribute(EventNames::activate);
}

// https://w3c.github.io/ServiceWorker/#dom-serviceworkerglobalscope-onfetch
void ServiceWorkerGlobalScope::set_onfetch(GC::Ptr<WebIDL::CallbackType> value)
{
    set_event_handler_attribute(EventNames::fetch, value);
}

// https://w3c.github.io/ServiceWorker/#dom-serviceworkerglobalscope-onfetch
GC::Ptr<WebIDL::CallbackType> ServiceWorkerGlobalScope::onfetch()
{
    return event_handler_attribute(EventNames::fetch);
}

// https://w3c.github.io/ServiceWorker/#dom-serviceworkerglobalscope-onmessage
void ServiceWorkerGlobalScope::set_onmessage(GC::Ptr<WebIDL::CallbackType> value)
{
    set_event_handler_attribute(EventNames::message, value);
}

// https://w3c.github.io/ServiceWorker/#dom-serviceworkerglobalscope-onmessage
GC::Ptr<WebIDL::CallbackType> ServiceWorkerGlobalScope::onmessage()
{
    return event_handler_attribute(EventNames::message);
}

// https://w3c.github.io/ServiceWorker/#dom-serviceworkerglobalscope-onmessageerror
void ServiceWorkerGlobalScope::set_onmessageerror(GC::Ptr<WebIDL::CallbackType> value)
{
    set_event_handler_attribute(EventNames::messageerror, value);
}

// https://w3c.github.io/ServiceWorker/#dom-serviceworkerglobalscope-onmessageerror
GC::Ptr<WebIDL::CallbackType> ServiceWorkerGlobalScope::onmessageerror()
{
    return event_handler_attribute(EventNames::messageerror);
}

// https://cookiestore.spec.whatwg.org/#ServiceWorkerGlobalScope
GC::Ref<CookieStore::CookieStore> ServiceWorkerGlobalScope::cookie_store()
{
    auto& realm = this->realm();

    // The cookieStore getter steps are to return this’s associated CookieStore.
    if (!m_cookie_store)
        m_cookie_store = realm.create<CookieStore::CookieStore>(realm, page()->client());
    return *m_cookie_store;
}

}
