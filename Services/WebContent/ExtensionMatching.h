/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Optional.h>
#include <AK/StringView.h>
#include <LibURL/URL.h>

namespace WebContent {

// A WebExtension match pattern, such as `*://*.example.com/*` or `<all_urls>`.
// https://developer.mozilla.org/docs/Mozilla/Add-ons/WebExtensions/Match_patterns
class MatchPattern {
public:
    static Optional<MatchPattern> parse(StringView source);

    bool matches(URL::URL const&) const;

private:
    MatchPattern() = default;

    bool m_all_urls { false };
    // The `*` scheme, which stands for http, https, ws and wss.
    bool m_any_scheme { false };
    ByteString m_scheme;
    // Null for any host.
    Optional<ByteString> m_host;
    // `*.example.com` also matches `example.com` itself.
    bool m_subdomains { false };
    Optional<u16> m_port;
    ByteString m_path;
};

// `*` matches any run of characters. When `question_mark_matches_one` is set, `?` matches exactly one.
bool glob_matches(StringView pattern, StringView text, bool question_mark_matches_one, bool ignore_case);

}
