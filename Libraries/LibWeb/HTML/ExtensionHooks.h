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
#include <AK/Utf16View.h>
#include <AK/Vector.h>
#include <LibGC/Function.h>
#include <LibHTTP/Header.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/Completion.h>
#include <LibURL/Forward.h>
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

    // The document got its root element. This is when a browser extension's document_start scripts run, before the
    // parser adds anything to it and so before any script of the page's.
    virtual void document_element_inserted(DOM::Document&) = 0;

    // The uuid of the extension whose content script world `realm` is, if it is one.
    virtual Optional<String> content_script_extension(JS::Realm& realm) = 0;

    // The same, of the code that is running, if any is. What a page's policies forbid its own scripts, a content script
    // may do, as in Firefox.
    static Optional<String> content_script_extension_running();

    // Whether `request` is a content script reading a file of its own extension, which it may whether or not the file
    // is web accessible.
    static bool is_content_script_reading_its_own_extension(Fetch::Infrastructure::Request const&);

    // Whether a document of `requesting_origin` (null when the request has no origin) may load the extension resource
    // that `url` names. An extension's own pages may; a web page may only reach what the extension made web accessible.
    virtual bool allows_extension_resource(URL::URL const& url, URL::Origin const* requesting_origin) = 0;

    // Whether the extension whose pages have `origin` was granted the host of `url`, which lets those pages read
    // from it without CORS.
    virtual bool allows_cross_origin_request(URL::Origin const& origin, URL::URL const& url) = 0;

    // Reads the file that a `moz-extension:` URL names. The browser process does, because this process may not read
    // the extension's files, so the answer arrives later. The callback is given null when the file cannot be read.
    virtual void load_extension_resource(URL::URL const& url, PageId, Function<void(Optional<ExtensionResource>)> callback) = 0;

    // Called with an imported module, or with what to reject the import() with.
    using ModuleLoadCompletion = GC::Function<void(JS::ThrowCompletionOr<GC::Ref<JS::Module>>)>;

    // An import() made in `realm`. If that is the world of an extension's content scripts, this takes the import over
    // and returns true, calling `on_loaded` now or later. Such a world imports files of its extension, as modules of
    // its own, which the loading of a page's modules knows nothing about.
    virtual bool load_extension_module(JS::Realm&, Utf16View specifier, GC::Ref<ModuleLoadCompletion> on_loaded) = 0;

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
        // Set when the response's headers have arrived, which is when extensions see them.
        Optional<u32> status_code {};
        Vector<HTTP::Header> response_headers {};
    };

    // What the extensions decided. A request is neither cancelled nor redirected unless one of them said so.
    struct InterceptVerdict {
        bool cancel { false };
        Optional<URL::URL> redirect_url;
        // Headers to set on the request, replacing any of the same name.
        Vector<HTTP::Header> request_headers;
        // All the headers the response is to have, in place of those it came with.
        Optional<Vector<HTTP::Header>> response_headers {};
    };

    // Whether any extension wants to see requests, so that the common case costs nothing.
    virtual bool wants_request_interception() = 0;

    // The same for the headers of responses.
    virtual bool wants_response_interception() = 0;

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
