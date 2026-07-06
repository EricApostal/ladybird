/*
 * Copyright (c) 2026, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Clients.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/ServiceWorker/Clients.h>

namespace Web::ServiceWorker {

GC_DEFINE_ALLOCATOR(Clients);

Clients::Clients(JS::Realm& realm)
    : Bindings::PlatformObject(realm)
{
}

void Clients::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(Clients);
}

// https://w3c.github.io/ServiceWorker/#clients-claim
// FIXME: This does not actually claim any clients (i.e. it does not set any service worker client's active
//        service worker, nor does it notify controller change) - it only satisfies the JS-visible contract
//        of resolving a promise, since Flutter's flutter_service_worker.js calls this unconditionally in its
//        activate handler and expects it to succeed.
GC::Ref<WebIDL::Promise> Clients::claim()
{
    auto& realm = HTML::relevant_realm(*this);
    return WebIDL::create_resolved_promise(realm, JS::js_undefined());
}

}
