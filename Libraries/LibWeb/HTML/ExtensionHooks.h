/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/Function.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <LibURL/Forward.h>
#include <AK/Vector.h>
#include <LibHTTP/Header.h>
#include <LibURL/URL.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWebCommon/Page/PageId.h>

namespace Web::HTML {

// Lets an embedder react to documents coming and going without LibWeb knowing what it does with them. Browser
// extensions use it to run content scripts.
class WEB_API ExtensionHooks {
public:
    virtual ~ExtensionHooks() = default;

    // The document has its window, URL and origin, and has not started parsing yet.
    virtual void document_did_start(DOM::Document&) = 0;

    // The document is being destroyed. Whatever was held on its behalf must be released now.
    virtual void document_will_be_destroyed(DOM::Document&) = 0;

    // The contents of a file of a browser extension, as the browser process read them.
    struct ExtensionResource {
        ByteBuffer data;
        Optional<time_t> modified_time;
    };

    // Whether a document of `requesting_origin` (null when the request has no origin) may load the extension resource
    // that `url` names. An extension's own pages may; a web page may only reach what the extension made web accessible.
    virtual bool allows_extension_resource(URL::URL const& url, URL::Origin const* requesting_origin) = 0;

    // Whether the extension whose pages have `origin` was granted the host of `url`, which lets those pages read
    // from it without CORS.
    virtual bool allows_cross_origin_request(URL::Origin const& origin, URL::URL const& url) = 0;

    // Reads the file that a `moz-extension:` URL names. The browser process does, because this process may not read
    // the extension's files, so the answer arrives later. The callback is given null when the file cannot be read.
    virtual void load_extension_resource(URL::URL const& url, PageId, Function<void(Optional<ExtensionResource>)> callback) = 0;

    // A request that is about to be sent, as an extension's `webRequest` listener sees it.
    struct InterceptedRequest {
        PageId page_id;
        // Unique among the requests in flight in this process.
        String request_id;
        URL::URL url;
        ByteString method;
        // `main_frame`, `sub_frame`, `script`, `xmlhttprequest` and so on.
        String resource_type;
        // The URL of the document that made the request.
        URL::URL document_url;
        bool is_top_level { false };
    };

    // What the extensions decided. A request is neither cancelled nor redirected unless one of them said so.
    struct InterceptVerdict {
        bool cancel { false };
        Optional<URL::URL> redirect_url;
        // Headers to set on the request, replacing any of the same name.
        Vector<HTTP::Header> request_headers;
    };

    // Whether any extension wants to see requests, so that the common case costs nothing.
    virtual bool wants_request_interception() = 0;

    // Asks the browser what its extensions want done with `request`. The answer arrives later, and the request is
    // held until it does.
    virtual void intercept_request(InterceptedRequest, Function<void(InterceptVerdict)>) = 0;

    // The `webRequest` resource type of a request: what it is a request for.
    static String resource_type_for(Fetch::Infrastructure::Request const&);

    // The installed hooks, or null when the embedder installed none.
    static ExtensionHooks* the();
    static void install(ExtensionHooks*);
};

}
