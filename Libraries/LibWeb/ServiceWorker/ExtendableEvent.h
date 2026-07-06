/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/Event.h>
#include <LibWeb/Export.h>

namespace Web::ServiceWorker {

// https://w3c.github.io/ServiceWorker/#extendableevent-interface
class WEB_API ExtendableEvent : public DOM::Event {
    WEB_PLATFORM_OBJECT(ExtendableEvent, DOM::Event);
    GC_DECLARE_ALLOCATOR(ExtendableEvent);

public:
    [[nodiscard]] static GC::Ref<ExtendableEvent> create(JS::Realm&, FlyString const& event_name, Bindings::ExtendableEventInit const& = {});
    static WebIDL::ExceptionOr<GC::Ref<ExtendableEvent>> construct_impl(JS::Realm&, FlyString const& event_name, Bindings::ExtendableEventInit const&);

    ExtendableEvent(JS::Realm&, FlyString const& event_name, Bindings::ExtendableEventInit const&);
    virtual ~ExtendableEvent() override;

    WebIDL::ExceptionOr<void> wait_until(JS::Value);

private:
    virtual void initialize(JS::Realm&) override;
};

}
