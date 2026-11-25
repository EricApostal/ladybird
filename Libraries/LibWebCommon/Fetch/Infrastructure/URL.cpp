/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AnyOf.h>
#include <LibURL/URL.h>
#include <LibWebCommon/Fetch/Infrastructure/URL.h>

namespace Web::Fetch::Infrastructure {

// https://fetch.spec.whatwg.org/#is-local
bool is_local_url(URL::URL const& url)
{
    // A URL is local if its scheme is a local scheme.
    return any_of(LOCAL_SCHEMES, [&](auto scheme) { return url.scheme() == scheme; });
}

// https://fetch.spec.whatwg.org/#fetch-scheme
bool is_fetch_scheme(StringView scheme)
{
    // A fetch scheme is "about", "blob", "data", "file", or an HTTP(S) scheme.
    // AD-HOC: "moz-extension" is one too, or a navigation to a browser extension's page would be handed to an external
    //         handler instead of creating a document.
    if (scheme == "moz-extension"sv)
        return true;
    return any_of(FETCH_SCHEMES, [&](auto fetch_scheme) { return scheme == fetch_scheme; });
}

// https://fetch.spec.whatwg.org/#http-scheme
bool is_http_or_https_scheme(StringView scheme)
{
    // An HTTP(S) scheme is "http" or "https".
    return any_of(HTTP_SCHEMES, [&](auto http_scheme) { return scheme == http_scheme; });
}

}
