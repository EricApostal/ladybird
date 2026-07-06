/*
 * Copyright (c) 2026, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::ServiceWorker {

// https://w3c.github.io/ServiceWorker/#clients-interface
class Clients : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(Clients, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(Clients);

public:
    GC::Ref<WebIDL::Promise> claim();

private:
    explicit Clients(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
};

}
