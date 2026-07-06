/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/ExtendableEvent.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/ServiceWorker/ExtendableEvent.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::ServiceWorker {

GC_DEFINE_ALLOCATOR(ExtendableEvent);

GC::Ref<ExtendableEvent> ExtendableEvent::create(JS::Realm& realm, FlyString const& event_name, Bindings::ExtendableEventInit const& event_init)
{
    return realm.create<ExtendableEvent>(realm, event_name, event_init);
}

WebIDL::ExceptionOr<GC::Ref<ExtendableEvent>> ExtendableEvent::construct_impl(JS::Realm& realm, FlyString const& event_name, Bindings::ExtendableEventInit const& event_init)
{
    return create(realm, event_name, event_init);
}

ExtendableEvent::ExtendableEvent(JS::Realm& realm, FlyString const& event_name, Bindings::ExtendableEventInit const& event_init)
    : DOM::Event(realm, event_name, event_init)
{
}

ExtendableEvent::~ExtendableEvent() = default;

void ExtendableEvent::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(ExtendableEvent);
    Base::initialize(realm);
}

// https://w3c.github.io/ServiceWorker/#wait-until-method
// FIXME: This only implements the "must be called during active dispatch" validation from the Add Lifetime
//        Promise algorithm. It does not track the promise in the event's extend lifetime promises, so callers
//        (e.g. the Install/Activate algorithms) do not currently wait for it to settle before proceeding.
WebIDL::ExceptionOr<void> ExtendableEvent::wait_until(JS::Value)
{
    // 1. If event is not active, throw an "InvalidStateError" DOMException.
    if (!dispatched())
        return WebIDL::InvalidStateError::create(realm(), "waitUntil() called outside of event dispatch"_utf16);

    return {};
}

}
