/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Root.h>
#include <LibURL/URL.h>
#include <LibWeb/Bindings/ServiceWorker.h>
#include <LibWeb/Bindings/Worker.h>
#include <LibWeb/Forward.h>

namespace Web::ServiceWorker {

// https://w3c.github.io/ServiceWorker/#dfn-service-worker
// This class corresponds to "service worker", not "ServiceWorker"
// FIXME: This should be owned and managed at the user agent level
// FIXME: A lot of the fields for this struct actually need to live in the Agent for the service worker in the WebWorker process
struct ServiceWorkerRecord {
    // https://w3c.github.io/ServiceWorker/#dfn-state
    // A service worker has an associated state, which is one of "parsed", "installing", "installed", "activating", "activated", and "redundant". It is initially "parsed".
    Bindings::ServiceWorkerState state = Bindings::ServiceWorkerState::Parsed;

    // https://w3c.github.io/ServiceWorker/#dfn-script-url
    // A service worker has an associated script url (a URL).
    URL::URL script_url;

    // https://w3c.github.io/ServiceWorker/#dfn-type
    // A service worker has an associated type which is either "classic" or "module". Unless stated otherwise, it is "classic".
    Bindings::WorkerType worker_type = Bindings::WorkerType::Classic;

    // https://w3c.github.io/ServiceWorker/#dfn-classic-scripts-imported-flag
    // A service worker has an associated classic scripts imported flag. It is initially unset.
    bool classic_scripts_imported { false };

    // AD-HOC: Handle to the running agent (WebWorker process proxy) once "Run Service Worker" has succeeded.
    //         Null until then. Held as a GC::Root (rather than GC::Ptr) since ServiceWorkerRecord is a plain
    //         struct, not a JS::Cell, and has no visit_edges() of its own to keep a GC::Ptr reachable.
    GC::Root<ServiceWorkerAgent> agent;

    // FIXME: A lot more fields after this...
};

}
